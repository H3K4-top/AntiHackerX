#include "class_scanner.h"

#include <QFile>
#include <QFileInfo>
#include <QVector>
#include <QHash>
#include <QStringList>
#include <algorithm>

extern "C" {
#include <unzip.h>
}

namespace {

// ---------------------------------------------------------------------------
// JVM 相关常量
// ---------------------------------------------------------------------------

constexpr quint32 CLASS_MAGIC = 0xCAFEBABE;

// 类访问标志
constexpr quint16 ACC_NATIVE     = 0x0100;
constexpr quint16 ACC_INTERFACE  = 0x0200;
constexpr quint16 ACC_ABSTRACT   = 0x0400;
constexpr quint16 ACC_SYNTHETIC  = 0x1000;
constexpr quint16 ACC_ANNOTATION = 0x2000;
constexpr quint16 ACC_ENUM       = 0x4000;
constexpr quint16 ACC_MODULE     = 0x8000;

/**
 * 疑似第三方库的包名前缀。
 * 这些类通常是被插件打包进来的依赖,不应参与混淆。
 */
const char *const kThirdPartyPrefixes[] = {
    // JDK 自带
    "java/", "javax/", "jdk/", "sun/", "com/sun/", "org/w3c/", "org/xml/",
    // 服务端 / Minecraft 生态
    "org/bukkit/", "org/spigotmc/", "io/papermc/", "com/destroystokyo/",
    "net/minecraft/", "net/minecraftforge/", "net/neoforged/",
    "net/fabricmc/", "org/spongepowered/", "com/velocitypowered/",
    "net/kyori/", "net/md_5/", "org/jetbrains/annotations/",
    // 常见第三方库
    "com/google/", "org/apache/", "org/eclipse/", "org/jetbrains/",
    "kotlin/", "kotlinx/", "scala/", "groovy/",
    "org/slf4j/", "ch/qos/logback/", "org/yaml/", "com/fasterxml/",
    "org/json/", "com/google/gson/", "org/objectweb/", "org/ow2/",
    "io/netty/", "org/sqlite/", "com/zaxxer/", "org/h2/",
    "com/mysql/", "org/mariadb/", "org/postgresql/", "redis/clients/",
    "org/bstats/", "us/myles/", "com/tcoded/", "de/tr7zw/",
    "libs/", "shadow/", "org/bstats/"
};

} // namespace

// ---------------------------------------------------------------------------
// 字节读取器(带边界检查)
// ---------------------------------------------------------------------------

namespace {

class ByteReader {
public:
    explicit ByteReader(const QByteArray &data) : m_data(data), m_pos(0), m_ok(true) {}

    bool ok() const { return m_ok; }
    int remaining() const { return m_data.size() - m_pos; }

    quint8 u1() {
        if (m_pos + 1 > m_data.size()) { m_ok = false; return 0; }
        return static_cast<quint8>(m_data[m_pos++]);
    }

    quint16 u2() {
        if (m_pos + 2 > m_data.size()) { m_ok = false; return 0; }
        quint16 v = static_cast<quint16>(
            (static_cast<quint16>(static_cast<quint8>(m_data[m_pos])) << 8)
            | static_cast<quint8>(m_data[m_pos + 1]));
        m_pos += 2;
        return v;
    }

    quint32 u4() {
        if (m_pos + 4 > m_data.size()) { m_ok = false; return 0; }
        quint32 v = (static_cast<quint32>(static_cast<quint8>(m_data[m_pos])) << 24)
                    | (static_cast<quint32>(static_cast<quint8>(m_data[m_pos + 1])) << 16)
                    | (static_cast<quint32>(static_cast<quint8>(m_data[m_pos + 2])) << 8)
                    | static_cast<quint32>(static_cast<quint8>(m_data[m_pos + 3]));
        m_pos += 4;
        return v;
    }

