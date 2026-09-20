#include "packer_pipeline.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>
#include <QThread>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include "archive.h"
#include <QRandomGenerator>

#include <unzip.h>

namespace {

/** 总步数(用于进度) */
constexpr int kTotalSteps = 11;

/** Windows 版 jni_md.h 内容。
 *  交叉编译到 Windows 时需要它:Linux JDK 只自带 include/linux/jni_md.h。 */
const char *const kWin32JniMd =
        "#ifndef _JAVASOFT_JNI_MD_H_\n"
        "#define _JAVASOFT_JNI_MD_H_\n"
        "\n"
        "#define JNIEXPORT __declspec(dllexport)\n"
        "#define JNIIMPORT __declspec(dllimport)\n"
        "#define JNICALL __stdcall\n"
        "\n"
        "/* long 在 Windows 上是 32 位 */\n"
        "typedef long jint;\n"
        "typedef long long jlong;\n"
        "typedef signed char jbyte;\n"
        "\n"
        "#endif\n";

/** 把内嵌资源释放成磁盘上的文件 */
bool releaseResource(const QString &resourcePath, const QString &targetPath,
                     QString &errorMessage) {
    QFile in(resourcePath);
    if (!in.exists()) {
        errorMessage = QStringLiteral("资源不存在: %1").arg(resourcePath);
        return false;
    }
    if (!in.open(QIODevice::ReadOnly)) {
        errorMessage = QStringLiteral("无法读取资源 %1:%2").arg(resourcePath, in.errorString());
        return false;
    }
    const QByteArray data = in.readAll();
    in.close();

    QDir().mkpath(QFileInfo(targetPath).absolutePath());
    QFile out(targetPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        errorMessage = QStringLiteral("无法写入 %1:%2").arg(targetPath, out.errorString());
        return false;
    }
    if (out.write(data) != data.size()) {
        errorMessage = QStringLiteral("写入 %1 不完整").arg(targetPath);
        return false;
    }
    out.close();
    return true;
}

/** 列出 JAR 里以 suffix 结尾的条目名(默认全部) */
QStringList listJarEntries(const QString &jarPath, const QString &suffix = QString()) {
    QStringList out;
    unzFile zip = unzOpen(jarPath.toUtf8().constData());
    if (!zip) {
        return out;
    }
    if (unzGoToFirstFile(zip) == UNZ_OK) {
        do {
            char nameBuffer[2048] = {0};
            unz_file_info info;
            if (unzGetCurrentFileInfo(zip, &info, nameBuffer, sizeof(nameBuffer) - 1,
                                      nullptr, 0, nullptr, 0) != UNZ_OK) {
                continue;
            }
            const QString entry = QString::fromUtf8(nameBuffer);
            if (entry.endsWith(QLatin1Char('/'))) {
                continue;
            }
            if (suffix.isEmpty() || entry.endsWith(suffix)) {
                out << entry;
            }
        } while (unzGoToNextFile(zip) == UNZ_OK);
    }
    unzClose(zip);
    return out;
}

constexpr quint16 kAccInterface  = 0x0200;
constexpr quint16 kAccAnnotation = 0x2000;
constexpr quint16 kAccEnum       = 0x4000;
constexpr quint16 kAccModule     = 0x8000;

/**
 * 从 class 字节里取 access_flags。
 *
 * <p><b>必须是「跳过常量池之后再读 2 字节」。</b>ClassFile 的结构是</p>
 *
 * <pre>
 *   magic(4) minor(2) major(2) constant_pool_count(2) constant_pool[...] access_flags(2)
 * </pre>
 *
 * <p>access_flags <b>不在固定偏移上</b> —— 在 magic 后第 8 字节处拿到的是
 * constant_pool_count(比如 0x0a27),跳过它当 flags 用会得出完全荒唐的值。</p>
 *
 * <p>解析失败(畸形/未知 tag)返回 0。</p>
 */
quint16 classAccessFlagsOf(const QByteArray &data) {
    const int len = data.size();
    if (len < 10) {
        return 0;
    }
    const unsigned char *b = reinterpret_cast<const unsigned char *>(data.constData());
    if (b[0] != 0xCA || b[1] != 0xFE || b[2] != 0xBA || b[3] != 0xBE) {
        return 0;
    }
    const auto u2 = [b](int p) -> int { return (b[p] << 8) | b[p + 1]; };
    int p = 8;                                  // 跳过 magic/minor/major
    const int cpCount = u2(p);
    p += 2;
    for (int i = 1; i < cpCount; i++) {
        if (p >= len) {
            return 0;
        }
        const int tag = b[p++];
        switch (tag) {
        case 1:                                 // Utf8
            if (p + 2 > len) { return 0; }
            p += 2 + u2(p);
            break;
        case 7: case 8: case 16: case 19: case 20:   // Class/String/MethodType/Module/Package
            p += 2;
            break;
        case 15:                                // MethodHandle
            p += 3;
            break;
        case 3: case 4: case 9: case 10: case 11: case 12:
        case 17: case 18:                       // Integer/Float/Fieldref/Methodref/.../Dynamic
            p += 4;
            break;
        case 5: case 6:                         // Long/Double 各占两个常量槽
            p += 8;
            i++;
            break;
        default:
            return 0;                           // 不认识的 tag:不猜
        }
        if (p > len) {
            return 0;
        }
    }
    if (p + 2 > len) {
        return 0;
    }
    return quint16(u2(p));
}

/**
 * 读出指定类的 access_flags。
 *
 * <p>只读每个 class 的前 8 字节,而且<b>只遍历一次</b> JAR —— 逐个 unzOpen
 * 会慢得离谱(几千个类)。名字用内部名(斜杠分隔);JAR 里没有的不会出现在结果里。</p>
 */
QHash<QString, quint16> readClassAccessFlags(const QString &jarPath,
                                            const QStringList &internalNames) {
    QHash<QString, quint16> out;
    if (internalNames.isEmpty()) {
        return out;
    }
    QHash<QString, bool> wanted;
    for (const QString &name : internalNames) {
        wanted.insert(name, true);
    }
    unzFile zip = unzOpen(jarPath.toUtf8().constData());
    if (!zip) {
        return out;
    }
    if (unzGoToFirstFile(zip) == UNZ_OK) {
        do {
            char nameBuffer[2048] = {0};
            unz_file_info info;
            if (unzGetCurrentFileInfo(zip, &info, nameBuffer, sizeof(nameBuffer) - 1,
                                      nullptr, 0, nullptr, 0) != UNZ_OK) {
                continue;
            }
            const QString entry = QString::fromUtf8(nameBuffer);
            if (!entry.endsWith(QStringLiteral(".class"))) {
                continue;
            }
            const QString internal = entry.left(entry.size() - 6);
            if (!wanted.contains(internal)) {
                continue;
            }
            if (unzOpenCurrentFile(zip) != UNZ_OK) {
                continue;
            }
            // 常量池的长度不固定,只能把整个 class 读出来再跳过去。
            QByteArray data;
            data.resize(int(info.uncompressed_size));
            const int n = unzReadCurrentFile(zip, data.data(), unsigned(info.uncompressed_size));
            unzCloseCurrentFile(zip);
            if (n > 0) {
                data.truncate(n);
                out.insert(internal, classAccessFlagsOf(data));
            }
        } while (unzGoToNextFile(zip) == UNZ_OK);
    }
    unzClose(zip);
    return out;
}

/** 递归复制整棵目录树 */
bool copyTree(const QString &srcDir, const QString &dstDir, QString &errorMessage) {
    QDir src(srcDir);
    if (!src.exists()) {
        errorMessage = QStringLiteral("源目录不存在: %1").arg(srcDir);
        return false;
    }
    if (!QDir().mkpath(dstDir)) {
        errorMessage = QStringLiteral("无法创建目标目录: %1").arg(dstDir);
        return false;
    }
    const QFileInfoList entries = src.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo &entry : entries) {
        const QString dst = QDir(dstDir).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyTree(entry.absoluteFilePath(), dst, errorMessage)) {
                return false;
            }
        } else {
            QFile::remove(dst);
            if (!QFile::copy(entry.absoluteFilePath(), dst)) {
                errorMessage = QStringLiteral("复制失败: %1 -> %2")
                                       .arg(entry.absoluteFilePath(), dst);
                return false;
            }
        }
    }
    return true;
}

/** 在若干候选路径里找第一个存在的文件 */
QString firstExisting(const QStringList &candidates) {
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return QString();
}

/** 递归收集指定后缀的文件 */
QStringList collectBySuffix(const QString &dir, const QStringList &suffixes) {
    QStringList out;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        for (const QString &suffix : suffixes) {
            if (path.endsWith(suffix)) {
                out << path;
                break;
            }
        }
    }
    out.sort();
    return out;
}

/** jar-obfuscator 的字符串加密会把字面量换成
 *  `<decryptClassName>.<decryptMethodName>("密文")`。这个"解密器类"的默认名字是
 *  org.apache.commons.collections.list.AbstractHashMap —— 每次混淆都会生成一份、
 *  且内嵌的 AES 密钥是随机的新密钥。
 *
 *  于是同一趟流水线里混淆两个 JAR 就会撞名:用户 JAR 里一份、验证模块里一份,
 *  同名不同密钥。运行时 AhxClassLoader 是**父优先**,用户类发起的解密调用会被
 *  应用类路径上的模块版解密器接走,拿错密钥解出 null —— 表现为
 *  `System.getProperty(null) -> NullPointerException`,而且字符串越用得多炸得越晚,
 *  极难排查。
 *
 *  所以这里给每次混淆随机分配一个候选名,并保证两次不重复。
 *  候选都取自真实存在的第三方类名,反编译时不会显得突兀。 */
const char *const kDecryptNamePool[] = {
    "org.apache.commons.collections.list.AbstractHashMap",
    "org.apache.commons.collections.map.AbstractHashedMap",
    "org.apache.commons.collections.keyvalue.AbstractMapEntry",
    "org.apache.commons.collections.map.AbstractReferenceMap",
    "org.apache.commons.collections.buffer.AbstractBuffer",
    "org.apache.commons.collections.set.AbstractSetDecorator",
    "org.apache.commons.collections.bag.AbstractMapBag",
    "org.apache.commons.collections.map.AbstractInputCheckedMapDecorator",
};

