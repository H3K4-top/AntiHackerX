#ifndef PACKER_PIPELINE_H
#define PACKER_PIPELINE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>

#include "verify_module.h"

/**
 * 加壳流水线编排器。
 *
 * <p>按固定顺序调用一串外部工具,产出最终的加壳 JAR。全部通过 QProcess 调用外部
 * 程序完成,与 GPL 组件保持进程隔离(沿用 ObfuscatorController 的思路)。</p>
 *
 * <p><b>各步骤与顺序(顺序不能乱):</b></p>
 * <pre>
 *  1. 混淆用户 JAR            jar-obfuscator → user.jar + mapping
 *  2. 解析映射 → TARGET       用户主类改名后的新名字;同时算出选中类的新名字
 *  3. 释放验证模块源码 + 生成入口(注入密钥分片与 TARGET)
 *  4. javac 编译              --release 8 -proc:none
 *  5. 打包验证模块 JAR
 *  6. 混淆验证模块 JAR        入口类进 classBlackList(保名)
 *  7. NOBF 转 C++:验证模块    → native0
 *  8. NOBF 转 C++:选中的用户类 → native1(仅当用户勾选了要隐藏的类)
 *  9. zig 交叉编译 native 库   按目标平台产出 .so / .dll
 * 10. 修补两个 Loader 并回填 JAR
 * 11. AhxPacker:加密用户 JAR + 注入验证模块 + 改写 plugin.yml → 输出 JAR
 * </pre>
 *
 * <p><b>为什么"先混淆、后转 C++"</b>:转成 C++ 后,原生字符串池里存的是类名
 * (实测 {@code native_jvm.cpp} 里能找到 {@code top.h3k4.AgentGuard} 等),
 * 运行时按名字查类。先转再改名会让 native 代码找不到类。</p>
 *
 * <p><b>为什么跑两次 NOBF</b>:两次会各自生成一个 {@code nativeN/Loader} +
 * {@code nativeN/x64-*.so},靠 {@code --custom-lib-dir} 把它们钉在 native0 与
 * native1,互不干扰。若把用户类和模块合到一个 JAR 里跑一次,就得把选中类的
 * native stub 注入到应用类路径上 —— 而它引用的其他用户类是被加密进载荷的,
 * 应用类路径看不见,一调用就 NoClassDefFoundError。所以 stub 必须留在载荷里。</p>
 */
class PackerPipeline : public QObject {
    Q_OBJECT

public:
    /** 目标平台(决定 zig 交叉编译产出哪些原生库) */
    enum class Target {
        WindowsX64,
        LinuxX64,
        Both
    };

    struct Config {
        // 输入 / 输出
        QString inputJar;
        QString outputJar;
        QString workDir;             // 中间产物目录

        /** 用户程序的原始主类全限定名(用于在改名映射里定位它的新名字) */
        QString originalMainClass;

        /** 要额外转成原生代码的用户类(原始全限定名,点分隔)。
         *  空列表 = 只对验证模块做原生化(旧行为)。
         *  这些类会被 NOBF 转成 native stub,而 stub 仍然留在加密载荷里,
         *  由 AhxClassLoader 加载,详见类注释。 */
        QStringList hideClasses;

        /** 额外的编译期类路径:服务端 JAR / Bukkit-API / libraries 目录。
         *
         *  为什么非要它:Bukkit 插件对服务端 API 是 provided 作用域,
         *  {@code org.bukkit.*} 根本不在插件 JAR 里。而验证模块的 Paper 入口
         *  要 {@code import org.bukkit.plugin.java.JavaPlugin} ——
         *  不给这一份就编不过。NOBF 转选中类时同样需要(算栈帧要解析父类)。
         *
         *  每个条目可以是 .jar 文件,也可以是目录(目录会被递归展开)。 */
        QStringList extraClassPath;

        // 外部工具
        QString javaExe;             // java
        QString javacExe;            // javac
        QString jarExe;              // jar
        QString javaHome;            // 用于定位 JNI 头文件
        QString zigExe;              // zig(交叉编译 C++)
        QString obfuscatorJar;       // jar-obfuscator-*-jar-with-dependencies.jar
        QString nobfJar;             // native-obfuscator.jar
        QString ahxPackerSrc;        // AhxPacker.java(用 java 单文件模式执行)

        // 防护配置
        VerifyModule::Options verifyOptions;
        Target target = Target::Both;

