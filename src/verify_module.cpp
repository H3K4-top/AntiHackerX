#include "verify_module.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTextStream>

#include <algorithm>

namespace {

/** 生成的入口类所在包(原入口不在默认包时使用) */
const char *const kBootstrapPackage = "top.h3k4";

/** 生成的入口类名 */
const char *const kBootstrapClass = "Bootstrap";

/** 内嵌资源根路径 */
const char *const kResourceRoot = ":/verify";

/** 普通 JAR 入口模板 */
const char *const kBootstrapTemplate = ":/verify/templates/Bootstrap.java.tmpl";

/** Paper 插件入口模板(入口须 extends JavaPlugin) */
const char *const kPluginBootstrapTemplate =
        ":/verify/templates/PluginBootstrap.java.tmpl";
/// Fabric MOD 的入口模板(PreLaunchEntrypoint)
const char *const kFabricBootstrapTemplate =
        ":/verify/templates/FabricBootstrap.java.tmpl";

/** Java 源码里每个语句的缩进(位于 main 方法体内) */
const char *const kIndent = "        ";

/** 生成 n 字节密码学随机数据 */
QByteArray randomBytes(int n) {
    QByteArray out;
    out.reserve(n);
    while (out.size() < n) {
        const quint32 word = QRandomGenerator::system()->generate();
        for (int i = 0; i < 4 && out.size() < n; ++i) {
            out.append(static_cast<char>((word >> (8 * i)) & 0xFF));
        }
    }
    return out;
}

/** 十六进制字符串转字节 */
QByteArray hexToBytes(const QString &hex) {
    QByteArray out;
    out.reserve(hex.size() / 2);
    for (int i = 0; i + 1 < hex.size(); i += 2) {
        out.append(static_cast<char>(hex.mid(i, 2).toUInt(nullptr, 16)));
    }
    return out;
}

/** 字节序列格式化成 Java 的 int[] 字面量(每 4 字节一个大端 int) */
QString formatIntArray(const QByteArray &bytes) {
    QStringList words;
    for (int i = 0; i + 3 < bytes.size(); i += 4) {
        const quint32 word = (static_cast<quint32>(static_cast<quint8>(bytes[i])) << 24)
                             | (static_cast<quint32>(static_cast<quint8>(bytes[i + 1])) << 16)
                             | (static_cast<quint32>(static_cast<quint8>(bytes[i + 2])) << 8)
                             | static_cast<quint32>(static_cast<quint8>(bytes[i + 3]));
        // 固定 8 位十六进制:Java 允许 0xFFFFFFFF 这样的 int 字面量(按补码解释)
        words << QStringLiteral("0x") + QString::number(word, 16).rightJustified(8, QLatin1Char('0'));
    }
    return QStringLiteral("{ ") + words.join(QStringLiteral(", ")) + QStringLiteral(" }");
}

/**
 * 把 32 字节密钥拆成三个分片。
 *
 * <p>构造方式(与 {@code AhxRuntime.deriveKey} 对应):</p>
 * <pre>
 *   S0 = K  ^ r0          // 直接存
 *   S1 = r0 ^ r1          // 字节序反转后存
 *   S2 = r1               // 逐字节取反后存
 * </pre>
 * <p>其中 r0、r1 是随机量且<b>不落盘</b>,运行时异或时会自然抵消:
 * {@code (K^r0) ^ (r0^r1) ^ r1 == K}。</p>
 */
void buildKeyShards(const QByteArray &key,
                    QString &outShard0,
                    QString &outShard1,
                    QString &outShard2) {
    const QByteArray r0 = randomBytes(key.size());
    const QByteArray r1 = randomBytes(key.size());

    QByteArray s0;
    QByteArray s1;
    QByteArray s2;
    s0.reserve(key.size());
    s1.reserve(key.size());
    s2.reserve(key.size());

    for (int i = 0; i < key.size(); ++i) {
        s0.append(static_cast<char>(key[i] ^ r0[i]));
        s1.append(static_cast<char>(r0[i] ^ r1[i]));
        s2.append(static_cast<char>(r1[i]));
    }

    // 第 2 层:字节序反转后存放(运行时再反转回来)
    std::reverse(s1.begin(), s1.end());
    // 第 3 层:逐字节取反后存放(运行时再取反回来)
    for (int i = 0; i < s2.size(); ++i) {
        s2[i] = static_cast<char>(~s2[i]);
    }

    outShard0 = formatIntArray(s0);
    outShard1 = formatIntArray(s1);
    outShard2 = formatIntArray(s2);
}

} // namespace