QString pickDecryptClassName(const QStringList &avoid) {
    QStringList pool;
    for (const char *const candidate : kDecryptNamePool) {
        const QString name = QString::fromLatin1(candidate);
        if (!avoid.contains(name)) {
            pool << name;
        }
    }
    if (pool.isEmpty()) {
        return QStringLiteral("org.apache.commons.collections.list.AbstractHashMap");
    }
    return pool.at(QRandomGenerator::global()->bounded(pool.size()));
}

/** 另一个必须避开的解密器符号名(方法名/密钥字段名)。
 *  这两个名字是固定的,本身不会互相冲突,但同一 JVM 里同名类的同名成员会让人
 *  误以为是同一个类,故一并随机化。 */
QString pickDecryptMethodName() {
    static const char *const pool[] = { "newMap", "map", "lookup", "resolve" };
    return QString::fromLatin1(pool[QRandomGenerator::global()->bounded(4)]);
}

QString pickDecryptKeyName() {
    static const char *const pool[] = { "LiLiLLLiiiLLiiLLi", "llILiLliiLiiLiL", "iLiLliLILiLLiii" };
    return QString::fromLatin1(pool[QRandomGenerator::global()->bounded(3)]);
}

/** 公钥指纹(SHA-256 前 8 字节,X.509 DER 的 hex 字符串作为输入)。
 *
 *  与 {@code AhxSign keygen} 打印的那个算法一致 —— 出错时两边能对得上号,
 *  能直接确认“产物里烧的是哪把钥匙”。 */
QString pubKeyFingerprint(const QString &spkiHex) {
    const QByteArray spki = QByteArray::fromHex(spkiHex.toLatin1());
    const QByteArray digest =
            QCryptographicHash::hash(spki, QCryptographicHash::Sha256);
    QStringList parts;
    for (int i = 0; i < 8 && i < digest.size(); ++i) {
        parts << QStringLiteral("%1").arg(static_cast<quint8>(digest.at(i)), 2, 16,
                                           QLatin1Char('0'));
    }
    return parts.join(QLatin1Char(':'));
}

/** 生成一段纯小写字母的随机标识符。
 *
 *  用作 native 包的目录名 / 加载器类名 / 合成隐藏类名前缀。**必须每份产物都不同**,
 *  因为 NOBF 的加载器与合成隐藏类是被原生库用
 *  {@code DefineClass(..., nullptr, ...)} 定义到 <b>bootstrap</b> 加载器里的 ——
 *  那是整个 JVM 全局的名字空间。两份都用加壳的插件装在一起时,若名字相同
 *  (上游默认是 {@code native0} / {@code Loader} / {@code Hidden0}),第二个就会:
 *
 *  <pre>
 *  LinkageError: loader 'bootstrap' attempted duplicate class definition
 *      for native0.hidden.Hidden0
 *  </pre>
 *
 *  而直接加载失败 —— 这是实测踩过的坑,不是理论担忧。 */