        // 混淆选项。默认只开"性价比"最好的那几项 ——
        // 之前默认全开被发现"混淆过头":包名/方法名/字段名/参数名都改,
        // 与依赖反射、序列化、第三方库的插件极易撞车。
        bool enableClassName = true;          // 类名混淆(含引用修正)
        bool enablePackageName = false;       // 包名混淆
        bool enableMethodName = false;        // 方法名混淆
        bool enableFieldName = false;         // 字段名混淆
        bool enableParamName = false;         // 方法参数名混淆
        bool enableDeleteCompileInfo = false; // 删除编译调试信息
        bool enableEncryptString = true;      // 字符串 AES 加密 + 运行时解密
        bool enableAdvanceString = false;     // 字符串改为访问全局列表
        bool enableXor = false;               // 整型常数多重异或混淆
        bool enableJunk = true;               // 垃圾代码
        int junkLevel = 2;                    // 垃圾代码级别(默认 L2)
        bool enableHideMethod = true;         // IDEA 反编译时隐藏方法
        bool enableHideField = true;          // IDEA 反编译时隐藏字段
    };

    explicit PackerPipeline(QObject *parent = nullptr);

    /** 在后台线程启动流水线(立即返回)。同一实例同时只能跑一次。 */
    void startAsync(const Config &config);

    /** 请求中止;当前外部进程会被杀掉。 */
    void requestStop();

    bool isRunning() const { return m_running; }

    /** 用应用目录推导默认的工具路径(找不到的填空串) */
    static Config defaultConfig();

signals:
    /** 进入第 index 步(从 1 开始) */
    void staged(int index, int total, const QString &name);
    /** 子进程输出的一行 */
    void log(const QString &line);
    /** 全部结束 */
    void finished(bool ok, const QString &message);

private:
    void run(const Config &config);

    // 各步骤(返回 false 时 errorMessage 说明原因)
    bool stepObfuscateUser(const Config &config, const QString &workDir,
                           QString &userJar, QString &mappingFile, QString &errorMessage);
    bool stepResolveTarget(const QString &mappingFile, const QString &originalMain,
                           QString &targetClass, QString &errorMessage);
    bool stepPrepareModule(const Config &config, const QString &targetClass,
                           const QStringList &payloadPackages,
                           const QString &tamperPubKeyHex,
                           QString &workDir, QString &keyHex,
                           QStringList &definerClasses, QString &errorMessage);

    /** 生成反篡改签名密钥对(ECDSA P-256)。
     *
     *  <p>公钥写进 {@code <buildDir>/ahx-sign.pub} 并回填给调用方(随后被当作
     *  字符串字面量烧进入口类);私钥留在 {@code <buildDir>/ahx-sign.key},
     *  只在最终打包时递给 AhxPacker,打包成功后由流水线删除。</p>
     *
     *  <p>为什么每份产物一对新钥匙:共用一把就意味着私钥要随工具分发到用户机器上,
     *  拿到它就能伪造签名 —— 等于没有签名。</p> */
    bool stepGenerateSignKey(const Config &config, const QString &buildDir,
                             QString &pubKeyHex, QString &errorMessage);
    /** 编译验证模块。
     *  @param definerClasses 各载荷包的定义器全限定名。编译完成后它们会被
     *        从 _classes 里**摘出来**存到 moduleDir/_definers,交给 run()
     *        在最后一步直接注入明文区 —— 绝不能进混淆器。 */
    bool stepCompileModule(const Config &config, const QString &moduleDir,
                           const QString &targetClass,
                           const QStringList &definerClasses, QString &moduleJar,
                           QString &errorMessage);
    bool stepObfuscateModule(const Config &config, const QString &moduleJar,
                             const QString &entryClass,
                             const QStringList &keepNames,
                             QString &obfJar, QString &errorMessage);
    /** 跑一次 NOBF:把 inputJar 里的部分/全部类转成 C++。
     *  @param nativeDirName native 包目录名。**每份产物随机**(见
     *                       packer_pipeline.cpp 的 randomNativeName),
     *                       否则 NOBF 会自己挑 native0,两份产物就会撞。
     *  @param loaderName    加载器类的简单名(默认 Loader 会让两份产物撞车 ——
     *                       合成隐藏类是 bootstrap 加载器里的全局名字)。
     *  @param hiddenName    合成隐藏类的简单名前缀(同上)。
     *  @param whiteList     空串 = 不传白名单(全部转换);
     *                       否则是逐条规则的文件,注意**空文件等于什么都不转**。 */
    bool stepNativeConvert(const Config &config, const QString &inputJar,
                           const QString &outDir, const QString &nativeDirName,
                           const QString &loaderName, const QString &hiddenName,
                           const QString &whiteList, const QString &libsDir,
                           const QStringList &blackList,
                           QString &cppDir, QString &nativeJar, QString &errorMessage);
    bool stepBuildNative(const Config &config, const QString &cppDir,
                         QString &errorMessage);