// 内嵌的验证模块源码清单(来自 AntiHackerXVerify 工程)。
// 资源路径固定为 :/verify/<relativePath>,目标落在 <工作目录>/<relativePath>。
const VerifyModule::ResourceEntry VerifyModule::kEmbeddedResources[] = {
    // 防护守卫
    { ":/verify/java/top/h3k4/VirtualMachineGuard.java", "java/top/h3k4/VirtualMachineGuard.java" },
    { ":/verify/java/top/h3k4/AgentGuard.java",          "java/top/h3k4/AgentGuard.java"          },
    { ":/verify/java/top/h3k4/DebugTimerGuard.java",     "java/top/h3k4/DebugTimerGuard.java"     },
    // 解密运行时
    { ":/verify/java/top/h3k4/AhxPayload.java",     "java/top/h3k4/AhxPayload.java"     },
    { ":/verify/java/top/h3k4/AhxRuntime.java",     "java/top/h3k4/AhxRuntime.java"     },
    // 反篡改:完整性签名校验(入口类的静态块会调它,清单里漏了就直接编不过)
    { ":/verify/java/top/h3k4/AhxSignature.java",   "java/top/h3k4/AhxSignature.java"   },
    // 启动时的加壳版权提示
    { ":/verify/java/top/h3k4/CopyrightNotice.java", "java/top/h3k4/CopyrightNotice.java" },
    // 入口模板(Fabric 那份必须在表里:漏了就会在打包时毙掉,
    // 因为模板是从 Qt 资源里读的)
    { ":/verify/templates/FabricBootstrap.java.tmpl", "templates/FabricBootstrap.java.tmpl" },
};

// ---------------------------------------------------------------------------
// 路径与命名
// ---------------------------------------------------------------------------

QString VerifyModule::workDirFor(const QString &packerDir, const QString &inputJarPath) {
    const QFileInfo info(inputJarPath);
    // completeBaseName():Foo.bar.jar -> Foo.bar,比 baseName() 少切一层
    const QString base = info.completeBaseName();
    return QDir(packerDir).filePath(base + QStringLiteral("+Verify"));
}