QString randomNativeName() {
    static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz";
    QString s;
    for (int i = 0; i < 8; ++i) {
        s.append(QLatin1Char(kAlphabet[QRandomGenerator::global()->bounded(26)]));
    }
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// 构造 / 默认配置
// ---------------------------------------------------------------------------

PackerPipeline::PackerPipeline(QObject *parent) : QObject(parent) {
}

PackerPipeline::Config PackerPipeline::defaultConfig() {
    Config config;
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString projectRoot = QFileInfo(appDir + "/../..").absoluteFilePath();

    config.javaExe = QStringLiteral("java");
    config.javacExe = QStringLiteral("javac");
    config.jarExe = QStringLiteral("jar");
    // zig 交叉编译需要 JNI 头文件(jni.h / jni_md.h),它们位于 $JAVA_HOME/include
    config.javaHome = qEnvironmentVariable("JAVA_HOME");

    config.obfuscatorJar = firstExisting({
        appDir + "/libs/jar-obfuscator-2.0.1-jar-with-dependencies.jar",
        projectRoot + "/jar-obfuscator/target/jar-obfuscator-2.0.1-jar-with-dependencies.jar",
    });
    config.nobfJar = firstExisting({
        appDir + "/libs/native-obfuscator.jar",
    });
    config.zigExe = firstExisting({
        projectRoot + "/libs/zig/zig",
        appDir + "/libs/zig/zig",
    });
    config.ahxPackerSrc = firstExisting({
        projectRoot + "/AntiHackerXVerify/tools/AhxPacker.java",
    });

    return config;
}

void PackerPipeline::startAsync(const Config &config) {
    if (m_running) {
        emit finished(false, QStringLiteral("流水线已在运行"));
        return;
    }
    m_stop = false;
    m_running = true;

    QThread *thread = QThread::create([this, config]() { run(config); });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void PackerPipeline::requestStop() {
    m_stop = true;
}

void PackerPipeline::abort(const QString &reason) {
    m_stop = true;
    emit log(QStringLiteral("[中止] %1").arg(reason));
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------

void PackerPipeline::run(const Config &config) {
    QElapsedTimer timer;
    timer.start();

    QString error;

    const QString appDir = QCoreApplication::applicationDirPath();
    const QFileInfo inputInfo(config.inputJar);
    const QString buildDir = QDir(appDir).filePath(inputInfo.completeBaseName() + QStringLiteral("+Verify-build"));

    auto fail = [&](const QString &what) {
        m_running = false;
        emit finished(false, QStringLiteral("%1 失败:%2").arg(what, error));
    };

    // 工作目录:纯中间产物,每次重建
    {
        if (QDir(buildDir).exists() && !QDir(buildDir).removeRecursively()) {
            error = QStringLiteral("无法清理构建目录 %1").arg(buildDir);
            fail(QStringLiteral("准备"));
            return;
        }
        if (!QDir().mkpath(buildDir)) {
            error = QStringLiteral("无法创建构建目录 %1").arg(buildDir);
            fail(QStringLiteral("准备"));
            return;
        }
        emit log(QStringLiteral("构建目录: %1").arg(buildDir));
    }

    int step = 0;

    // ---- 0. Fabric:读元数据 + 改写 fabric.mod.json ----
    // 必须在混淆**之前**:混淆要用到“哪些类冻结改名”,而那份名单是从
    // mixins.json 里读出来的。
    QString fabricBridge;
    if (config.fabricMod) {
        fabricBridge = VerifyModule::bootstrapClassNameFor(config.originalMainClass);
        emit log(QStringLiteral("检测到 Fabric MOD,桥类: %1").arg(fabricBridge));
        if (!stepPrepareFabricMod(config, config.inputJar, fabricBridge, error)) {
            fail(QStringLiteral("读取 Fabric 元数据"));
            return;
        }
        emit log(QStringLiteral("mixin 类(明文 + 冻结改名): %1 个")
                         .arg(m_fabricKeepPlain.size()));
    }

    // ---- 1. 混淆用户 JAR ----
    emit staged(++step, kTotalSteps, QStringLiteral("混淆用户 JAR"));
    QString userJar;
    QString mappingFile;
    if (!stepObfuscateUser(config, buildDir, userJar, mappingFile, error)) {
        fail(QStringLiteral("混淆用户 JAR"));
        return;
    }

    // ---- 2. 解析映射,得到用户主类的新名字 ----
    emit staged(++step, kTotalSteps, QStringLiteral("解析改名映射"));
    QString targetClass;
    if (!stepResolveTarget(mappingFile, config.originalMainClass, targetClass, error)) {
        fail(QStringLiteral("解析改名映射"));
        return;
    }
    emit log(QStringLiteral("目标主类(改名后): %1").arg(targetClass));

    // 用户在界面上勾选「要隐藏的类」→ 翻译成混淆后的内部名,供白名单使用。
    const QHash<QString, QString> classMapping = parseMappingFile(mappingFile);
    const QStringList hideInternalNames =
            resolveHideClassNames(config.hideClasses, classMapping);
    if (!config.hideClasses.isEmpty()) {
        emit log(QStringLiteral("选中 %1 个用户类要转原生")
                         .arg(hideInternalNames.size()));
    }

    // ---- 从「要转原生的类」里剔除 NOBF 处理不了的 ----
    // 枚举 / 接口 / 注解 / module-info 一律不转。
    //
    // 枚举是实测踩到的坑:CollisionData 这个枚举被转成 76 个 native 方法后,
    // 它的 <clinit>(javac 生成:创建常量实例、填 $VALUES、调用受保护的
    // Enum.<init>)被搬进原生库;而 native 侧解析类型引用时用的并不是插件自己的
    // PluginClassLoader,于是形如 [L.../StateType; 的数组类型直接
    // NoClassDefFoundError。接口/注解没有可执行方法体,转了只有风险。
    QStringList nativeCandidates = hideInternalNames;
    if (!nativeCandidates.isEmpty()) {
        const QHash<QString, quint16> accessFlags =
                readClassAccessFlags(userJar, nativeCandidates);
        QStringList keep;
        QStringList skipped;
        for (const QString &name : nativeCandidates) {
            const quint16 flags = accessFlags.value(name, 0);
            if (flags & (kAccInterface | kAccAnnotation | kAccEnum | kAccModule)) {
                skipped << name;
            } else {
                keep << name;
            }
        }
        if (!skipped.isEmpty()) {
            emit log(QStringLiteral("剔除 %1 个不适合原生的类(枚举/接口/注解)")
                             .arg(skipped.size()));
            emit log(QStringLiteral("  例如: %1")
                             .arg(skipped.mid(0, 5).join(QStringLiteral(", "))));
        }
        nativeCandidates = keep;
        emit log(QStringLiteral("实际送进 NOBF: %1 个类")
                         .arg(nativeCandidates.size()));
    }

    // 用户给的服务端 / API 依赖。Paper 插件编验证模块时**必须**有它,
    // 否则 import org.bukkit.plugin.java.JavaPlugin 直接编不过。
    const QStringList extraJars = expandJars(config.extraClassPath);
    if (!extraJars.isEmpty()) {
        qint64 total = 0;
        for (const QString &jar : extraJars) {
            total += QFileInfo(jar).size();
        }
        emit log(QStringLiteral("额外类路径: %1 个 jar,共 %2 MB")
                         .arg(extraJars.size())
                         .arg(total / 1024.0 / 1024.0, 0, 'f', 1));
    }

    // 缺省入口类名:由 VerifyModule 依据目标包决定
    const QString entryClass = VerifyModule::bootstrapClassNameFor(targetClass);

    // 载荷里出现过的包。解密出来的类必须由宿主类加载器定义,而
    // Lookup#defineClass 要求同包 —— 所以每个包得先放一个「定义器」进去。
    QStringList payloadPackages;
    {
        const QStringList entries = listJarEntries(userJar, QStringLiteral(".class"));
        for (const QString &entry : entries) {
            // 多版本 JAR 的 META-INF/versions/9/... 不是合法的 Java 包名,
            // 拿它去生成定义器会编不过。这类条目要么被丢掉要么按资源保留,
            // 绝不能按包名处理。
            if (entry.startsWith(QStringLiteral("META-INF/"), Qt::CaseInsensitive)) {
                continue;
            }
            const int slash = entry.lastIndexOf(QLatin1Char('/'));
            const QString pkg = slash < 0 ? QString()
                                          : entry.left(slash).replace(QLatin1Char('/'), QLatin1Char('.'));
            if (!payloadPackages.contains(pkg)) {
                payloadPackages << pkg;
            }
        }
        emit log(QStringLiteral("载荷包: %1 个").arg(payloadPackages.size()));
    }

    // ---- 3. 释放验证模块源码 + 生成入口 ----
    emit staged(++step, kTotalSteps, QStringLiteral("生成加壳入口"));
    //
    // 反篡改是个“先烧公钥、后签名”的闭环:入口类里必须已经带着公钥,而签名要等
    // 产物内容全部确定之后才能算。所以密钥对必须在装配入口**之前**生成。
    // 私钥只留在构建目录,打包成功后即删。
    QString tamperPubKeyHex;
    QString signKeyPath;
    if (config.verifyOptions.antiTamper) {
        if (!stepGenerateSignKey(config, buildDir, tamperPubKeyHex, error)) {
            fail(QStringLiteral("生成签名密钥"));
            return;
        }
        signKeyPath = QDir(buildDir).filePath(QStringLiteral("ahx-sign.key"));
        emit log(QStringLiteral("反篡改:已生成 ECDSA P-256 密钥对,公钥指纹 %1")
                         .arg(pubKeyFingerprint(tamperPubKeyHex)));
    }
    QString moduleWorkDir;
    QString keyHex;
    QStringList definerClasses;
    if (!stepPrepareModule(config, targetClass, payloadPackages, tamperPubKeyHex,
                           moduleWorkDir, keyHex, definerClasses, error)) {
        fail(QStringLiteral("生成加壳入口"));
        return;
    }
    emit log(QStringLiteral("载荷密钥(仅打包器持有): %1...").arg(keyHex.left(16)));

    // ---- 4. 编译验证模块 ----
    emit staged(++step, kTotalSteps, QStringLiteral("编译验证模块"));
    QString moduleJar;
    if (!stepCompileModule(config, moduleWorkDir, targetClass, definerClasses,
                           moduleJar, error)) {
        fail(QStringLiteral("编译验证模块"));
        return;
    }

    // ---- 5. 混淆验证模块(入口类保名) ----
    // 不用再给定义器传黑名单:编译时它们就被摘出了模块 JAR,
    // 混淆器根本看不到它们。
    emit staged(++step, kTotalSteps, QStringLiteral("混淆验证模块"));
    QString moduleObfJar;
    if (!stepObfuscateModule(config, moduleJar, entryClass, QStringList(),
                             moduleObfJar, error)) {
        fail(QStringLiteral("混淆验证模块"));
        return;
    }

    // ---- 6. 转换验证模块为 C++ ----
    //
    // 两份 NOBF 产物的包名 / 加载器类名 / 隐藏类名前缀全都每份随机。
    // 它们会被原生库定义进 bootstrap 加载器(全局名字空间),
    // 名字撞了就 LinkageError —— 见 randomNativeName() 的注释。
    const QString moduleNativePkg = randomNativeName();
    const QString moduleLoaderName = randomNativeName();
    const QString moduleHiddenName = randomNativeName();
    const QString userNativePkg = randomNativeName();
    const QString userLoaderName = randomNativeName();
    const QString userHiddenName = randomNativeName();
    emit staged(++step, kTotalSteps, QStringLiteral("转换验证模块"));
    QString moduleCppDir;
    QString moduleNativeJar;
    if (!stepNativeConvert(config, moduleObfJar,
                           QDir(buildDir).filePath(QStringLiteral("module-native")),
                           moduleNativePkg, moduleLoaderName, moduleHiddenName,
                           QString(), QString(), QStringList(),
                           moduleCppDir, moduleNativeJar, error)) {
        fail(QStringLiteral("转换验证模块"));
        return;
    }

    // NOBF 的 -l 只收目录,把额外的 jar 汇总一份给它。
    // 这是可选的精度优化,失败就算了(算不准栈帧会退化成 Object,不会报错)。
    const QString nobfLibsDir = QDir(buildDir).filePath(QStringLiteral("nobf-libs"));
    if (!extraJars.isEmpty()) {
        const int n = stageNobfLibs(extraJars, nobfLibsDir);
        emit log(QStringLiteral("NOBF 类路径: 已备好 %1 个 jar").arg(n));
    }

    // ---- 7. 把选中的用户类也转成 C++ ----
    //     载荷源随之换成 NOBF 的产物:选中类在那里已经变成 native stub,
    //     未选中的类字节不动。没有勾选就整步跳过,保持旧行为。
    QString userNativeJar = userJar;   // 载荷源(默认就是混淆后的用户 JAR)
    QString userCppDir;
    QString userNativeDir;             // NOBF 工作目录,注入时要从这里取 <pkg>/**
    bool haveUserNative = false;

    emit staged(++step, kTotalSteps, QStringLiteral("转换选中类"));
    if (nativeCandidates.isEmpty()) {
        emit log(QStringLiteral("没有可转换的用户类,跳过(只原生化验证模块)"));
    } else {
        const QString whiteList = buildNativeWhiteList(nativeCandidates);
        if (!writeFile(QDir(buildDir).filePath(QStringLiteral("user-native-whitelist.txt")),
                       whiteList, error)) {
            fail(QStringLiteral("转换选中类"));
            return;
        }
        userNativeDir = QDir(buildDir).filePath(QStringLiteral("user-native"));
        QString convertedJar;
        if (!stepNativeConvert(config, userJar, userNativeDir,
                               userNativePkg, userLoaderName, userHiddenName,
                               QDir(buildDir).filePath(QStringLiteral("user-native-whitelist.txt")),
                               extraJars.isEmpty() ? QString() : nobfLibsDir,
                               QStringList(),
                               userCppDir, convertedJar, error)) {
            fail(QStringLiteral("转换选中类"));
            return;
        }
        userNativeJar = convertedJar;
        haveUserNative = true;
    }

    // ---- 8. 交叉编译原生库 ----
    emit staged(++step, kTotalSteps, QStringLiteral("交叉编译原生库"));
    if (!stepBuildNative(config, moduleCppDir, error)) {
        fail(QStringLiteral("交叉编译原生库"));
        return;
    }
    if (haveUserNative && !stepBuildNative(config, userCppDir, error)) {
        fail(QStringLiteral("交叉编译原生库"));
        return;
    }

    // ---- 9. 修补 Loader ----
    emit staged(++step, kTotalSteps, QStringLiteral("回填原生库"));
    if (!stepPatchLoader(config, QFileInfo(moduleNativeJar).absolutePath(), error)) {
        fail(QStringLiteral("回填原生库"));
        return;
    }
    if (haveUserNative
        && !stepPatchLoader(config, QFileInfo(userNativeJar).absolutePath(), error)) {
        fail(QStringLiteral("回填原生库"));
        return;
    }

    // ---- 10. 组装注入目录并合并 ----
    emit staged(++step, kTotalSteps, QStringLiteral("加密与合并"));
    const QString injectDir = QDir(buildDir).filePath(QStringLiteral("inject"));
    QDir().mkpath(injectDir);

    // 验证模块那一半:<moduleNativePkg>/** 与模块的 native stub 一起明文注入。
    if (!runTool(config.jarExe,
                 QStringList() << QStringLiteral("xf") << moduleNativeJar,
                 injectDir, error)) {
        fail(QStringLiteral("解包验证模块"));
        return;
    }
    // 选中用户类那一半:**只**注入 <userNativePkg>/**,那些类的 stub 本身要留在载荷里。
    // 若把 NOBF 产出整个解出来,未选中的用户类会明文落到应用类路径上 ——
    // 既破坏了加密,也会让「载荷里的同名类」和「类路径上的明文类」打架。
    if (haveUserNative) {
        const QString tmpDir = QDir(buildDir).filePath(QStringLiteral("user-native-extract"));
        QDir().mkpath(tmpDir);
        if (!runTool(config.jarExe,
                     QStringList() << QStringLiteral("xf") << userNativeJar,
                     tmpDir, error)) {
            fail(QStringLiteral("解包用户原生类"));
            return;
        }
        const QString src = QDir(tmpDir).filePath(userNativePkg);
        const QString dst = QDir(injectDir).filePath(userNativePkg);
        if (!QDir(src).exists() || !copyTree(src, dst, error)) {
            error = error.isEmpty()
                    ? QStringLiteral("NOBF 产物里没有 %1 目录:%2").arg(userNativePkg, src)
                    : error;
            fail(QStringLiteral("加密与合并"));
            return;
        }
        emit log(QStringLiteral("已注入 %1/(用户原生类的加载器)").arg(userNativePkg));
    }

    // 补原生库:NOBF 的 Loader 用 getResourceAsStream("/nativeN/x64-*.so") 从 JAR 里取,
    // 而那个 JAR 是在编译**之前**由 NOBF 生成的,不可能含刚编出来的库。
    if (!placeNativeLibs(moduleCppDir, QDir(injectDir).filePath(moduleNativePkg), error)) {
        fail(QStringLiteral("加密与合并"));
        return;
    }
    if (haveUserNative
        && !placeNativeLibs(userCppDir, QDir(injectDir).filePath(userNativePkg), error)) {
        fail(QStringLiteral("加密与合并"));
        return;
    }

    // 定义器补回明文区。它们在编译后就被摘出了模块 JAR(见 stepCompileModule),
    // 所以既没被 jar-obfuscator 改包名,也没被 NOBF 转走 —— 这里原样注入。
    // 入口类的 LINK 数组 + 宿主加载器就是靠这批类把载荷定义进宿主加载器的。
    const QString definerDir = QDir(moduleWorkDir).filePath(QStringLiteral("_definers"));
    if (QDir(definerDir).exists()) {
        if (!copyTree(definerDir, injectDir, error)) {
            error = error.isEmpty()
                    ? QStringLiteral("注入定义器目录失败:%1").arg(definerDir)
                    : error;
            fail(QStringLiteral("加密与合并"));
            return;
        }
        emit log(QStringLiteral("已注入定义器(明文区): %1 个")
                         .arg(definerClasses.size()));
    } else if (!definerClasses.isEmpty()) {
        error = QStringLiteral("定义器目录不见了:%1").arg(definerDir);
        fail(QStringLiteral("加密与合并"));
        return;
    }

    if (!stepFinalPack(config, userNativeJar, injectDir, keyHex,
                         // Fabric:真实入口完全藏进载荷,所以:
                         //   mainClass 传桥类(避免 MANIFEST 里泄露真实入口名),
                         //   bridgeClass 传空(不重定向任何类的父类 ——
                         //   Fabric 没有 JavaPlugin 那种单实例硬检查,
                         //   入口是我们自己反射调用的)。
                         config.fabricMod ? fabricBridge : targetClass,
                         config.fabricMod ? QString() : entryClass,
                         signKeyPath, error)) {
        return;
    }

    // ---- 11. 收尾 ----
    emit staged(++step, kTotalSteps, QStringLiteral("完成"));
    // 私钥用完即删:留在构建目录里就等于把“伪造签名”的能力一起留下了。
    if (!signKeyPath.isEmpty() && QFile::exists(signKeyPath)) {
        QFile::remove(signKeyPath);
        emit log(QStringLiteral("反篡改:签名私钥已销毁(想重新签名就只能重新打包)"));
    }
    m_running = false;
    emit log(QStringLiteral("总耗时 %1 ms").arg(timer.elapsed()));
    emit finished(true, QStringLiteral("加壳完成:%1").arg(config.outputJar));
}

// ---------------------------------------------------------------------------
// 步骤实现
// ---------------------------------------------------------------------------

bool PackerPipeline::stepPrepareFabricMod(const Config &config,
                                          const QString &inputJar,
                                          const QString &bridgeClass,
                                          QString &errorMessage) {
    m_fabricKeepPlain.clear();
    m_fabricFrozen.clear();
    m_fabricResourceOverrides.clear();

    // 元数据落地目录。放输出 JAR 旁边,解压完就留着 —— 改写后的
    // fabric.mod.json 要到 stepFinalPack 才被用上。
    const QString metaDir = QDir(QFileInfo(config.outputJar).absolutePath())
                                    .filePath(QStringLiteral("ahx-fabric-meta"));
    Archive::removeDirectoryRecursively(metaDir);
    if (!Archive::extract(inputJar, metaDir, &errorMessage)) {
        errorMessage = QStringLiteral("解开 JAR 读 Fabric 元数据失败:%1").arg(errorMessage);
        return false;
    }

    // ---- 1. mixins:哪些类必须明文 + 冻结改名 ----
    //
    // 第一阶段不重写 *.mixins.json(那要动 jar-obfuscator 的 ResourceTransformer),
    // 所以 mixin 类名只能冻结不改。但**明文**是硬性的:Mixin 框架自己从 JAR 里
    // 按名字读字节,不走 Class.forName,加密了它就看不到这个类。
    QDirIterator it(metaDir, { QStringLiteral("*.mixins.json") },
                    QDir::Files, QDirIterator::Subdirectories);
    int mixinConfigs = 0;
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        const QString pkg = root.value(QStringLiteral("package")).toString();
        const QString prefix = pkg.isEmpty() ? QString() : pkg + QLatin1Char('.');
        ++mixinConfigs;

        const QStringList keys = { QStringLiteral("mixins"), QStringLiteral("client"),
                                   QStringLiteral("server") };
        for (const QString &key : keys) {
            const QJsonArray list = root.value(key).toArray();
            for (const QJsonValue &value : list) {
                // 两种写法都要认:字符串,或者带 class 字段的对象
                QString name = value.isString()
                        ? value.toString()
                        : (value.isObject()
                           ? value.toObject().value(QStringLiteral("class")).toString()
                           : QString());
                name = name.trimmed();
                if (name.isEmpty()) {
                    continue;
                }
                const QString full = name.contains(QLatin1Char('.')) ? name : prefix + name;
                m_fabricKeepPlain << full;
                m_fabricFrozen << full;
            }
        }
    }
    m_fabricKeepPlain.removeDuplicates();
    m_fabricFrozen.removeDuplicates();

    // ---- 2. fabric.mod.json:抹掉 entrypoints,只留指向桥类的 preLaunch ----
    const QString modJsonPath = QDir(metaDir).filePath(QStringLiteral("fabric.mod.json"));
    QFile modJsonFile(modJsonPath);
    if (!modJsonFile.exists() || !modJsonFile.open(QIODevice::ReadOnly)) {
        errorMessage = QStringLiteral("输入 JAR 里没有 fabric.mod.json,不是 Fabric MOD");
        return false;
    }
    QJsonObject mod = QJsonDocument::fromJson(modJsonFile.readAll()).object();
    modJsonFile.close();

    const QJsonObject original = mod.value(QStringLiteral("entrypoints")).toObject();
    int erased = 0;
    for (auto entry = original.begin(); entry != original.end(); ++entry) {
        erased += entry.value().toArray().size();
    }

    // 四个阶段都指向桥类:载荷由 preLaunch 装好,真实入口由 Fabric 在各自阶段
    // 回调时再拉起 —— **不能**在 preLaunch 就把 client 入口拉起来,那时游戏
    // 还没初始化(实测:voicechat 会去注册按键绑定,MinecraftClient 还是 null)。
    QJsonObject entrypoints;
    entrypoints.insert(QStringLiteral("preLaunch"), QJsonArray{ bridgeClass });
    entrypoints.insert(QStringLiteral("main"), QJsonArray{ bridgeClass });
    entrypoints.insert(QStringLiteral("client"), QJsonArray{ bridgeClass });
    entrypoints.insert(QStringLiteral("server"), QJsonArray{ bridgeClass });
    mod.insert(QStringLiteral("entrypoints"), entrypoints);

    const QString rewritten = metaDir + QStringLiteral("/fabric.mod.json.ahx");
    if (!writeFile(rewritten,
                   QString::fromUtf8(QJsonDocument(mod).toJson(QJsonDocument::Indented)),
                   errorMessage)) {
        return false;
    }
    m_fabricResourceOverrides << QStringLiteral("fabric.mod.json=") + rewritten;
    m_fabricFrozen << bridgeClass;
    m_fabricFrozen.removeDuplicates();

    emit log(QStringLiteral("Fabric 元数据: %1 个 mixins 配置,抹除 %2 项 entrypoint,"
                            "改为 preLaunch → %3")
                     .arg(mixinConfigs)
                     .arg(erased)
                     .arg(bridgeClass));
    return true;
}

bool PackerPipeline::stepObfuscateUser(const Config &config, const QString &workDir,
                                       QString &userJar, QString &mappingFile,
                                       QString &errorMessage) {
    // jar-obfuscator 没有输出路径选项 —— 它一定把产物写在**输入 jar 旁边**
    // (`<输入名>_obf.jar` 和 `<输入名>_obf.jar.mapping.txt`)。
    // 而 mapping.txt 含原始类名,留在用户工程里会被提交、被误分发。
    // 所以先把输入复制到构建目录,再对副本动手。
    const QString stagedInput = QDir(workDir).filePath(QStringLiteral("input.jar"));
    QFile::remove(stagedInput);
    if (!QFile::copy(config.inputJar, stagedInput)) {
        errorMessage = QStringLiteral("无法复制输入 JAR 到 %1").arg(stagedInput);
        return false;
    }
    const QString outJar = QDir(workDir).filePath(QStringLiteral("input_obf.jar"));

    // 用户 JAR:主类**不允许任何改名**(类名、包名都不动) ——
    // Paper 的 plugin.yml.main 必须保持原始名字,用户也要求 main 不能是乱码。
    // 黑名单类在混淆器里走 identity 映射,但方法体/字段里的引用照常重映射。
    QStringList blackClass;
    if (!config.originalMainClass.trimmed().isEmpty()) {
        blackClass << config.originalMainClass.trimmed();
    }
    // Fabric:桥类与 mixin 类必须冻结改名。
    // 前者被 fabric.mod.json 用字符串引用,后者被 *.mixins.json 用字符串引用
    // —— 第一阶段不重写那两个 JSON,所以名字一动 mod 就崩。
    for (const QString &frozen : m_fabricFrozen) {
        const QString trimmed = frozen.trimmed();
        if (!trimmed.isEmpty() && !blackClass.contains(trimmed)) {
            blackClass << trimmed;
        }
    }
    // 第三方库用正则整体拉黑(否则会破坏反射/序列化等).
    const QStringList blackRegex = {
        QStringLiteral("java/.*"),  QStringLiteral("javax/.*"),
        QStringLiteral("org/bukkit/.*"), QStringLiteral("io/papermc/.*"),
        QStringLiteral("org/apache/.*"), QStringLiteral("com/google/.*"),
        QStringLiteral("kotlin/.*"), QStringLiteral(".*/shaded/.*"),
    };

    const QString cfgPath = QDir(workDir).filePath(QStringLiteral("user-obf.yaml"));
    // 占住一个解密器类名,留给下面 stepObfuscateModule 避开
    m_userDecryptClass = pickDecryptClassName(QStringList());
    if (!writeFile(cfgPath,
                   buildObfuscatorConfig(config, blackClass, blackRegex, m_userDecryptClass,
                                         true /* forUserJar:按界面勾选 */),
                   errorMessage)) {
        return false;
    }
    emit log(QStringLiteral("主类已拉黑不改名: %1").arg(config.originalMainClass));
    emit log(QStringLiteral("字符串解密器类: %1").arg(m_userDecryptClass));

    {
        QStringList args;
        const QString extraCp = extraClasspathArg(config);
        if (!extraCp.isEmpty()) {
            args << extraCp;
        }
        args << QStringLiteral("-jar") << config.obfuscatorJar
             << QStringLiteral("--jar") << stagedInput
             << QStringLiteral("--config") << cfgPath;
        if (!runTool(config.javaExe, args, workDir, errorMessage)) {
            return false;
        }
    }

    if (!QFileInfo::exists(outJar)) {
        errorMessage = QStringLiteral("混淆器未产出 %1").arg(outJar);
        return false;
    }
    userJar = outJar;
    mappingFile = outJar + QStringLiteral(".mapping.txt");
    if (!QFileInfo::exists(mappingFile)) {
        errorMessage = QStringLiteral("未找到改名映射表 %1").arg(mappingFile);
        return false;
    }
    emit log(QStringLiteral("用户 JAR 已混淆: %1").arg(userJar));
    emit log(QStringLiteral("改名映射表: %1").arg(mappingFile));
    return true;
}

bool PackerPipeline::stepResolveTarget(const QString &mappingFile,
                                       const QString &originalMain,
                                       QString &targetClass,
                                       QString &errorMessage) {
    Q_UNUSED(originalMain);

    QFile file(mappingFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errorMessage = QStringLiteral("无法读取映射表 %1").arg(mappingFile);
        return false;
    }
    const QString text = QString::fromUtf8(file.readAll());
    file.close();

    // 映射表里类名形如:  top.h3k4.unpassMain -> iillillLLL.llliiilLll.liiLillLiili
    // 主类是唯一同时出现在 MANIFEST.Main-Class / plugin.yml.main 里的那个,
    // 这里改用更可靠的判据:取「被 plugin.yml 或 MANIFEST 指向」的原始主类,
    // 由调用方通过 originalMain 传入;若为空则退化为取映射里第一个类。
    static const QRegularExpression re(QStringLiteral("^([\\w.$]+)\\s*->\\s*([\\w.$]+)\\s*$"));

    QStringList pairs;
    for (const QString &rawLine : text.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QRegularExpressionMatch match = re.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        const QString from = match.captured(1);
        const QString to = match.captured(2);
        if (!originalMain.isEmpty() && from == originalMain) {
            targetClass = to;
            return true;
        }
        pairs << from + QStringLiteral("=") + to;
    }

    if (!originalMain.isEmpty()) {
        // 主类被拉黑不改名(或整个类名混淆被关掉)时,identity 映射不会被写进
        // mapping.txt(MappingWriter 跳过 from==to 的条目)。
        // 这两种情况下主类在产物里都仍是原名。
        emit log(QStringLiteral("主类 %1 未在映射表里,按原名继续").arg(originalMain));
        targetClass = originalMain;
        return true;
    }
    if (pairs.isEmpty()) {
        errorMessage = QStringLiteral("映射表里没有类名映射");
        return false;
    }
    errorMessage = QStringLiteral("原始主类 %1 不在映射表里(可能被拉黑改名了)")
                           .arg(originalMain);
    return false;
}

bool PackerPipeline::stepGenerateSignKey(const Config &config, const QString &buildDir,
                                        QString &pubKeyHex, QString &errorMessage) {
    // AhxSign 与 AhxPacker 一样是“单文件源码启动”,发布版从内嵌资源释放一份 ——
    // 用户机器上没有 AntiHackerXVerify/tools/ 这个目录。
    const QString signerSrc = QDir(buildDir).filePath(QStringLiteral("AhxSign.java"));
    if (!releaseResource(QStringLiteral(":/verify/tools/AhxSign.java"), signerSrc,
                         errorMessage)) {
        return false;
    }
    QStringList args;
    args << signerSrc << QStringLiteral("keygen") << buildDir;
    if (!runTool(config.javaExe, args, buildDir, errorMessage)) {
        errorMessage = QStringLiteral("生成签名密钥失败:%1").arg(errorMessage);
        return false;
    }
    const QString pubPath = QDir(buildDir).filePath(QStringLiteral("ahx-sign.pub"));
    QFile pub(pubPath);
    if (!pub.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errorMessage = QStringLiteral("找不到签名公钥 %1").arg(pubPath);
        return false;
    }
    pubKeyHex = QString::fromLatin1(pub.readAll()).trimmed();
    pub.close();
    if (pubKeyHex.isEmpty()) {
        errorMessage = QStringLiteral("签名公钥为空:%1").arg(pubPath);
        return false;
    }
    return true;
}

bool PackerPipeline::stepPrepareModule(const Config &config,
                                       const QString &targetClass,
                                       const QStringList &payloadPackages,
                                       const QString &tamperPubKeyHex,
                                       QString &workDir,
                                       QString &keyHex,
                                       QStringList &definerClasses,
                                       QString &errorMessage) {
    VerifyModule::Options options = config.verifyOptions;
    options.tamperPubKeyHex = tamperPubKeyHex;
    const VerifyModule::Result result =
            VerifyModule::prepare(config.inputJar, targetClass, options, payloadPackages);
    if (!result.ok) {
        errorMessage = result.errorMessage;
        return false;
    }
    workDir = result.workDir;
    keyHex = result.keyHex;
    definerClasses = result.definerClasses;
    emit log(QStringLiteral("验证模块工作目录: %1").arg(workDir));
    emit log(QStringLiteral("入口类: %1").arg(result.bootstrapClassName));
    emit log(QStringLiteral("定义器: %1 × %2 个包")
                     .arg(result.linkName).arg(definerClasses.size()));
    return true;
}

bool PackerPipeline::stepCompileModule(const Config &config,
                                       const QString &moduleDir,
                                       const QString &targetClass,
                                       const QStringList &definerClasses,
                                       QString &moduleJar,
                                       QString &errorMessage) {
    Q_UNUSED(targetClass);

    const QString classesDir = QDir(moduleDir).filePath(QStringLiteral("_classes"));
    if (!QDir().mkpath(classesDir)) {
        errorMessage = QStringLiteral("无法创建 %1").arg(classesDir);
        return false;
    }

    // 一次递归就够:moduleDir/java/**/*.java 和 moduleDir/*.java
    // (默认包路由下入口就落在 moduleDir 根上)都在里面。
    // 之前分两次收集再相加,而第二次本来就包含第一次的结果 —— 每个源文件都传了两遍。
    QStringList sources = collectBySuffix(moduleDir, { QStringLiteral(".java") });
    sources.removeDuplicates();
    if (sources.isEmpty()) {
        errorMessage = QStringLiteral("%1 下没有找到任何 .java 源文件").arg(moduleDir);
        return false;
    }

    QStringList args;
    args << QStringLiteral("--release") << QStringLiteral("8")
         << QStringLiteral("-proc:none")     // 关键:否则会读到插件自带的注解处理器并报错
         << QStringLiteral("-encoding") << QStringLiteral("UTF-8")
         << QStringLiteral("-d") << classesDir;

    // 类路径 = 插件 JAR + 用户给的服务端/API 依赖。
    // Paper 入口要 import org.bukkit.plugin.java.JavaPlugin,而它是 provided 依赖,
    // 插件 JAR 里没有 —— 只给插件 JAR 的话这一步必然编不过。
    QStringList cp;
    cp << QFileInfo(config.inputJar).absoluteFilePath();
    cp << expandJars(config.extraClassPath);
    args << QStringLiteral("-cp") << cp.join(QDir::listSeparator());
    args << sources;

    if (!runTool(config.javacExe, args, moduleDir, errorMessage)) {
        return false;
    }

    // 定义器必须彻底绕开混淆器与 NOBF:jar-obfuscator 会重命名**包名**
    // (类名黑名单挡得住类名,挡不住包路径),而入口类里的 LINK 数组记的是
    // 打包时的原名 —— 一旦包名被改,运行时 Class.forName 就找不到定义器,
    // 进而每个载荷包都「没有定义器」,解密出来的类一个都定义不进去。
    // 所以在这里把它们从 _classes 里摘出来单独存(run() 最后注入明文区)。
    if (!definerClasses.isEmpty()) {
        const QString definerDir = QDir(moduleDir).filePath(QStringLiteral("_definers"));
        if (!QDir().mkpath(definerDir)) {
            errorMessage = QStringLiteral("无法创建定义器目录:%1").arg(definerDir);
            return false;
        }
        int moved = 0;
        for (const QString &fqn : definerClasses) {
            if (fqn.trimmed().isEmpty()) {
                continue;
            }
            const QString internal = QString(fqn).replace(QLatin1Char('.'),
                                                          QLatin1Char('/'));
            const QString src = QDir(classesDir).filePath(internal + QStringLiteral(".class"));
            if (!QFileInfo::exists(src)) {
                continue;
            }
            const QString dst = QDir(definerDir).filePath(internal + QStringLiteral(".class"));
            QDir().mkpath(QFileInfo(dst).absolutePath());
            // rename 优先(同盘瞬时);跨盘时退化成 copy+remove ——
            // 必须确保 _classes 里不再留下副本,否则混淆器照样能看到它。
            if (QFile::rename(src, dst)) {
                ++moved;
            } else if (QFile::copy(src, dst) && QFile::remove(src)) {
                ++moved;
            } else {
                errorMessage = QStringLiteral("无法隔离定义器 %1(源 %2)").arg(fqn, src);
                return false;
            }
        }
        emit log(QStringLiteral("定义器已隔离出混淆流程: %1 / %2")
                         .arg(moved).arg(definerClasses.size()));
    }

    moduleJar = QDir(moduleDir).filePath(QStringLiteral("module.jar"));
    if (!runTool(config.jarExe,
                 QStringList() << QStringLiteral("cf") << moduleJar
                               << QStringLiteral("-C") << classesDir << QStringLiteral("."),
                 moduleDir, errorMessage)) {
        return false;
    }
    emit log(QStringLiteral("验证模块已编译: %1").arg(moduleJar));
    return true;
}

bool PackerPipeline::stepObfuscateModule(const Config &config,
                                         const QString &moduleJar,
                                         const QString &entryClass,
                                         const QStringList &keepNames,
                                         QString &obfJar,
                                         QString &errorMessage) {
    const QString dir = QFileInfo(moduleJar).absolutePath();
    const QString cfgPath = QDir(dir).filePath(QStringLiteral("module-obf.yaml"));

    // 入口类必须保名 —— plugin.yml / MANIFEST 要按这个名字引用它。
    // 其余类名、方法名拉到最满。
    QStringList blackList;
    QString entryInternal = entryClass;
    entryInternal.replace(QLatin1Char('.'), QLatin1Char('/'));
    blackList << entryInternal;

    // 定义器也必须保名 —— 运行时是按「包名 + 固定简单名」去 Class.forName 找它的,
    // 被改名后就找不到了。
    for (const QString &name : keepNames) {
        QString internal = name;
        internal.replace(QLatin1Char('.'), QLatin1Char('/'));
        if (!blackList.contains(internal)) {
            blackList << internal;
        }
    }
    // 必须避开用户 JAR 已占用的解密器名:两个 JAR 各生成一份同名类的话,
    // 父优先加载会让用户类的解密调用落在模块那份上,密钥不同 -> 解出 null。
    const QString moduleDecryptClass = pickDecryptClassName({ m_userDecryptClass });
    emit log(QStringLiteral("字符串解密器类: %1").arg(moduleDecryptClass));
    if (!writeFile(cfgPath,
                   buildObfuscatorConfig(config, blackList, QStringList(), moduleDecryptClass,
                                         false /* 验证模块:强制全开 */),
                   errorMessage)) {
        return false;
    }

    {
        QStringList args;
        const QString extraCp = extraClasspathArg(config);
        if (!extraCp.isEmpty()) {
            args << extraCp;
        }
        args << QStringLiteral("-jar") << config.obfuscatorJar
             << QStringLiteral("--jar") << moduleJar
             << QStringLiteral("--config") << cfgPath;
        if (!runTool(config.javaExe, args, dir, errorMessage)) {
            return false;
        }
    }

    obfJar = dir + QStringLiteral("/") + QFileInfo(moduleJar).completeBaseName() + QStringLiteral("_obf.jar");
    if (!QFileInfo::exists(obfJar)) {
        errorMessage = QStringLiteral("验证模块混淆未产出 %1").arg(obfJar);
        return false;
    }
    emit log(QStringLiteral("验证模块已混淆: %1").arg(obfJar));
    return true;
}

bool PackerPipeline::stepNativeConvert(const Config &config,
                                       const QString &inputJar,
                                       const QString &outDir,
                                       const QString &nativeDirName,
                                       const QString &loaderName,
                                       const QString &hiddenName,
                                       const QString &whiteList,
                                       const QString &libsDir,
                                       const QStringList &blackList,
                                       QString &cppDir,
                                       QString &nativeJar,
                                       QString &errorMessage) {
    QDir().mkpath(outDir);

    QStringList args;
    args << QStringLiteral("-jar") << config.nobfJar
         << inputJar << outDir;
    // native 目录名 / 加载器类名 / 隐藏类名前缀统统随机,不许 NOBF 自己挑。
    // 理由:加载器与合成隐藏类会被原生库 DefineClass 进 bootstrap 加载器
    // (JVM 全局名字空间),两份加壳插件用同一套名字就是
    //   LinkageError: loader 'bootstrap' attempted duplicate class definition
    // 而 NOBF 默认取「输入 JAR 里第一个没用过的 nativeN」—— 两次转换的输入各自
    // 都不含 nativeN,于是都会挑 native0,必撞。
    args << QStringLiteral("--custom-lib-dir") << nativeDirName;
    args << QStringLiteral("--loader-name") << loaderName;
    args << QStringLiteral("--hidden-name") << hiddenName;
    args << QStringLiteral("-p") << QStringLiteral("hotspot");
    if (!whiteList.isEmpty()) {
        args << QStringLiteral("-w") << whiteList;
    }
    // -l 只接受目录。它决定 NOBF 算栈帧时能不能解析类层次 ——
    // 转 Bukkit 插件类(继承 JavaPlugin 之类)时需要服务端 API。
    if (!libsDir.isEmpty() && QDir(libsDir).exists()
        && !QDir(libsDir).entryList(QDir::Files).isEmpty()) {
        args << QStringLiteral("-l") << libsDir;
    }
    // 黑名单。定义器只有一行静态初始化,没有任何值得原生化的东西,
    // 转过去只会白增体积还多一层出错机会。
    if (!blackList.isEmpty()) {
        const QString blPath = QDir(outDir).filePath(QStringLiteral("nobf-blacklist.txt"));
        if (!writeFile(blPath, blackList.join(QLatin1Char('\n')) + QLatin1Char('\n'),
                       errorMessage)) {
            return false;
        }
        args << QStringLiteral("-b") << blPath;
    }

    if (!runTool(config.javaExe, args, QFileInfo(inputJar).absolutePath(), errorMessage)) {
        return false;
    }

    cppDir = QDir(outDir).filePath(QStringLiteral("cpp"));
    if (!QDir(cppDir).exists()) {
        errorMessage = QStringLiteral("未生成 C++ 目录 %1").arg(cppDir);
        return false;
    }
    nativeJar = QDir(outDir).filePath(QFileInfo(inputJar).fileName());
    if (!QFileInfo::exists(nativeJar)) {
        errorMessage = QStringLiteral("未找到 NOBF 输出的 JAR %1").arg(nativeJar);
        return false;
    }
    emit log(QStringLiteral("C++ 源码(%1): %2").arg(nativeDirName, cppDir));
    return true;
}

bool PackerPipeline::stepBuildNative(const Config &config,
                                     const QString &cppDir,
                                     QString &errorMessage) {
    if (config.zigExe.isEmpty() || !QFileInfo::exists(config.zigExe)) {
        errorMessage = QStringLiteral("未找到 zig 交叉编译器");
        return false;
    }

    // 准备平台相关的 JNI 头
    const QString winJniDir = QDir(cppDir).filePath(QStringLiteral("_winjni"));
    QDir().mkpath(winJniDir);
    if (!writeFile(winJniDir + QStringLiteral("/jni_md.h"),
                   QString::fromLatin1(kWin32JniMd), errorMessage)) {
        return false;
    }

    // 收集全部 .cpp,但跳过 NOBF 自带的 build/ 目录(里面是 cmake 产物)。
    // 注意:必须用「相对 cppDir 的路径」来判断 —— 绝对路径里可能本来就含
    // /build/(比如打包器被构建在 <项目>/build/bin 下),那样会误杀全部源文件。
    QStringList sources;
    {
        const QDir root(cppDir);
        const QStringList all = collectBySuffix(cppDir, { QStringLiteral(".cpp") });
        for (const QString &s : all) {
            const QString rel = root.relativeFilePath(s);
            if (rel.startsWith(QStringLiteral("build/"))) {
                continue;
            }
            sources << s;
        }
    }
    if (sources.isEmpty()) {
        errorMessage = QStringLiteral("在 %1 下没有找到任何 .cpp 源文件").arg(cppDir);
        return false;
    }

    const QString jniInclude = config.javaHome + QStringLiteral("/include");

    struct TargetSpec {
        const char *triple;
        QString output;
    };
    QList<TargetSpec> targets;
    if (config.target == Target::WindowsX64 || config.target == Target::Both) {
        targets.append({ "x86_64-windows-gnu", QDir(cppDir).filePath(QStringLiteral("x64-windows.dll")) });
    }
    if (config.target == Target::LinuxX64 || config.target == Target::Both) {
        targets.append({ "x86_64-linux-gnu", QDir(cppDir).filePath(QStringLiteral("x64-linux.so")) });
    }

    for (const TargetSpec &spec : targets) {
        const bool windows = QString::fromLatin1(spec.triple).contains(QStringLiteral("windows"));

        // 公共参数。-c 与输入/输出文件在编译和链接两趟里不一样。
        QStringList common;
        common << QStringLiteral("c++")
               << QStringLiteral("-target") << QString::fromLatin1(spec.triple)
               << QStringLiteral("-O2")
               << QStringLiteral("-DNDEBUG") << QStringLiteral("-DUSE_HOTSPOT=1")
               << QStringLiteral("-I") << cppDir
               << QStringLiteral("-I") << cppDir + QStringLiteral("/output")
               << QStringLiteral("-I") << jniInclude;

        if (windows) {
            common << QStringLiteral("-I") << winJniDir;
        } else {
            common << QStringLiteral("-I") << jniInclude + QStringLiteral("/linux");
        }

        const QString label = QFileInfo(spec.output).fileName();

        // ---- 第一趟:并行把每个 .cpp 编成 .o ----
        // 为什么不用一条 `zig c++ a.cpp b.cpp ... -o lib.so`:那样 clang 在
        // **一个进程里串行**编几百个文件 —— 只吃一个核(所以风扇狂转但很慢),
        // 而且在结束前**一行输出都没有**,用户完全看不出是在跑还是卡死了。
        // 拆开之后既能吃满多核,又能逐个文件报进度。
        const QString objDir = QDir(cppDir).filePath(
                QStringLiteral("_obj_") + QString::fromLatin1(spec.triple).section(QLatin1Char('-'), 0, 0)
                + QStringLiteral("_") + (windows ? QStringLiteral("win") : QStringLiteral("nix")));
        QStringList objects;
        if (!compileObjects(config, sources, common, objDir, label, objects, errorMessage)) {
            return false;
        }

        // ---- 第二趟:链接 ----
        QStringList linkArgs = common;
        linkArgs << QStringLiteral("-shared") << objects
                 << QStringLiteral("-o") << spec.output;

        emit log(QStringLiteral("链接 %1(%2 个目标文件)...").arg(label).arg(objects.size()));
        if (!runTool(config.zigExe, linkArgs, cppDir, errorMessage)) {
            return false;
        }
        if (!QFileInfo::exists(spec.output)) {
            errorMessage = QStringLiteral("未产出 %1").arg(spec.output);
            return false;
        }
        emit log(QStringLiteral("已产出 %1(%2 字节)")
                         .arg(label).arg(QFileInfo(spec.output).size()));
    }
    return true;
}

// ---------------------------------------------------------------------------
// 并行编译
// ---------------------------------------------------------------------------

bool PackerPipeline::compileObjects(const Config &config,
                                    const QStringList &sources,
                                    const QStringList &commonArgs,
                                    const QString &objDir,
                                    const QString &label,
                                    QStringList &objects,
                                    QString &errorMessage) {
    QDir().mkpath(objDir);

    const int total = sources.size();
    // 并行度:吃满多核,但别开太多 —— NOBF 生成的 .cpp 单个就能吃掉几百 MB 内存,
    // 32 核机器上无脑开 32 个 clang 会直接 OOM。
    const int parallel = qBound(1, QThread::idealThreadCount(), 8);

    emit log(QStringLiteral("编译 %1:%2 个源文件,并行度 %3")
                     .arg(label).arg(total).arg(parallel));

    struct Job {
        QString source;
        QString object;
        QProcess *proc = nullptr;
        QByteArray output;
        bool finished = false;
    };

    QList<Job> jobs;
    jobs.reserve(total);
    for (int i = 0; i < total; ++i) {
        Job job;
        job.source = sources.at(i);
        // 目标文件只用编号命名。NOBF 生成的类名本身就有 150+ 字符,
        // 再拼上源文件名很容易撞上文件系统的 255 字节名长上限。
        job.object = QDir(objDir).filePath(
                QString::number(i + 1).rightJustified(4, QLatin1Char('0'))
                + QStringLiteral(".o"));
        jobs.append(job);
    }

    auto killAll = [&jobs] {
        for (Job &job : jobs) {
            if (job.proc && !job.finished) {
                job.proc->kill();
                job.proc->waitForFinished(2000);
                delete job.proc;
                job.proc = nullptr;
            }
        }
    };

    int started = 0;
    int completed = 0;
    int failed = 0;
    QString sampleFailure;

    while (completed < total) {
        if (m_stop) {
            killAll();
            errorMessage = QStringLiteral("已中止");
            return false;
        }

        // 补满在跑的任务
        while (started < total && (started - completed) < parallel) {
            Job &job = jobs[started];
            ++started;

            // ⚠ 不要 new QProcess(this):this 活在**主线程**,而这里跑在工作线程。
            //   挂到 this 上会被 Qt 移回主线程,而工作线程没有事件循环,
            //   进程状态就再也不会更新了。无父对象即可。
            job.proc = new QProcess();
            job.proc->setProcessChannelMode(QProcess::MergedChannels);
            job.proc->setWorkingDirectory(QFileInfo(job.source).absolutePath());

            QStringList args = commonArgs;
            args << QStringLiteral("-c") << job.source << QStringLiteral("-o") << job.object;
            job.proc->start(config.zigExe, args);
            if (!job.proc->waitForStarted(20000)) {
                errorMessage = QStringLiteral("无法启动 zig 编译 %1:%2")
                                       .arg(job.source, job.proc->errorString());
                delete job.proc;
                job.proc = nullptr;
                job.finished = true;
                killAll();
                return false;
            }
        }

        // ⚠ 必须调 waitForFinished —— 工作线程里没有事件循环,光看 state()
        //   永远不会变成 NotRunning,那就是个死循环(这个坑已经踩过一次了,
        //   表现为"卡在 [8/11] 交叉编译原生库"一动不动)。
        //   runTool 一直是这么写的,所以它才没出问题。
        for (Job &job : jobs) {
            if (!job.proc || job.finished) {
                continue;
            }
            const bool done = job.proc->waitForFinished(10);
            job.output += job.proc->readAll();
            if (!done) {
                continue;
            }

            job.finished = true;
            ++completed;
            const bool ok = (job.proc->exitStatus() == QProcess::NormalExit
                             && job.proc->exitCode() == 0);
            if (ok) {
                objects << job.object;
            } else {
                ++failed;
                if (sampleFailure.isEmpty()) {
                    sampleFailure = QFileInfo(job.source).fileName();
                }
            }

            // 编译器的真实输出(警告/错误)照打,不再吞掉
            const QString text = QString::fromLocal8Bit(job.output);
            for (const QString &line : text.split(QLatin1Char('\n'))) {
                const QString trimmed = line.trimmed();
                if (!trimmed.isEmpty()) {
                    emit log(QStringLiteral("  | %1").arg(trimmed));
                }
            }
            emit log(QStringLiteral("  [%1/%2] %3").arg(completed).arg(total)
                             .arg(QFileInfo(job.source).fileName()));

            delete job.proc;
            job.proc = nullptr;
        }
    }

    if (failed > 0) {
        errorMessage = QStringLiteral("%1 里有 %2/%3 个源文件编译失败,例如 %4")
                               .arg(label).arg(failed).arg(total).arg(sampleFailure);
        return false;
    }
    emit log(QStringLiteral("%1 编译完成(%2 个目标文件)").arg(label).arg(objects.size()));
    return true;
}

bool PackerPipeline::placeNativeLibs(const QString &cppDir,
                                     const QString &destNativeDir,
                                     QString &errorMessage) {
    QDir().mkpath(destNativeDir);
    const QStringList libs = QDir(cppDir).entryList(
            { QStringLiteral("*.so"), QStringLiteral("*.dll") }, QDir::Files);
    if (libs.isEmpty()) {
        errorMessage = QStringLiteral("没有得到任何原生库(编译那步没产出?):%1").arg(cppDir);
        return false;
    }
    const QString label = QFileInfo(destNativeDir).fileName();
    for (const QString &lib : libs) {
        const QString src = QDir(cppDir).filePath(lib);
        const QString dst = QDir(destNativeDir).filePath(lib);
        QFile::remove(dst);
        if (!QFile::copy(src, dst)) {
            errorMessage = QStringLiteral("复制原生库失败: %1").arg(dst);
            return false;
        }
        emit log(QStringLiteral("原生库 -> %1/%2 (%3 字节)")
                         .arg(label, lib).arg(QFileInfo(src).size()));
    }
    return true;
}

bool PackerPipeline::stepPatchLoader(const Config &config,
                                     const QString &nativeDir,
                                     QString &errorMessage) {
    Q_UNUSED(config)
    // 加载器的包名与类名现在都是每份产物随机的(--custom-lib-dir / --loader-name),
    // 所以这里不能再写死 "native*" 与 "Loader.java" —— 直接把生成源项目里
    // 唯一的那个 .java 找出来。
    const QDir javaRoot(nativeDir + QStringLiteral("/java"));
    QStringList javaFiles;
    QDirIterator javaIt(javaRoot.absolutePath(), QStringList() << QStringLiteral("*.java"),
                        QDir::Files, QDirIterator::Subdirectories);
    while (javaIt.hasNext()) {
        javaFiles << javaIt.next();
    }
    if (javaFiles.isEmpty()) {
        errorMessage = QStringLiteral("%1 下找不到加载器源文件")
                               .arg(javaRoot.absolutePath());
        return false;
    }

    int patched = 0;
    for (const QString &loaderPath : javaFiles) {
        QFile file(loaderPath);
        if (!file.exists()) {
            continue;
        }
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            errorMessage = QStringLiteral("无法读取 %1").arg(loaderPath);
            return false;
        }
        QString source = QString::fromUtf8(file.readAll());
        file.close();

        // 生成的 Loader 用 createTempFile("lib", null) 得到 libNNNN.tmp;
        // Windows 的 LoadLibrary 要求 .dll 扩展名,否则加载失败。
        const QString oldCall = QStringLiteral("File.createTempFile(\"lib\", null)");
        const QString newCall = QStringLiteral(
                "File.createTempFile(\"lib\", osTypeName.substring(osTypeName.lastIndexOf(0x2E)))");

        if (source.contains(newCall)) {
            continue;   // 已经补过
        }
        if (!source.contains(oldCall)) {
            errorMessage = QStringLiteral(
                    "%1 里找不到预期的 createTempFile 调用(上游生成代码变了?)")
                                   .arg(loaderPath);
            return false;
        }
        source.replace(oldCall, newCall);
        if (!writeFile(loaderPath, source, errorMessage)) {
            return false;
        }
        emit log(QStringLiteral("已修补加载器源文件的临时文件扩展名: %1").arg(loaderPath));
        ++patched;
    }
    if (patched == 0) {
        emit log(QStringLiteral("Loader 已修补过,跳过"));
    }
    return true;
}

bool PackerPipeline::stepFinalPack(const Config &config,
                                   const QString &payloadSourceJar,
                                   const QString &injectDir,
                                   const QString &keyHex,
                                   const QString &mainClass,
                                   const QString &bridgeClass,
                                   const QString &signKeyPath,
                                   QString &errorMessage) {
    // 打包器源码:优先用磁盘上的(开发时改完立刻生效),没有就从内嵌资源释放一份。
    // 发布版必须走后者 —— 用户机器上没有 AntiHackerXVerify/tools/ 这个目录。
    QString packerSrc = config.ahxPackerSrc;
    if (packerSrc.isEmpty() || !QFileInfo::exists(packerSrc)) {
        const QString staged = QDir(injectDir + QStringLiteral("/.."))
                                       .filePath(QStringLiteral("AhxPacker.java"));
        if (!releaseResource(QStringLiteral(":/verify/tools/AhxPacker.java"),
                             staged, errorMessage)) {
            errorMessage = QStringLiteral(
                    "未找到 AhxPacker.java(磁盘上既没有 %1,内嵌资源也读不到:%2)")
                                   .arg(config.ahxPackerSrc, errorMessage);
            return false;
        }
        packerSrc = staged;
        emit log(QStringLiteral("AhxPacker.java 从内嵌资源释放: %1").arg(staged));
    }

    QStringList packerArgs;
    packerArgs << packerSrc
               << payloadSourceJar << config.outputJar
               << injectDir << keyHex << mainClass << bridgeClass;
    // Fabric 的两份额外输入:必须明文的 mixin 类,以及改写后的 fabric.mod.json。
    // 改写在算签名之前发生(AhxPacker 内部先覆盖资源再签名),
    // 否则签名盖的是旧字节,产物一启动就报“文件已被修改”。
    if (!m_fabricKeepPlain.isEmpty()) {
        packerArgs << QStringLiteral("--keep-plain")
                   << m_fabricKeepPlain.join(QLatin1Char(','));
    }
    for (const QString &override : m_fabricResourceOverrides) {
        packerArgs << QStringLiteral("--set-resource") << override;
    }
    if (!signKeyPath.isEmpty()) {
        // 打包末尾对“全部非类文件”算摘要并用这把私钥签名。
        packerArgs << QStringLiteral("--sign-key") << signKeyPath;
    }
    if (!runTool(config.javaExe, packerArgs,
                 QFileInfo(config.outputJar).absolutePath(), errorMessage)) {
        return false;
    }
    if (!QFileInfo::exists(config.outputJar)) {
        errorMessage = QStringLiteral("未产出 %1").arg(config.outputJar);
        return false;
    }
    emit log(QStringLiteral("最终产物: %1").arg(config.outputJar));
    return true;
}

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------

QStringList PackerPipeline::expandJars(const QStringList &entries) {
    QStringList out;
    for (const QString &entry : entries) {
        if (entry.isEmpty()) {
            continue;
        }
        const QFileInfo info(entry);
        if (info.isFile()) {
            out << info.absoluteFilePath();
            continue;
        }
        if (!info.isDir()) {
            continue;
        }
        // 目录:递归找 .jar/.zip。Paper 服务端的 libraries/ 就是多层嵌套的。
        QDirIterator it(info.absoluteFilePath(),
                        { QStringLiteral("*.jar"), QStringLiteral("*.zip") },
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            out << it.next();
        }
    }
    out.removeDuplicates();
    out.sort();
    return out;
}

int PackerPipeline::stageNobfLibs(const QStringList &jars, const QString &destDir) {
    QDir().mkpath(destDir);
    int staged = 0;
    for (const QString &jar : jars) {
        const QString base = QFileInfo(jar).fileName();
        if (base.isEmpty()) {
            continue;
        }
        // 不同目录下的同名 jar 会打架,加个序号后缀避开
        QString name = base;
        int seq = 1;
        while (QFileInfo::exists(QDir(destDir).filePath(name))) {
            name = QStringLiteral("%1.%2").arg(seq++).arg(base);
        }
        if (QFile::copy(jar, QDir(destDir).filePath(name))) {
            ++staged;
        }
    }
    return staged;
}

QHash<QString, QString> PackerPipeline::parseMappingFile(const QString &path) {
    QHash<QString, QString> out;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return out;
    }
    const QString text = QString::fromUtf8(file.readAll());
    file.close();

    // 类名形如:  top.h3k4.unpassMain -> iillillLLL.llliiilLll.liiLillLiili
    // 方法行带空格和括号(`Foo bar()V -> xxx`),字段行的 from 永远比类名多一段,
    // 所以这条正则只会命中原样就是类名映射的那些行。
    static const QRegularExpression re(QStringLiteral("^([\\w.$]+)\\s*->\\s*([\\w.$]+)\\s*$"));
    for (const QString &rawLine : text.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QRegularExpressionMatch match = re.match(line);
        if (match.hasMatch()) {
            out.insert(match.captured(1), match.captured(2));
        }
    }
    return out;
}