    /** 并行把每个 .cpp 单独编成 .o。
     *
     *  <p>为什么不直接一条 `zig c++ a.cpp b.cpp ... -o lib.so`:那样 clang 在
     *  单个进程里**串行**编几百个文件 —— 只吃一个核(风扇狂转但很慢),而且
     *  在结束前一行输出都没有,用户完全看不出是在跑还是卡死。</p>
     *
     *  @param commonArgs 不含 -c / 输入 / 输出的公共参数
     *  @param objects    产出:编好的 .o 路径(顺序不保证)
     */
    bool compileObjects(const Config &config, const QStringList &sources,
                        const QStringList &commonArgs, const QString &objDir,
                        const QString &label, QStringList &objects,
                        QString &errorMessage);

    bool stepPatchLoader(const Config &config, const QString &nativeDir,
                         QString &errorMessage);
    /** 最后一步:加密载荷 + 注入明文 + 写 plugin.yml / MANIFEST。
     *
     *  @param mainClass   真实插件主类(plugin.yml 的 main 保持指向它)。
     *                     Paper 必须能直接加载并实例化它。
     *  @param bridgeClass 插在真实主类与 JavaPlugin 之间的“桥”(见模板注释)。
     *                     打包器会把真实主类的父类改到它上面。 */
    bool stepFinalPack(const Config &config, const QString &payloadSourceJar,
                       const QString &injectDir, const QString &keyHex,
                       const QString &mainClass, const QString &bridgeClass,
                       const QString &signKeyPath, QString &errorMessage);

    /** 把 cppDir 下编好的 .so/.dll 放进 nativeN/ —— NOBF 的 Loader 是从 JAR 里
     *  取这个资源的,而它在编译之前就生成了,必然缺库。 */
    bool placeNativeLibs(const QString &cppDir, const QString &destNativeDir,
                         QString &errorMessage);

    // 工具函数
    bool runTool(const QString &program, const QStringList &args,
                 const QString &workDir, QString &errorMessage);
    bool writeFile(const QString &path, const QString &content, QString &errorMessage);
    QString buildObfuscatorConfig(const Config &config,
                                  const QStringList &classBlackList,
                                  const QStringList &classBlackRegexList,
                                  const QString &decryptClassName,
                                  bool forUserJar) const;

    /** 把额外类路径递给混淆器:它靠这些 JAR 才能算出类型的公共父类。
     *
     *  <p>没有它们,ASM 的 getCommonSuperClass 只能退回 java/lang/Object ——
     *  对栈上的操作数这会让生成的栈帧与指令不匹配,产物一加载就是
     *  {@code VerifyError: Bad type on operand stack}。</p>
     *
     *  <p>返回空串表示没有可用条目(调用方应直接跳过这个参数)。</p> */
    static QString extraClasspathArg(const Config &config);

    /** 把用户勾选的原名翻译成混淆后的类内部名。
     *  映射表里没有的(被黑名单跳过的类)按原名处理 —— 混淆器没改它。 */
    static QStringList resolveHideClassNames(const QStringList &requested,
                                             const QHash<QString, QString> &mapping);
    /** 解析 jar-obfuscator 的 mapping.txt:`原始名 -> 混淆后名`(均为点分隔) */
    static QHash<QString, QString> parseMappingFile(const QString &path);
    /** 把 extraClassPath 里的条目展开成实际存在的 .jar(目录递归展开) */
    static QStringList expandJars(const QStringList &entries);
    /** 把若干 jar 汇总到一个目录,给 NOBF 的 -l 用(它只接受目录)。
     *  返回实际放进去的 jar 数。-l 是可选优化,失败不应当让整个任务挂掉。 */
    int stageNobfLibs(const QStringList &jars, const QString &destDir);
    /** 给选中的类生成 NOBF 白名单。
     *  ⚠ NOBF 的规则是逐条正则:类名要精确匹配 `a/b/C`,方法名要匹配
     *  `a/b/C#方法名!描述符`。只写类名会让所有方法都匹配不上,
     *  而 NOBF 要求"至少有一个方法能过",结果是整个类被跳过。
     *  所以每个类必须写两条。 */
    static QString buildNativeWhiteList(const QStringList &internalNames);

    void abort(const QString &reason);

    volatile bool m_stop = false;
    volatile bool m_running = false;
    /** 用户 JAR 那次混淆所占用的「字符串解密器类名」。
     *  验证模块那次混淆必须避开它,否则两个 JAR 会各自生成一个同名但密钥不同的
     *  解密器类;而 AhxClassLoader 是父优先,用户类的字符串解密调用会被父加载器
     *  (模块)的同名类截胡,解出 null,见 stepObfuscateModule 的注释。 */
    QString m_userDecryptClass;
};

#endif // PACKER_PIPELINE_H
