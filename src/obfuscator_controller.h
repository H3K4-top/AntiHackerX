#ifndef OBFUSCATORCONTROLLER_H
#define OBFUSCATORCONTROLLER_H

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

/**
 * ObfuscatorController - 负责调用外部的 native-obfuscator-mod
 * 
 * 通过进程隔离方式调用 GPL 3.0 组件,避免许可证传染
 */
class ObfuscatorController : public QObject {
    Q_OBJECT

public:
    enum class Platform {
        HotSpot,    // 标准 HotSpot JVM
        StdJava,    // 标准 Java
        Android     // Android 平台
    };

    struct Config {
        QString inputJarPath;           // 输入 JAR 文件路径
        QString outputDirPath;          // 输出目录路径
        QString blacklistPath;          // 黑名单文件路径 (可选)
        QString whitelistPath;          // 白名单文件路径 (可选)
        QString librariesDir;           // 依赖库目录 (可选)
        Platform platform;              // 目标平台
        bool useAnnotations;            // 是否使用注解
        QString customLibDir;           // 自定义库目录 (可选)
        bool debugMode;                 // 调试模式
    };

    explicit ObfuscatorController(QObject *parent = nullptr);
    ~ObfuscatorController();

    // 设置 native-obfuscator-mod.jar 路径
    void setObfuscatorJarPath(const QString &path);
    QString getObfuscatorJarPath() const;

    // 设置要使用的 java 主程序(启动引导阶段选定的便携版或系统版)
    void setJavaExecutable(const QString &path);
    QString getJavaExecutable() const;

    // 设置传给子进程的 JAVA_HOME
    void setJavaHome(const QString &path);
    QString getJavaHome() const;

    // 开始混淆处理
    bool start(const Config &config);
    
    // 停止处理
    void stop();
    
    // 检查是否正在运行
    bool isRunning() const;

signals:
    // 进度更新 (0-100)
    void progressUpdated(int progress);
    
    // 日志消息
    void logMessage(const QString &message);
    
    // 处理完成
    void finished(bool success, const QString &message);
    
    // 错误信号
    void errorOccurred(const QString &error);

private slots:
    void onProcessReadyReadStdOut();
    void onProcessReadyReadStdErr();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);

private:
    QStringList buildArguments(const Config &config);
    QString platformToString(Platform platform) const;
    void parseOutputLine(const QString &line);

    QProcess *process;
    QString obfuscatorJarPath;
    QString javaExecutable;
    QString javaHome;
    bool running;
};

#endif // OBFUSCATORCONTROLLER_H
