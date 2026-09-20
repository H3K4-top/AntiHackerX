#ifndef RUNTIME_BOOTSTRAP_H
#define RUNTIME_BOOTSTRAP_H

#include <QObject>
#include <QString>
#include <QStringList>

class QWidget;

/**
 * 启动期运行环境引导。
 *
 * 程序启动时(主窗口显示之前)依次保证:
 *   1. 有可用的 Java 运行时 —— 优先用程序自带便携版(<EXE 同级>/java),
 *      其次 $JAVA_HOME,再次 PATH 上的 java。
 *      一个都没有时弹窗询问是否下载 JDK 25 便携版(带进度条),
 *      下载并解压完成后把 JAVA_HOME 指向它,继续第 2 步;
 *      用户拒绝则提示"必须安装 Java"并让程序退出。
 *   2. 有可用的 native-obfuscator 依赖 —— 从 GitHub Release 取最新版,
 *      解开后把 jar 放到 <EXE 同级>/libs/。
 *      本地已有时会比对版本(远端 tag ↔ 本地 jar 的 `--version`),只有不一致
 *      才重下;断网 / API 限流时保留现有 jar,不回退到旧版本。
 *
 * 目录布局(都以可执行文件所在目录为基准,便于做成绿色版):
 *   <root>/AntiHackerX(.exe)
 *   <root>/java/...            便携版 JDK
 *   <root>/libs/native-obfuscator.jar
 */
class RuntimeBootstrap : public QObject {
    Q_OBJECT

public:
    explicit RuntimeBootstrap(QWidget *parent = nullptr);

    /// 阻塞式执行完整引导。返回 false 表示无法继续,调用方应直接退出。
    bool ensureReady();

    /// 强制重新下载 native-obfuscator 依赖(供主窗口"重新下载依赖"菜单调用)
    bool refreshNativeObfuscator();

    /// 强制重新下载 jar-obfuscator 依赖(同上,取最新发行版里的那份)
    bool refreshJarObfuscator();

    /// 强制重新安装便携版 Java(供主窗口"重新安装 Java"菜单调用)
    bool installPortableJava();

    /// 强制重新安装便携版 C++ 工具链(zig;供主窗口"重新安装工具链"菜单调用)
    bool installPortableCppToolchain();

    bool refreshJavaDetection();

    QString javaExecutable() const { return m_javaExe; }
    QString javaHome() const { return m_javaHome; }
    QString obfuscatorJarPath() const { return m_jarPath; }
    /// 便携版 C++ 编译器(CC 包装脚本);未就绪时为空串
    QString cppCompilerPath() const { return m_cppCompiler; }
    /// 便携版 C++ 编译器(CXX 包装脚本);未就绪时为空串
    QString cppCompilerXXPath() const { return m_cppCompilerXX; }
    QString installRoot() const { return m_root; }
    QString errorMessage() const { return m_error; }
    /// 引导过程中产生的日志,主窗口可直接回放到日志面板
    QStringList collectedLog() const { return m_log; }

    /// 依赖 jar 相对于安装根目录的固定位置
    static QString jarPathForRoot(const QString &root);
    /// jar-obfuscator 相对于安装根目录的固定位置
    /// (文件名必须与打包器查找的一致,见 .cpp 里的说明)
    static QString jarObfuscatorPathForRoot(const QString &root);
    /// 便携版 Java 主程序路径
    static QString bundledJavaExecutable(const QString &root);
    /// 便携版 C++ 工具链(zig)的根目录
    static QString zigRootForRoot(const QString &root);
    /// 便携版 C++ 工具链的 zig 主程序
    static QString zigExecutable(const QString &root);
    /// 供 CMake/Makefile 当作编译器使用的包装脚本(cxx=false → CC,true → CXX)
    static QString zigCompilerWrapper(const QString &root, bool cxx);
    /// Windows 交叉编译所需的 jni_md.h 在工具链内的相对路径
    static QString zigWin32JniMdPath(const QString &root);
    /// 供其它模块复用的 Java 可执行文件名
    static QString javaExecutableName();

    /// 探测可用的 Java 主程序(不弹窗、不下载)。找不到返回空串。
    static QString detectJavaExecutable(QString *versionOut = nullptr, QString *homeOut = nullptr);
    /// 判断给定 java 主程序能否正常执行
    static bool probeJava(const QString &javaExecutable, QString *versionOut, QString *errorOut);
    /// 读取本地 native-obfuscator jar 自报的版本号(跑 `java -jar <jar> --version`)。
    /// 拿不到返回空串 —— 调用方应把它当作"无法判断",而不是"过期"。
    static QString probeNativeObfuscatorVersion(const QString &javaExecutable,
                                                const QString &jarPath);

signals:
    void logMessage(const QString &message);

private:
    bool ensureJava();
    bool ensureNativeObfuscator();
    /// 确保 jar-obfuscator 就绪(缺失时从最新发行版下载)。
    /// 失败不算致命,所以恒返回 true —— 见 .cpp 里的说明。
    bool ensureJarObfuscator();
    bool ensureCppToolchain();
    bool downloadNativeObfuscatorArchive(const QString &zipUrl, QString *errorOut);

    /// 弹出带进度条的窗口完成一次下载(内部跑事件循环,调用期间界面不卡死)
    bool runDownloadWithDialog(const QString &title,
                               const QString &intro,
                               const QString &stage,
                               const QUrl &url,
                               const QString &destFile,
                               QString *errorOut);

    /// 确保 m_root 已确定
    void ensureRoot();
    QString preferredInstallRoot() const;
    QString stagingRoot() const;
    void appendLog(const QString &message);
    void warn(const QString &title, const QString &text);

    QWidget *m_parent = nullptr;
    QString m_root;
    QString m_javaExe;
    QString m_javaHome;
    QString m_jarPath;
    QString m_cppCompiler;
    QString m_cppCompilerXX;
    QString m_error;
    QStringList m_log;
};

#endif // RUNTIME_BOOTSTRAP_H
