#ifndef CLASS_SCANNER_H
#define CLASS_SCANNER_H

#include <QString>
#include <QStringList>
#include <QList>

/**
 * 单个类的扫描结果
 *
 * 判断标准严格对齐 native-obfuscator (NOBF) 的实际处理逻辑:
 *
 *   NativeObfuscator.process() 中,一个 .class 条目会被转换,当且仅当:
 *     1. 文件头是 0xCAFEBABE(是合法的 class 文件);
 *     2. classMethodFilter.shouldProcess(classNode) 通过
 *        (未配置黑名单/白名单/注解时为恒真);
 *     3. 该类的 methods 中存在 **至少一个** 方法同时满足:
 *          - MethodProcessor.shouldProcess(method):
 *              非 abstract、非 native、名字不等于 "<init>"
 *          - classMethodFilter.shouldProcess(classNode, method)
 *        (未配置名单/注解时为恒真)
 *
 * 注意:<clinit> 不参与该判断(它被特殊处理),因此只有 <clinit> 的类
 * 依旧可能被判为"可转换";而只有 <init> 的类则不可转换。
 */
struct ScannedClass {
    QString className;          // 全限定类名(点号形式):com.example.Foo
    QString entryPath;          // JAR 内条目路径:com/example/Foo.class

    bool convertible = false;   // 是否可被 NOBF 转换为 C++
    bool thirdParty = false;    // 是否疑似第三方库(默认不勾选)
    QString reason;             // 状态说明(可转换/不可转换原因)

    int totalMethods = 0;       // 方法总数
    int convertibleMethods = 0; // 可转换方法数(非 abstract/native/<init>)

    bool isInterface = false;   // 接口
    bool isAbstract = false;    // 抽象类
    bool isEnum = false;        // 枚举
    bool isAnnotation = false;  // 注解类型
    bool isSynthetic = false;   // 编译器生成
    bool isModuleInfo = false;  // module-info

    quint16 majorVersion = 0;   // class 文件主版本号(52=Java8, 61=Java17 ...)
};

/**
 * 整个 JAR 的扫描结果
 */
struct ClassScanResult {
    bool isValid = false;            // 扫描是否成功
    QString errorMessage;            // 失败原因

    QList<ScannedClass> classes;     // 所有扫到的类(按类名排序)
    int totalClasses = 0;            // 类总数
    int convertibleCount = 0;        // 可转换类数
    int thirdPartyCount = 0;         // 疑似第三方库类数
    QString detectedRootPackage;     // 实际采用的插件根包(可由外部传入或自动推断)

    /**
     * 推荐勾选的类(可转换 且 非第三方库)
     */
    QList<ScannedClass> recommendedClasses() const;

    /**
     * 生成人类可读的摘要文本
     */
    QString summaryText() const;
};

/**
 * JAR 类扫描器
 *
 * 只读扫描:不会修改 JAR,也不会写出任何文件。
 */
class ClassScanner {
public:
    /**
     * 扫描 JAR 中所有 .class 条目,判断其能否被 NOBF 转换成 C++。
     *
     * @param jarPath           JAR 文件路径
     * @param pluginRootPackage 插件自身的根包名(如 ac.grim.grimac),
     *                          传入后落在该包之外的类会被标记为第三方库。
     *                          可由 plugin.yml 的 main 字段推导,也可留空。
     * @return 扫描结果(即使失败也会返回结构体,isValid 为 false)
     */
    static ClassScanResult scan(const QString &jarPath,
                               const QString &pluginRootPackage = QString());

    /**
     * 解析单个 class 字节码,填充 ScannedClass(除 entryPath/className 之外)
     *
     * @param classBytes class 文件原始字节
     * @param entryPath  JAR 内条目路径(用于推导类名)
     * @param out        输出结果
     * @param pluginRootPackage 插件自身根包名(可选,用于标记第三方库)
     * @return 是否解析成功
     */
    static bool analyzeClassBytes(const QByteArray &classBytes,
                                  const QString &entryPath,
                                  ScannedClass &out,
                                  const QString &pluginRootPackage = QString());

    /**
     * 由 JAR 内条目路径推导全限定类名
     * com/example/Foo.class -> com.example.Foo
     */
    static QString classNameFromEntryPath(const QString &entryPath);

    /**
     * 判断是否疑似第三方库类
     *
     * 判定优先级:
     *   1. 路径含 shaded/ shadow/ relocated/ repackaged/ 等重定位标记;
     *   2. 命中已知的第三方包名前缀(java.、org.bukkit.、com.google. ...);
     *   3. 提供了 pluginRootPackage 时,不在该根包下的类。
     *
     * @param entryPath         JAR 内条目路径
     * @param pluginRootPackage 插件自身根包名(可为空)
     */
    static bool isThirdPartyClass(const QString &entryPath,
                                  const QString &pluginRootPackage = QString());

    /**
     * 由主类全限定名推导插件根包
     * ac.grim.grimac.GrimAC -> ac.grim.grimac
     */
    static QString rootPackageOf(const QString &mainClass);
};

#endif // CLASS_SCANNER_H