QStringList PackerPipeline::resolveHideClassNames(const QStringList &requested,
                                                  const QHash<QString, QString> &mapping) {
    QStringList out;
    for (const QString &name : requested) {
        if (name.isEmpty()) {
            continue;
        }
        // 映射表里没有 => 混淆器没改它(被黑名单跳过了),按原名用
        QString internal = mapping.value(name, name);
        internal.replace(QLatin1Char('.'), QLatin1Char('/'));
        if (!out.contains(internal)) {
            out << internal;
        }
    }
    out.sort();
    return out;
}

QString PackerPipeline::buildNativeWhiteList(const QStringList &internalNames) {
    // ⚠ NOBF 的 -w 是逐条**正则**:
    //     类名   -> `a/b/C`
    //     方法名 -> `a/b/C#方法名!描述符`
    //   而 shouldProcess 要求「类名能过」**且**「至少有一个方法能过」。
    //   只写类名的话方法全都匹配不上,整个类会被静默跳过(日志里只出现 Skipping)。
    //   所以每个类必须写两条。`**` 会被展开成 `(.*?)`,能吃掉 `#` 后面的部分。
    //
    //   另外:NOBF 对空文件会得到一个空表,而「不在表里就不处理」是恒真的,
    //   结果是**什么都不转**。所以调用方必须保证这里非空。
    QString out;
    out += QStringLiteral("# native-obfuscator -w 白名单(每类两条:类名 + 全部方法)\n");
    for (const QString &name : internalNames) {
        out += name + QStringLiteral("\n");
        out += name + QStringLiteral("#**\n");
    }
    return out;
}