QString VerifyModule::bootstrapClassNameFor(const QString &originalMainClass) {
    QString mainClass = originalMainClass.trimmed();
    mainClass.replace(QLatin1Char('/'), QLatin1Char('.'));

    // 原入口在默认包(类名里没有点)时,生成的入口也必须在默认包 ——
    // Java 不允许命名包中的类引用默认包中的类。
    if (mainClass.isEmpty() || mainClass.indexOf(QLatin1Char('.')) < 0) {
        return QString::fromLatin1(kBootstrapClass);
    }
    return QString::fromLatin1(kBootstrapPackage) + QLatin1Char('.')
           + QString::fromLatin1(kBootstrapClass);
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------

VerifyModule::Result VerifyModule::prepare(const QString &inputJarPath,
                                           const QString &originalMainClass,
                                           const Options &options,
                                           const QStringList &payloadPackages) {
    Result result;

    if (inputJarPath.isEmpty() || !QFileInfo::exists(inputJarPath)) {
        result.errorMessage = QStringLiteral("输入 JAR 不存在:%1").arg(inputJarPath);
        return result;
    }
    if (originalMainClass.trimmed().isEmpty()) {
        result.errorMessage = QStringLiteral("无法确定原程序入口,不能生成加壳入口");
        return result;
    }

    const QString packerDir = QCoreApplication::applicationDirPath();
    result.workDir = workDirFor(packerDir, inputJarPath);
    result.bootstrapClassName = bootstrapClassNameFor(originalMainClass);

    // 1) 重建工作目录。
    //    这里必须先清空:目录内容完全由「内嵌资源 + 本次选项」推导而来,
    //    若留着上一次的产物,切换原主类后会残留旧的入口源码
    //    (例如旧的是默认包 Bootstrap.java,新的是 ahx/generated/Bootstrap.java),
    //    javac 会因两个同名类而报错。
    {
        const QFileInfo workInfo(result.workDir);
        // 安全兜底:只清理形如 <名字>+Verify 的目录,防止路径算错时误删
        if (!workInfo.fileName().endsWith(QStringLiteral("+Verify"))) {
            result.errorMessage = QStringLiteral("工作目录名异常,拒绝清理:%1").arg(result.workDir);
            return result;
        }
        if (workInfo.exists()) {
            if (!QDir(result.workDir).removeRecursively()) {
                result.errorMessage = QStringLiteral("无法清理旧的工作目录:%1").arg(result.workDir);
                return result;
            }
        }
        if (!QDir().mkpath(result.workDir)) {
            result.errorMessage = QStringLiteral("无法创建工作目录:%1").arg(result.workDir);
            return result;
        }
    }

    // 2) 释放验证模块源码
    for (const ResourceEntry &entry : kEmbeddedResources) {
        const QString target = QDir(result.workDir).filePath(QString::fromLatin1(entry.relativePath));
        QDir().mkpath(QFileInfo(target).absolutePath());

        QString error;
        if (!extractResource(QString::fromLatin1(entry.resource), target, error)) {
            result.errorMessage = QStringLiteral("释放验证模块源码失败:%1").arg(error);
            return result;
        }
        result.extractedSources << target;
    }

    // 3) 生成本次产物专用的随机密钥与定义器名,再生成入口源码
    result.keyHex = generateKeyHex();
    result.linkName = generateLinkName();
    result.templateUsed = (options.kind == Kind::BukkitPlugin)
            ? QStringLiteral("PluginBootstrap.java.tmpl")
            : (options.kind == Kind::FabricMod)
              ? QStringLiteral("FabricBootstrap.java.tmpl")
              : QStringLiteral("Bootstrap.java.tmpl");

    // 每个载荷包一个定义器 —— 解密出来的类必须由宿主类加载器定义,
    // 而 Lookup#defineClass 要求同包。
    {
        QString error;
        if (!generateDefiners(result.workDir, payloadPackages, result.linkName,
                              result.extractedSources, result.definerClasses, error)) {
            result.errorMessage = error;
            return result;
        }
    }

    QString error;
    const QString source = buildBootstrapSource(inputJarPath, originalMainClass,
                                                result.keyHex, result.definerClasses,
                                                options, result.guardCalls, error);
    if (!error.isEmpty()) {
        result.errorMessage = error;
        return result;
    }

    // 入口源码的目录跟着它自己的包走
    const int lastDot = result.bootstrapClassName.lastIndexOf(QLatin1Char('.'));
    const QString packagePath = (lastDot < 0)
            ? QString()
            : result.bootstrapClassName.left(lastDot).replace(QLatin1Char('.'), QLatin1Char('/'));

    const QString fileName = QString::fromLatin1(kBootstrapClass) + QStringLiteral(".java");
    const QString relativePath = packagePath.isEmpty()
            ? fileName
            : packagePath + QLatin1Char('/') + fileName;

    result.bootstrapPath = QDir(result.workDir).filePath(relativePath);
    QDir().mkpath(QFileInfo(result.bootstrapPath).absolutePath());

    QFile out(result.bootstrapPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.errorMessage = QStringLiteral("无法写入入口源码:%1").arg(result.bootstrapPath);
        return result;
    }
    {
        QTextStream stream(&out);
        stream.setEncoding(QStringConverter::Utf8);
        stream << source;
    }
    out.close();

    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// 资源释放
// ---------------------------------------------------------------------------

bool VerifyModule::extractResource(const QString &resourcePath,
                                   const QString &targetPath,
                                   QString &errorMessage) {
    QFile in(resourcePath);
    if (!in.exists()) {
        errorMessage = QStringLiteral("内嵌资源缺失:%1(检查 verify.qrc 是否已加入构建)")
                           .arg(resourcePath);
        return false;
    }
    if (!in.open(QIODevice::ReadOnly)) {
        errorMessage = QStringLiteral("无法读取内嵌资源:%1").arg(resourcePath);
        return false;
    }
    const QByteArray data = in.readAll();
    in.close();

    QFile out(targetPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        errorMessage = QStringLiteral("无法写入:%1").arg(targetPath);
        return false;
    }
    out.write(data);
    out.close();

    if (out.error() != QFile::NoError) {
        errorMessage = QStringLiteral("写入失败:%1(%2)").arg(targetPath, out.errorString());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 入口代码生成
// ---------------------------------------------------------------------------

QString VerifyModule::buildGuardCalls(const Options &options, QStringList &outCalls) {
    const QString indent = QString::fromLatin1(kIndent);
    QString block;

    auto appendCall = [&](const QString &comment, const QString &call) {
        if (!block.isEmpty()) {
            block += QLatin1Char('\n');
        }
        block += indent + QStringLiteral("// ") + comment + QLatin1Char('\n');
        block += indent + call + QLatin1Char('\n');
        outCalls << call;
    };

    // 顺序与 UI 上的「防护选项」保持一致
    if (options.antiDebug) {
        appendCall(QStringLiteral("反调试:计时器守卫(检测断点冻结)"),
                   QStringLiteral("top.h3k4.DebugTimerGuard.start();"));
    }
    if (options.antiAgent) {
        appendCall(QStringLiteral("反 Agent:启动参数检查"),
                   QStringLiteral("top.h3k4.AgentGuard.check();"));
    }
    if (options.antiTamper) {
        if (options.tamperPubKeyHex.isEmpty()) {
            // 没有公钥就没法验签 —— 说明流水线没走到生成密钥那一步。
            // 宁可明确记一笔,也不要生成一个必然抛异常的调用。
            outCalls << QStringLiteral("反篡改:(缺少签名公钥,已跳过)");
        } else {
            appendCall(
                QStringLiteral(
                    "反篡改:ECDSA-P256 签名校验(校 p.dat / *.so / plugin.yml 等全部"
                    "非类文件;公钥以字面量传入,最终落进原生库)"),
                QStringLiteral("top.h3k4.AhxSignature.verify(\"%1\");")
                        .arg(options.tamperPubKeyHex));
        }
    }
    if (options.antiVirtualMachine) {
        appendCall(QStringLiteral("反虚拟机:固件指纹检测"),
                   QStringLiteral("top.h3k4.VirtualMachineGuard.check();"));
    }
    // 版权提示放最后:守卫可能 exit(-1) 或直接把 JVM 打崩,
    // 那种情况下本来也不该弹版权框。
    if (options.showCopyright) {
        if (options.kind == Kind::BukkitPlugin) {
            // 服务器上**绝不能**弹 Swing 框 —— 那会阻塞插件启用,直到有人跑去
            // 那台机器的桌面上点"确定"。CopyrightNotice 自己会检测无头环境,
            // 但"装了桌面的服务器"是有 DISPLAY 的,所以这里直接替它关掉弹窗。
            appendCall(QStringLiteral("版权提示:服务器上改为只写日志(避免弹框阻塞插件启用)"),
                       QStringLiteral("System.setProperty(\"antihackerx.splash\", \"false\");"));
        }
        appendCall(QStringLiteral("加壳版权提示(无桌面环境时自动退化为日志输出)"),
                   QStringLiteral("top.h3k4.CopyrightNotice.show();"));
    }

    if (block.isEmpty()) {
        block = indent + QStringLiteral("// 未勾选任何防护选项\n");
    }
    return block;
}

QString VerifyModule::kindName(Kind kind) {
    switch (kind) {
        case Kind::BukkitPlugin:
            return QStringLiteral("Bukkit/Paper 插件");
        case Kind::FabricMod:
            return QStringLiteral("Fabric MOD");
        case Kind::Plain:
        default:
            return QStringLiteral("普通 JAR");
    }
}

QString VerifyModule::generateKeyHex() {
    // 每份产物一把新密钥,避免一把泄露后影响历史产物
    return QString::fromLatin1(randomBytes(32).toHex());
}

QString VerifyModule::buildBootstrapSource(const QString &inputJarPath,
                                           const QString &originalMainClass,
                                           const QString &keyHex,
                                           const QStringList &definerClasses,
                                           const Options &options,
                                           QStringList &outCalls,
                                           QString &errorMessage) {
    // 按目标程序形态选入口模板:插件入口必须 extends JavaPlugin,
    // Fabric 的入口必须 implements PreLaunchEntrypoint,
    // 普通程序则走 main + 解密加载器。
    const char *templatePath = nullptr;
    switch (options.kind) {
        case Kind::BukkitPlugin:
            templatePath = kPluginBootstrapTemplate;
            break;
        case Kind::FabricMod:
            templatePath = kFabricBootstrapTemplate;
            break;
        case Kind::Plain:
        default:
            templatePath = kBootstrapTemplate;
            break;
    }

    QFile tmpl(QString::fromLatin1(templatePath));
    if (!tmpl.exists()) {
        errorMessage = QStringLiteral("内嵌入口模板缺失:%1")
                           .arg(QString::fromLatin1(templatePath));
        return QString();
    }
    if (!tmpl.open(QIODevice::ReadOnly)) {
        errorMessage = QStringLiteral("无法读取入口模板");
        return QString();
    }
    QString source = QString::fromUtf8(tmpl.readAll());
    tmpl.close();

    QString mainClass = originalMainClass.trimmed();
    mainClass.replace(QLatin1Char('/'), QLatin1Char('.'));

    const bool defaultPackage = (mainClass.indexOf(QLatin1Char('.')) < 0);
    const QString packageLine = defaultPackage
            ? QString()
            : QStringLiteral("package %1;").arg(QString::fromLatin1(kBootstrapPackage));

    const QString guardCalls = buildGuardCalls(options, outCalls);

    source.replace(QStringLiteral("{{TIMESTAMP}}"),
                   QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    source.replace(QStringLiteral("{{INPUT_JAR}}"), QFileInfo(inputJarPath).fileName());
    source.replace(QStringLiteral("{{PACKAGE_LINE}}"), packageLine);
    // 守卫与运行时的调用一律用全限定名,因此不需要 import
    source.replace(QStringLiteral("{{IMPORTS}}"), QString());
    source.replace(QStringLiteral("{{CLASS}}"), QString::fromLatin1(kBootstrapClass));
    source.replace(QStringLiteral("{{GUARD_CALLS}}"), guardCalls);

    // 密钥以三个分片形式注入,不明文出现
    QString shard0;
    QString shard1;
    QString shard2;
    buildKeyShards(hexToBytes(keyHex), shard0, shard1, shard2);
    source.replace(QStringLiteral("{{SHARD0}}"), shard0);
    source.replace(QStringLiteral("{{SHARD1}}"), shard1);
    source.replace(QStringLiteral("{{SHARD2}}"), shard2);

    source.replace(QStringLiteral("{{TARGET_CLASS}}"), mainClass);
    source.replace(QStringLiteral("{{DEFINERS}}"), buildDefinerArray(definerClasses));

    return source;
}

QString VerifyModule::buildDefinerArray(const QStringList &definerClasses) {
    if (definerClasses.isEmpty()) {
        return QStringLiteral("new String[0]");
    }
    QString out = QStringLiteral("{\n");
    for (int i = 0; i < definerClasses.size(); ++i) {
        out += QStringLiteral("            \"%1\"%2\n")
                       .arg(definerClasses.at(i),
                            i + 1 < definerClasses.size() ? QStringLiteral(",")
                                                          : QString());
    }
    out += QStringLiteral("    }");
    return out;
}

// ---------------------------------------------------------------------------
// 定义器(把解密出来的类定义进宿主类加载器)
// ---------------------------------------------------------------------------

QString VerifyModule::generateLinkName() {
    // 混进混淆类名里(混淆器的字符集是 i / l / L,这里再多一个 1),
    // 长度取 12:4^12 ≈ 1680 万,和同一个包里的载荷类名楜车概率可忽略。
    static const char kAlphabet[] = "ilL1";
    const int first = QRandomGenerator::global()->bounded(3);   // 首字符必须是合法标识符开头
    QString name;
    name += QLatin1Char(kAlphabet[first]);
    for (int i = 1; i < 12; ++i) {
        name += QLatin1Char(kAlphabet[QRandomGenerator::global()->bounded(4)]);
    }
    return name;
}

bool VerifyModule::generateDefiners(const QString &workDir,
                                    const QStringList &payloadPackages,
                                    const QString &linkName,
                                    QStringList &outPaths,
                                    QStringList &outClasses,
                                    QString &errorMessage) {
    // 默认包也要一个定义器(载荷里可能有默认包的类)
    QStringList packages;
    for (const QString &pkg : payloadPackages) {
        const QString trimmed = pkg.trimmed();
        if (!packages.contains(trimmed)) {
            packages << trimmed;
        }
    }
    if (packages.isEmpty()) {
        return true;   // 没有载荷包可定义
    }

    for (const QString &pkgRaw : packages) {
        const bool defaultPackage = pkgRaw.isEmpty();
        const QString packagePath = defaultPackage
                ? QString()
                : QString(pkgRaw).replace(QLatin1Char('.'), QLatin1Char('/'));

        // 默认包的类直接放工作目录根上(入口类也在这),不放进 java/ 子树
        const QString relative = defaultPackage
                ? linkName + QStringLiteral(".java")
                : QStringLiteral("java/") + packagePath + QLatin1Char('/')
                          + linkName + QStringLiteral(".java");
        const QString path = QDir(workDir).filePath(relative);
        QDir().mkpath(QFileInfo(path).absolutePath());

        QString source;
        source += QStringLiteral("/* 由 AntiHackerX 生成 —— 请勿手工修改。 */\n");
        if (!defaultPackage) {
            source += QStringLiteral("package %1;\n").arg(pkgRaw);
        }
        source += QStringLiteral("\n");
        source += QStringLiteral(
                "/**\n"
                " * 本包内的「定义器」。\n"
                " *\n"
                " * <p>加壳运行时靠它把解密出来的同包类定义进<b>本程序自己的类加载器</b>。\n"
                " * 这是必须的:{@code JavaPlugin} 的构造器要求实例的类加载器必须是\n"
                " * {@code PluginClassLoader},而自己写的 ClassLoader 永远过不了那条\n"
                " * {@code instanceof} 检查。Java 9 起的\n"
                " * {@code MethodHandles.Lookup#defineClass} 能把类定义到 lookup 所属\n"
                " * 类的加载器与包里,所以每个包需要一个这样的引子。</p>\n"
                " */\n");
        source += QStringLiteral("public final class %1 {\n").arg(linkName);
        source += QStringLiteral(
                "    /** 本类的 Lookup,由运行时反射取用 */\n"
                "    public static final java.lang.invoke.MethodHandles.Lookup l =\n"
                "            java.lang.invoke.MethodHandles.lookup();\n"
                "\n"
                "    private %1() {\n"
                "    }\n"
                "}\n").arg(linkName);

        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            errorMessage = QStringLiteral("无法写入定义器源码:%1").arg(path);
            return false;
        }
        out.write(source.toUtf8());
        out.close();

        outPaths << path;
        outClasses << (defaultPackage ? linkName : pkgRaw + QLatin1Char('.') + linkName);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 摘要
// ---------------------------------------------------------------------------

QString VerifyModule::Result::summary() const {
    if (!ok) {
        return QStringLiteral("装配失败:%1").arg(errorMessage);
    }

    QString text;
    text += QStringLiteral("工作目录: %1\n").arg(workDir);
    text += QStringLiteral("释放源码: %1 个\n").arg(extractedSources.size());
    text += QStringLiteral("入口类名: %1\n").arg(bootstrapClassName);
    text += QStringLiteral("入口模板: %1\n").arg(templateUsed);
    text += QStringLiteral("入口文件: %1\n").arg(bootstrapPath);
    text += QStringLiteral("载荷密钥: %1\n").arg(keyHex);
    text += QStringLiteral("定义器名: %1(%2 个)\n")
                    .arg(linkName).arg(definerClasses.size());
    text += QStringLiteral("装配调用:");
    if (guardCalls.isEmpty()) {
        text += QStringLiteral(" (无)");
    } else {
        for (const QString &call : guardCalls) {
            text += QStringLiteral("\n  • %1").arg(call);
        }
    }
    return text;
}
