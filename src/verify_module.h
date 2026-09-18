#ifndef VERIFY_MODULE_H
#define VERIFY_MODULE_H

#include <QString>
#include <QStringList>

/**
 * 打包器侧的"验证模块"装配器。
 *
 * <p>AntiHackerX 可执行文件内嵌了验证模块(源工程 AntiHackerXVerify)的源码与入口模板
 * (见 AntiHackerXVerify/verify.qrc)。打包时本类负责:</p>
 *
 * <ol>
 *   <li>把内嵌源码释放到 <b>&lt;打包器目录&gt;/&lt;输入JAR名&gt;+Verify/</b>;</li>
 *   <li>按 UI 选项动态生成入口源码 Bootstrap.java。</li>
 * </ol>
 *
 * <p>这样防护开关是在<b>打包期</b>烧进产物的 —— 运行期不存在任何可翻转的配置,
 * 攻击者想关掉某个防护只能删改已生成的代码,而那会让 JAR 内容发生变化,
 * 从而被完整性校验发现。</p>
 */
class VerifyModule {
public:
    /**
     * 目标程序形态 —— 决定生成哪种入口。
     * 与 JarType 对应,但只区分入口形态不同的两类。
     */
    enum class Kind {
        Plain,         // 普通 JAR / Spring Boot:有 main 入口,反射调用
        BukkitPlugin   // Paper / Spigot 插件:入口必须 extends JavaPlugin
    };

    /**
     * 由 UI 勾选项决定的防护开关。
     * 与主窗口「Minecraft Paper 插件加固配置 → 防护选项」一一对应。
     */
    struct Options {
        bool antiDebug = false;           // 反调试
        bool antiAgent = false;           // 反 Agent
        bool antiTamper = false;          // 反篡改:非类文件签名校验,被改则拒绝加载
        bool antiVirtualMachine = false;  // 反虚拟机内运行
        /** 本次打包生成的 ECDSA P-256 公钥(X.509 SubjectPublicKeyInfo 的 hex)。
         *
         *  <p>由流水线在<b>装配入口类之前</b>生成并塞进来。它作为字符串字面量
         *  出现在入口类的静态块里,而入口类会被 native-obfuscator 整类搬进原生库,
         *  于是这把公钥最终落在 {@code string_pool} 里 —— 想换掉它就得改二进制,
         *  而不是改一个 Java 常量。</p> */
        QString tamperPubKeyHex;
        /** 启动时展示加壳版权提示。
         *  有桌面环境弹窗,服务器等无头环境自动退化成日志输出。 */
        bool showCopyright = true;
        Kind kind = Kind::Plain;          // 目标程序形态

        /** 是否有任何一项被勾选 */
        bool anyEnabled() const {
            return antiDebug || antiAgent || antiTamper || antiVirtualMachine;
        }
    };

    /** 装配结果 */
    struct Result {
        bool ok = false;
        QString workDir;              // <打包器目录>/<输入JAR名>+Verify
        QString bootstrapPath;        // 生成的入口源码路径
        QString bootstrapClassName;   // 入口类全限定名
        QString keyHex;               // 本次为加密载荷生成的 AES-256 密钥
        QString templateUsed;         // 实际使用的入口模板
        QStringList extractedSources; // 释放出来的验证模块源码
        QStringList guardCalls;       // 实际生成的装配调用(用于日志/核对)
        /** 各包里那个「定义器」类的简单名。每次打包随机,不是固定特征。 */
        QString linkName;
        /** 生成的定义器类全限定名。流水线要把它们加进混淆/NOBF 的黑名单,
         *  否则被改名后运行时就算不出该找哪个类了。 */
        QStringList definerClasses;
        QString errorMessage;

        /** 人类可读摘要 */
        QString summary() const;
    };

    /** 形态名称(用于日志) */
    static QString kindName(Kind kind);

    /**
     * 计算工作目录:<打包器目录>/<输入JAR去掉扩展名的文件名>+Verify
     *
     * @param packerDir    打包器可执行文件所在目录
     * @param inputJarPath 待加壳的 JAR
     */
    static QString workDirFor(const QString &packerDir, const QString &inputJarPath);

    /**
     * 入口类的全限定名(便于写 MANIFEST 的 Main-Class)。
     * 原入口若在默认包中,生成的入口也必须在默认包 ——
     * 因为 Java 里命名包的类无法引用默认包中的类。
     */
    static QString bootstrapClassNameFor(const QString &originalMainClass);

    /**
     * 释放验证模块源码 + 按包生成定义器 + 生成入口源码。
     *
     * @param inputJarPath      待加壳的 JAR
     * @param originalMainClass 原程序入口全限定名(如 top.h3k4.unpassMain)
     * @param options           按 UI 勾选装配
     * @param payloadPackages   载荷里出现过的包名(点分隔;默认包传空串)。
     *                          每个包会生成一个定义器类 —— 解密出来的类必须由
     *                          宿主类加载器定义,而 Lookup#defineClass 要求同包。
     * @return 装配结果;ok 为 false 时 errorMessage 说明原因
     */
    static Result prepare(const QString &inputJarPath,
                          const QString &originalMainClass,
                          const Options &options,
                          const QStringList &payloadPackages = QStringList());

private:
    // 内嵌资源的释放表:资源路径 -> 相对工作目录的目标路径
    struct ResourceEntry {
        const char *resource;
        const char *relativePath;
    };

    static const ResourceEntry kEmbeddedResources[];

    static bool extractResource(const QString &resourcePath,
                                const QString &targetPath,
                                QString &errorMessage);

    static QString buildGuardCalls(const Options &options, QStringList &outCalls);

    /** 生成定义器源码(每个载荷包一个)。返回生成的文件路径列表。 */
    static bool generateDefiners(const QString &workDir,
                                 const QStringList &payloadPackages,
                                 const QString &linkName,
                                 QStringList &outPaths,
                                 QStringList &outClasses,
                                 QString &errorMessage);

    /** 随机生成一个能混进混淆类名里的定义器简单名 */
    static QString generateLinkName();

    /** 生成 32 字节随机密钥的十六进制形式(每份产物不同) */
    static QString generateKeyHex();

    static QString buildBootstrapSource(const QString &inputJarPath,
                                        const QString &originalMainClass,
                                        const QString &keyHex,
                                        const QStringList &definerClasses,
                                        const Options &options,
                                        QStringList &outCalls,
                                        QString &errorMessage);

    /** 定义器类名单 -> Java 字符串数组字面量 */
    static QString buildDefinerArray(const QStringList &definerClasses);
};

#endif // VERIFY_MODULE_H