bool PackerPipeline::runTool(const QString &program, const QStringList &args,
                             const QString &workDir, QString &errorMessage) {
    if (program.isEmpty()) {
        errorMessage = QStringLiteral("工具路径为空");
        return false;
    }

    QProcess process;
    process.setWorkingDirectory(workDir);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, args);

    if (!process.waitForStarted(20000)) {
        errorMessage = QStringLiteral("无法启动 %1:%2").arg(program, process.errorString());
        return false;
    }

    emit log(QStringLiteral("$ %1 %2").arg(QFileInfo(program).fileName(), args.join(QLatin1Char(' '))));

    // 轮询读取输出,同时响应中止请求
    while (process.state() != QProcess::NotRunning) {
        if (m_stop) {
            process.kill();
            process.waitForFinished(3000);
            errorMessage = QStringLiteral("已中止");
            return false;
        }
        process.waitForFinished(200);
        const QByteArray chunk = process.readAllStandardOutput();
        if (!chunk.isEmpty()) {
            const QStringList lines = QString::fromLocal8Bit(chunk).split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                if (!line.trimmed().isEmpty()) {
                    emit log(QStringLiteral("  | %1").arg(line.trimmed()));
                }
            }
        }
    }

    const QByteArray tail = process.readAllStandardOutput();
    if (!tail.isEmpty()) {
        const QStringList lines = QString::fromLocal8Bit(tail).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (!line.trimmed().isEmpty()) {
                emit log(QStringLiteral("  | %1").arg(line.trimmed()));
            }
        }
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        errorMessage = QStringLiteral("%1 退出码 %2")
                               .arg(QFileInfo(program).fileName())
                               .arg(process.exitCode());
        return false;
    }
    return true;
}