    QByteArray bytes(int n) {
        if (n < 0) { m_ok = false; return QByteArray(); }
        if (m_pos + n > m_data.size()) {
            m_ok = false;
            n = m_data.size() - m_pos;
        }
        QByteArray b = m_data.mid(m_pos, n);
        m_pos += n;
        return b;
    }

    void skip(int n) {
        if (n < 0 || m_pos + n > m_data.size()) {
            m_ok = false;
            m_pos = m_data.size();
            return;
        }
        m_pos += n;
    }

private:
    const QByteArray &m_data;
    int m_pos;
    bool m_ok;
};

// ---------------------------------------------------------------------------
// class 文件解析结果
// ---------------------------------------------------------------------------

struct ParsedClass {
    quint16 accessFlags = 0;
    quint16 majorVersion = 0;
    int totalMethods = 0;
    int convertibleMethods = 0;
};

/**
 * 跳过成员(字段/方法)的 attributes 区
 */
bool skipAttributes(ByteReader &r) {
    const quint16 count = r.u2();
    if (!r.ok()) return false;
    for (quint16 i = 0; i < count; ++i) {
        r.skip(2);                 // attribute_name_index
        const quint32 len = r.u4(); // attribute_length
        r.skip(static_cast<int>(len));
        if (!r.ok()) return false;
    }
    return true;
}

/**
 * 解析 class 文件,只提取判断 NOBF 可转换性所需的信息。
 *
 * 与 native-obfuscator 的判定保持一致:
 *   可转换方法 = 非 ACC_ABSTRACT、非 ACC_NATIVE、且方法名 != "<init>"
 *   (`<clinit>` 视为可转换,与 MethodProcessor.shouldProcess 一致)
 */
bool parseClassFile(const QByteArray &data, ParsedClass &out, QString &error) {
    if (data.size() < 10) {
        error = QStringLiteral("文件过短");
        return false;
    }

    ByteReader r(data);

    const quint32 magic = r.u4();
    if (!r.ok() || magic != CLASS_MAGIC) {
        error = QStringLiteral("不是合法的 class 文件(魔数错误)");
        return false;
    }

    r.u2();                        // minor_version
    out.majorVersion = r.u2();     // major_version

    // ---- 常量池:只需缓存 CONSTANT_Utf8,用于取方法名 ----
    const quint16 cpCount = r.u2();
    if (!r.ok()) {
        error = QStringLiteral("常量池计数读取失败");
        return false;
    }

    QVector<QString> utf8(cpCount);
    for (quint16 i = 1; i < cpCount; ++i) {
        const quint8 tag = r.u1();
        if (!r.ok()) {
            error = QStringLiteral("常量池读取越界");
            return false;
        }

        switch (tag) {
            case 1: { // CONSTANT_Utf8
                const quint16 len = r.u2();
                utf8[i] = QString::fromUtf8(r.bytes(len));
                break;
            }
            case 3:  // Integer
            case 4:  // Float
                r.skip(4);
                break;
            case 5:  // Long
            case 6:  // Double
                r.skip(8);
                ++i; // 占两个常量池槽位
                break;
            case 7:  // Class
            case 8:  // String
            case 16: // MethodType
            case 19: // Module
            case 20: // Package
                r.skip(2);
                break;
            case 9:  // Fieldref
            case 10: // Methodref
            case 11: // InterfaceMethodref
            case 12: // NameAndType
            case 17: // Dynamic
            case 18: // InvokeDynamic
                r.skip(4);
                break;
            case 15: // MethodHandle
                r.skip(3);
                break;
            default:
                error = QStringLiteral("未知常量池标签 %1").arg(tag);
                return false;
        }

        if (!r.ok()) {
            error = QStringLiteral("常量池读取越界");
            return false;
        }
    }

    // ---- 类头 ----
    out.accessFlags = r.u2();
    r.skip(2); // this_class
    r.skip(2); // super_class

    const quint16 interfaceCount = r.u2();
    r.skip(2 * interfaceCount);

    // ---- 字段表(全部跳过) ----
    const quint16 fieldCount = r.u2();
    if (!r.ok()) {
        error = QStringLiteral("字段表读取失败");
        return false;
    }
    for (quint16 i = 0; i < fieldCount; ++i) {
        r.skip(6); // access_flags + name_index + descriptor_index
        if (!skipAttributes(r)) {
            error = QStringLiteral("字段属性读取越界");
            return false;
        }
    }

    // ---- 方法表:这是判断的关键 ----
    const quint16 methodCount = r.u2();
    if (!r.ok()) {
        error = QStringLiteral("方法表读取失败");
        return false;
    }
    out.totalMethods = methodCount;

    for (quint16 i = 0; i < methodCount; ++i) {
        const quint16 access = r.u2();
        const quint16 nameIndex = r.u2();
        r.u2(); // descriptor_index

        if (!skipAttributes(r)) {
            error = QStringLiteral("方法属性读取越界");
            return false;
        }

        const QString name = (nameIndex < utf8.size()) ? utf8[nameIndex] : QString();

        const bool isAbstract = (access & ACC_ABSTRACT) != 0;
        const bool isNative   = (access & ACC_NATIVE) != 0;

        // 与 MethodProcessor.shouldProcess 完全一致
        if (!isAbstract && !isNative && name != QStringLiteral("<init>")) {
            ++out.convertibleMethods;
        }
    }

    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 第三方库 / 插件根包判定
// ---------------------------------------------------------------------------

namespace {

constexpr int kMinClassesForRootDetection = 8;

/**
 * 是否为重定位(shaded)或元数据目录。
 * shadedJar 打包依赖后会把这些包放到 shaded/ relocations/ 之类的目录下。
 */
bool hasShadingMarker(const QString &lowerPath) {
    static const char *const kMarkers[] = {
        "/shaded/", "/shadow/", "/relocated/", "/relocations/", "/repackaged/"
    };
    for (const char *marker : kMarkers) {
        if (lowerPath.contains(QLatin1String(marker))) {
            return true;
        }
    }
    return lowerPath.startsWith(QLatin1String("shaded/"))
           || lowerPath.startsWith(QLatin1String("shadow/"))
           || lowerPath.startsWith(QLatin1String("relocated/"))
           || lowerPath.startsWith(QLatin1String("META-INF/"));
}

/**
 * 是否命中已知的第三方库包名前缀
 */
bool startsWithKnownLib(const QString &lowerPath) {
    for (const char *prefix : kThirdPartyPrefixes) {
        if (lowerPath.startsWith(QLatin1String(prefix))) {
            return true;
        }
    }
    return false;
}

/**
 * 把 JAR 内条目路径转成全限定类名(小写,点号分隔)
 * ac/grim/grimac/Foo.class -> ac.grim.grimac.foo
 */
QString dottedNameOf(const QString &entryPath) {
    QString dotted = entryPath.toLower();
    if (dotted.endsWith(QLatin1String(".class"))) {
        dotted.chop(6);
    }
    dotted.replace(QLatin1Char('/'), QLatin1Char('.'));
    return dotted;
}

/**
 * 数据驱动地推断插件自身的根包。
 *
 * 思路:统计“既不在重定位目录、也不属于已知库”的类中,
 * 出现次数最多的 3 段包前缀;若其未过半则退回 2 段前缀。
 *
 * 这样处理比“直接拿 plugin.yml 里主类的包”靠得住——
 * 例如主类是 ac.grim.grimac.platform.bukkit.GrimACBukkitLoaderPlugin 时,
 * 真正的插件根包其实是 ac.grim.grimac。
 */
QString detectDominantRootPackage(const QStringList &entryPaths) {
    QHash<QString, int> count3;
    QHash<QString, int> count2;
    int considered = 0;

    for (const QString &entryPath : entryPaths) {
        const QString lower = entryPath.toLower();
        if (hasShadingMarker(lower) || startsWithKnownLib(lower)) {
            continue;
        }

        const QString dotted = dottedNameOf(entryPath);
        const QStringList parts = dotted.split(QLatin1Char('.'));
        const int packageDepth = parts.size() - 1; // 去掉类名本身
        ++considered;

        if (packageDepth >= 3) {
            count3[parts.mid(0, 3).join(QLatin1Char('.'))]++;
        }
        if (packageDepth >= 2) {
            count2[parts.mid(0, 2).join(QLatin1Char('.'))]++;
        }
    }

    if (considered < kMinClassesForRootDetection) {
        return QString();
    }

    auto mostFrequent = [](const QHash<QString, int> &counter) {
        QPair<QString, int> best;
        best.second = 0;
        for (auto it = counter.constBegin(); it != counter.constEnd(); ++it) {
            if (it.value() > best.second) {
                best.first = it.key();
                best.second = it.value();
            }
        }
        return best;
    };

    const auto best3 = mostFrequent(count3);
    if (!best3.first.isEmpty() && best3.second * 2 >= considered) {
        return best3.first;
    }

    const auto best2 = mostFrequent(count2);
    if (!best2.first.isEmpty() && best2.second * 2 >= considered) {
        return best2.first;
    }

    // 没有绝对主流的包 —— 宁可不做根包过滤,避免误伤
    return QString();
}

/**
 * 合并“自动推断的根包”与“外部传入的根包”。
 * 两者互为前缀时取较外层(较短)的那个。
 */
QString mergeRootPackages(const QString &computed, const QString &provided) {
    if (computed.isEmpty()) {
        return provided;
    }
    if (provided.isEmpty()) {
        return computed;
    }

    const QString c = computed.toLower();
    const QString p = provided.toLower();

    if (p == c || p.startsWith(c + QLatin1Char('.'))) {
        return computed;   // 传入的更靠内层,以推断结果为准
    }
    if (c.startsWith(p + QLatin1Char('.'))) {
        return provided;   // 推断结果更靠内层,以传入值为准
    }
    return computed;
}

} // namespace

// ---------------------------------------------------------------------------
// ClassScanner 实现
// ---------------------------------------------------------------------------

QString ClassScanner::classNameFromEntryPath(const QString &entryPath) {
    QString path = entryPath;

    // 多版本 JAR:META-INF/versions/9/com/example/Foo.class -> com/example/Foo
    static const QString kMultiRelease = QStringLiteral("META-INF/versions/");
    if (path.startsWith(kMultiRelease, Qt::CaseInsensitive)) {
        const int slash = path.indexOf(QLatin1Char('/'), kMultiRelease.size());
        if (slash >= 0) {
            path = path.mid(slash + 1);
        }
    }

    if (path.endsWith(QLatin1String(".class"), Qt::CaseInsensitive)) {
        path.chop(6);
    }

    path.replace(QLatin1Char('/'), QLatin1Char('.'));
    return path;
}

bool ClassScanner::isThirdPartyClass(const QString &entryPath,
                                     const QString &pluginRootPackage) {
    const QString lower = entryPath.toLower();

    // 1. shaded / 重定位包:shadedJar 打包依赖后的典型位置
    if (hasShadingMarker(lower)) {
        return true;
    }

    // 2. 已知第三方库包名前缀
    if (startsWithKnownLib(lower)) {
        return true;
    }

    // 3. 不在插件自身根包下 —— 很可能是随包携带的依赖
    //    注意:entryPath 用 '/' 分隔,根包用 '.' 分隔,需先转换为点号形式比较。
    if (!pluginRootPackage.isEmpty()) {
        QString root = pluginRootPackage.toLower();
        if (!root.endsWith(QLatin1Char('.'))) {
            root += QLatin1Char('.');
        }
        if (!dottedNameOf(entryPath).startsWith(root)) {
            return true;
        }
    }

    return false;
}

QString ClassScanner::rootPackageOf(const QString &mainClass) {
    if (mainClass.isEmpty()) {
        return QString();
    }

    QString name = mainClass;
    name.replace(QLatin1Char('/'), QLatin1Char('.'));
    name = name.trimmed();

    const int lastDot = name.lastIndexOf(QLatin1Char('.'));
    if (lastDot <= 0) {
        // 默认包中的类,无法推导根包
        return QString();
    }

    const QString package = name.left(lastDot);

    // 主类常常放在较深的子包里,例如
    //   ac.grim.grimac.platform.bukkit.GrimACBukkitLoaderPlugin
    // 若直接取整个包名,同项目的 ac.grim.grimac.checks.* 会被误判为第三方库。
    // 因此这里最多保留前 3 段作为"项目根包",这是个经验值,使用者可在 UI 中
    // 通过勾选框自行纠正。
    const QStringList parts = package.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    const int keep = qMin(3, parts.size());
    return parts.mid(0, keep).join(QLatin1Char('.'));
}

bool ClassScanner::analyzeClassBytes(const QByteArray &classBytes,
                                     const QString &entryPath,
                                     ScannedClass &out,
                                     const QString &pluginRootPackage) {
    out.entryPath = entryPath;
    out.className = classNameFromEntryPath(entryPath);
    out.thirdParty = isThirdPartyClass(entryPath, pluginRootPackage);

    ParsedClass parsed;
    QString error;
    if (!parseClassFile(classBytes, parsed, error)) {
        out.convertible = false;
        out.reason = QStringLiteral("解析失败:%1").arg(error);
        return false;
    }

    out.majorVersion = parsed.majorVersion;
    out.totalMethods = parsed.totalMethods;
    out.convertibleMethods = parsed.convertibleMethods;

    out.isInterface  = (parsed.accessFlags & ACC_INTERFACE) != 0;
    out.isAbstract   = (parsed.accessFlags & ACC_ABSTRACT) != 0;
    out.isEnum       = (parsed.accessFlags & ACC_ENUM) != 0;
    out.isAnnotation = (parsed.accessFlags & ACC_ANNOTATION) != 0;
    out.isSynthetic  = (parsed.accessFlags & ACC_SYNTHETIC) != 0;
    out.isModuleInfo = (parsed.accessFlags & ACC_MODULE) != 0
                       || out.className == QLatin1String("module-info");

    if (out.isModuleInfo) {
        out.convertible = false;
        out.reason = QStringLiteral("模块描述符(module-info),不可转换");
    } else if (out.convertibleMethods == 0) {
        out.convertible = false;
        if (out.isAnnotation) {
            out.reason = QStringLiteral("注解类型,无可转换方法");
        } else if (out.isInterface) {
            out.reason = QStringLiteral("接口,仅含抽象方法");
        } else if (out.isEnum) {
            out.reason = QStringLiteral("枚举,无可转换方法");
        } else if (out.totalMethods == 0) {
            out.reason = QStringLiteral("类中没有方法");
        } else {
            out.reason = QStringLiteral("方法均为 abstract / native / 构造器");
        }
    } else {
        out.convertible = true;
        out.reason = QStringLiteral("可转换为 C++(%1/%2 个方法)")
                         .arg(out.convertibleMethods)
                         .arg(out.totalMethods);
    }

    return true;
}

ClassScanResult ClassScanner::scan(const QString &jarPath, const QString &pluginRootPackage) {
    ClassScanResult result;

    const QFileInfo fileInfo(jarPath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        result.errorMessage = QStringLiteral("文件不存在或不是普通文件");
        return result;
    }

    unzFile zip = unzOpen(jarPath.toUtf8().constData());
    if (!zip) {
        result.errorMessage = QStringLiteral("无法打开 JAR(不是有效的 ZIP 文件?)");
        return result;
    }

    if (unzGoToFirstFile(zip) != UNZ_OK) {
        unzClose(zip);
        result.errorMessage = QStringLiteral("JAR 内没有任何条目");
        return result;
    }

    // 先收集所有类条目名,用于后面推断插件根包
    QStringList classEntryPaths;

    do {
        // 条目名最长支持 2048 字节,足够覆盖常规 JAR
        char nameBuffer[2048] = {0};
        unz_file_info info;
        if (unzGetCurrentFileInfo(zip, &info, nameBuffer, sizeof(nameBuffer) - 1,
                                  nullptr, 0, nullptr, 0) != UNZ_OK) {
            continue;
        }

        const QString entryPath = QString::fromUtf8(nameBuffer);

        // 目录条目
        if (entryPath.endsWith(QLatin1Char('/'))) {
            continue;
        }
        // 只关心 .class
        if (!entryPath.endsWith(QLatin1String(".class"), Qt::CaseInsensitive)) {
            continue;
        }
        // 多版本 JAR 的备用版本与主版本重名,跳过以免重复统计
        if (entryPath.startsWith(QLatin1String("META-INF/versions/"), Qt::CaseInsensitive)) {
            continue;
        }

        // 读取条目内容
        if (unzOpenCurrentFile(zip) != UNZ_OK) {
            continue;
        }

        QByteArray classBytes;
        classBytes.resize(static_cast<int>(info.uncompressed_size));
        const int bytesRead = unzReadCurrentFile(
            zip, classBytes.data(), static_cast<unsigned int>(classBytes.size()));
        unzCloseCurrentFile(zip);

        if (bytesRead < 0) {
            continue;
        }
        classBytes.resize(bytesRead);

        ScannedClass scanned;
        // 此处不传根包:等收集完所有条目、推断出真正的插件根包后再统一标记
        analyzeClassBytes(classBytes, entryPath, scanned, QString());
        classEntryPaths.append(entryPath);
        result.classes.append(scanned);

    } while (unzGoToNextFile(zip) == UNZ_OK);

    unzClose(zip);

    // ---- 推断插件根包,再统一标记第三方库 ----
    // 单靠 plugin.yml 主类推包并不可靠:主类可能位于子包中
    // (例如 ac.grim.grimac.platform.bukkit.GrimACBukkitLoaderPlugin),
    // 因此改为从实际类分布中统计出主流包前缀。
    result.detectedRootPackage =
        mergeRootPackages(detectDominantRootPackage(classEntryPaths), pluginRootPackage);

    for (ScannedClass &sc : result.classes) {
        sc.thirdParty = isThirdPartyClass(sc.entryPath, result.detectedRootPackage);
    }

    // 按类名排序,便于阅读
    std::sort(result.classes.begin(), result.classes.end(),
              [](const ScannedClass &a, const ScannedClass &b) {
                  return a.className.compare(b.className, Qt::CaseInsensitive) < 0;
              });

    result.isValid = true;
    for (const ScannedClass &sc : result.classes) {
        ++result.totalClasses;
        if (sc.convertible) {
            ++result.convertibleCount;
        }
        if (sc.thirdParty) {
            ++result.thirdPartyCount;
        }
    }

    return result;
}

QList<ScannedClass> ClassScanResult::recommendedClasses() const {
    QList<ScannedClass> recommended;
    for (const ScannedClass &sc : classes) {
        if (sc.convertible && !sc.thirdParty) {
            recommended.append(sc);
        }
    }
    return recommended;
}

QString ClassScanResult::summaryText() const {
    if (!isValid) {
        return QStringLiteral("扫描失败:%1").arg(errorMessage);
    }
    return QStringLiteral("共 %1 个类,其中 %2 个可转换为 C++(另有 %3 个疑似第三方库)")
        .arg(totalClasses)
        .arg(convertibleCount)
        .arg(thirdPartyCount);
}