bool PackerPipeline::writeFile(const QString &path, const QString &content,
                               QString &errorMessage) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        errorMessage = QStringLiteral("无法写入 %1").arg(path);
        return false;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << content;
    file.close();
    return true;
}

QString PackerPipeline::extraClasspathArg(const Config &config) {
    if (config.extraClassPath.isEmpty()) {
        return QString();
    }
    // 目录要**递归展开**成实际 JAR:Paper 的 libraries/ 是多层嵌套的
    // (libraries/com/google/code/gson/gson/2.11.0/gson-2.11.0.jar),
    // 而混淆器侧只做单层路径查找。
    // 目录本身也一并带上 —— 万一用户给的是散装 .class 的目录。
    QStringList items;
    for (const QString &entry : config.extraClassPath) {
        if (entry.trimmed().isEmpty()) {
            continue;
        }
        const QFileInfo info(entry);
        if (info.isDir()) {
            items << info.absoluteFilePath();
            items << expandJars(QStringList() << entry);
        } else if (info.isFile()) {
            items << info.absoluteFilePath();
        }
    }
    items.removeDuplicates();
    if (items.isEmpty()) {
        return QString();
    }
    return QStringLiteral("-Dahx.extra.classpath=") + items.join(QDir::listSeparator());
}

QString PackerPipeline::buildObfuscatorConfig(const Config &config,
                                              const QStringList &classBlackList,
                                              const QStringList &classBlackRegexList,
                                              const QString &decryptClassName,
                                              bool forUserJar) const {
    auto yamlList = [](const QStringList &items) {
        if (items.isEmpty()) {
            return QStringLiteral(" []");
        }
        QString out;
        for (const QString &item : items) {
            out += QStringLiteral("\n  - \"%1\"").arg(item);
        }
        return out;
    };
    auto yamlBool = [](bool value) {
        return QString::fromLatin1(value ? "true" : "false");
    };

    // 用户 JAR 按界面上的勾选走;验证模块必须全开 ——
    // 它的类名/结构是明文注入那一套机制的基准,关掉任何一项都会让注入的
    // 类名与混淆器认定的名字对不上。
    const auto opt = [forUserJar](bool userValue, bool moduleValue = true) {
        return forUserJar ? userValue : moduleValue;
    };

    QString cfg;
    cfg += QStringLiteral("logLevel: info\n");
    cfg += QStringLiteral("asmAutoCompute: true\n");
    cfg += QStringLiteral("useSpringBoot: false\n");
    cfg += QStringLiteral("useWebWar: false\n");
    cfg += QStringLiteral("obfuscateChars:\n  - \"i\"\n  - \"l\"\n  - \"L\"\n");
    cfg += QStringLiteral("classBlackList:%1\n").arg(yamlList(classBlackList));
    cfg += QStringLiteral("classBlackRegexList:%1\n").arg(yamlList(classBlackRegexList));
    cfg += QStringLiteral("methodBlackList: []\n");
    cfg += QStringLiteral("enableClassName: %1\n").arg(yamlBool(opt(config.enableClassName)));
    cfg += QStringLiteral("enablePackageName: %1\n").arg(yamlBool(opt(config.enablePackageName)));
    cfg += QStringLiteral("enableMethodName: %1\n").arg(yamlBool(opt(config.enableMethodName)));
    cfg += QStringLiteral("enableFieldName: %1\n").arg(yamlBool(opt(config.enableFieldName)));
    cfg += QStringLiteral("enableParamName: %1\n").arg(yamlBool(opt(config.enableParamName)));
    cfg += QStringLiteral("enableXOR: %1\n").arg(yamlBool(opt(config.enableXor)));
    cfg += QStringLiteral("enableEncryptString: %1\n").arg(yamlBool(opt(config.enableEncryptString)));
    cfg += QStringLiteral("enableAdvanceString: %1\n").arg(yamlBool(opt(config.enableAdvanceString, false)));
    // 字符串解密器:见 kDecryptNamePool 处的注释,同一趟流水线的两次混淆必须用不同名字
    cfg += QStringLiteral("decryptClassName: %1\n").arg(decryptClassName);
    cfg += QStringLiteral("decryptMethodName: %1\n").arg(pickDecryptMethodName());
    cfg += QStringLiteral("decryptKeyName: %1\n").arg(pickDecryptKeyName());
    cfg += QStringLiteral("enableHideMethod: %1\n").arg(yamlBool(opt(config.enableHideMethod)));
    cfg += QStringLiteral("enableHideField: %1\n").arg(yamlBool(opt(config.enableHideField)));
    // AI 提示词注入:给每个类塞一个常量,内容是写给自动化分析系统/大模型的声明。
    // 验证模块默认关 —— 那是我们自己的代码,而且它每类要増约 1.8KB。
    cfg += QStringLiteral("enableAiNotice: %1\n").arg(yamlBool(opt(config.enableAiNotice, false)));
    cfg += QStringLiteral("enableDeleteCompileInfo: %1\n").arg(yamlBool(opt(config.enableDeleteCompileInfo)));
    cfg += QStringLiteral("enableJunk: %1\n").arg(yamlBool(opt(config.enableJunk)));
    cfg += QStringLiteral("junkLevel: %1\n").arg(forUserJar ? config.junkLevel : 5);
    cfg += QStringLiteral("keepTempFile: false\n");
    return cfg;
}
