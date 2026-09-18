#include "obfuscator_controller.h"
#include <QFileInfo>
#include <QDir>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QDebug>

ObfuscatorController::ObfuscatorController(QObject *parent)
    : QObject(parent),
      process(nullptr),
      obfuscatorJarPath(""),
      javaExecutable("java"),
      javaHome(""),
      running(false)
{
}

ObfuscatorController::~ObfuscatorController() {
    if (process) {
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished();
        }
        delete process;
    }
}

void ObfuscatorController::setObfuscatorJarPath(const QString &path) {
    obfuscatorJarPath = path;
}

QString ObfuscatorController::getObfuscatorJarPath() const {
    return obfuscatorJarPath;
}

void ObfuscatorController::setJavaExecutable(const QString &path) {
    javaExecutable = path.isEmpty() ? QStringLiteral("java") : path;
}

QString ObfuscatorController::getJavaExecutable() const {
    return javaExecutable;
}

void ObfuscatorController::setJavaHome(const QString &path) {
    javaHome = path;
}

QString ObfuscatorController::getJavaHome() const {
    return javaHome;
}

bool ObfuscatorController::start(const Config &config) {
    if (running) {
        emit errorOccurred("已有任务正在运行");
        return false;
    }

    // 检查 jar 文件是否存在
    if (!QFileInfo::exists(obfuscatorJarPath)) {
        emit errorOccurred(QString("找不到 native-obfuscator-mod.jar: %1").arg(obfuscatorJarPath));
        return false;
    }

    // 检查输入文件
    if (!QFileInfo::exists(config.inputJarPath)) {
        emit errorOccurred(QString("找不到输入文件: %1").arg(config.inputJarPath));
        return false;
    }

    // 检查输出目录
    QDir outputDir(config.outputDirPath);
    if (!outputDir.exists()) {
        if (!outputDir.mkpath(".")) {
            emit errorOccurred(QString("无法创建输出目录: %1").arg(config.outputDirPath));
            return false;
        }
    }

    // 创建进程
    if (process) {
        delete process;
    }
    process = new QProcess(this);

    // 连接信号
    connect(process, &QProcess::readyReadStandardOutput,
            this, &ObfuscatorController::onProcessReadyReadStdOut);
    connect(process, &QProcess::readyReadStandardError,
            this, &ObfuscatorController::onProcessReadyReadStdErr);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ObfuscatorController::onProcessFinished);
    connect(process, &QProcess::errorOccurred,
            this, &ObfuscatorController::onProcessError);

    // 构建参数
    QStringList args = buildArguments(config);

    // 启动进程
    emit logMessage("===== 启动 native-obfuscator-mod =====");
    emit logMessage(QString("命令: %1 %2").arg(javaExecutable, args.join(" ")));

    // 用引导阶段确定好的 java(便携版优先),而不是裸 "java"
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!javaHome.isEmpty()) {
        env.insert("JAVA_HOME", javaHome);
    }
    process->setProcessEnvironment(env);
    process->setProgram(javaExecutable);
    process->setArguments(args);
    process->start();

    if (!process->waitForStarted()) {
        emit errorOccurred(QString("无法启动 Java 进程: %1").arg(javaExecutable));
        return false;
    }

    running = true;
    emit logMessage("进程已启动");
    return true;
}

void ObfuscatorController::stop() {
    if (process && process->state() != QProcess::NotRunning) {
        emit logMessage("正在停止进程...");
        process->terminate();
        
        if (!process->waitForFinished(3000)) {
            process->kill();
            process->waitForFinished();
        }
        
        running = false;
        emit logMessage("进程已停止");
    }
}

bool ObfuscatorController::isRunning() const {
    return running;
}

QStringList ObfuscatorController::buildArguments(const Config &config) {
    QStringList args;
    
    // jar 文件
    args << "-jar" << obfuscatorJarPath;
    
    // 输入和输出
    args << config.inputJarPath;
    args << config.outputDirPath;
    
    // 黑名单
    if (!config.blacklistPath.isEmpty() && QFileInfo::exists(config.blacklistPath)) {
        args << "-b" << config.blacklistPath;
    }
    
    // 白名单
    if (!config.whitelistPath.isEmpty() && QFileInfo::exists(config.whitelistPath)) {
        args << "-w" << config.whitelistPath;
    }
    
    // 依赖库目录
    if (!config.librariesDir.isEmpty() && QDir(config.librariesDir).exists()) {
        args << "-l" << config.librariesDir;
    }
    
    // 平台
    args << "-p" << platformToString(config.platform);
    
    // 注解
    if (config.useAnnotations) {
        args << "-a";
    }
    
    // 自定义库目录
    if (!config.customLibDir.isEmpty()) {
        args << "--custom-lib-dir" << config.customLibDir;
    }
    
    // 调试模式
    if (config.debugMode) {
        args << "--debug";
    }
    
    return args;
}

QString ObfuscatorController::platformToString(Platform platform) const {
    switch (platform) {
        case Platform::HotSpot:
            return "hotspot";
        case Platform::StdJava:
            return "std_java";
        case Platform::Android:
            return "android";
        default:
            return "hotspot";
    }
}

void ObfuscatorController::onProcessReadyReadStdOut() {
    if (!process) return;
    
    QString output = QString::fromUtf8(process->readAllStandardOutput());
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    
    for (const QString &line : lines) {
        parseOutputLine(line);
        emit logMessage(line);
    }
}

void ObfuscatorController::onProcessReadyReadStdErr() {
    if (!process) return;
    
    QString error = QString::fromUtf8(process->readAllStandardError());
    QStringList lines = error.split('\n', Qt::SkipEmptyParts);
    
    for (const QString &line : lines) {
        emit logMessage("[ERROR] " + line);
    }
}

void ObfuscatorController::parseOutputLine(const QString &line) {
    // 尝试解析进度信息
    // 例如: "Processing 45/100 classes..."
    QRegularExpression progressRegex(R"((\d+)/(\d+))");
    QRegularExpressionMatch match = progressRegex.match(line);
    
    if (match.hasMatch()) {
        int current = match.captured(1).toInt();
        int total = match.captured(2).toInt();
        if (total > 0) {
            int progress = (current * 100) / total;
            emit progressUpdated(progress);
        }
    }
    
    // 检查是否包含 "Processing" 或 "Transpiling"
    if (line.contains("Processing", Qt::CaseInsensitive) ||
        line.contains("Transpiling", Qt::CaseInsensitive)) {
        // 可以提取更多信息
    }
}

void ObfuscatorController::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    running = false;
    
    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        emit logMessage("===== 处理完成 =====");
        emit progressUpdated(100);
        emit finished(true, "处理成功完成");
    } else {
        QString errorMsg = QString("处理失败 (退出码: %1)").arg(exitCode);
        emit logMessage("===== 处理失败 =====");
        emit finished(false, errorMsg);
    }
}

void ObfuscatorController::onProcessError(QProcess::ProcessError error) {
    running = false;
    
    QString errorMsg;
    switch (error) {
        case QProcess::FailedToStart:
            errorMsg = "无法启动进程 (检查 Java 是否已安装)";
            break;
        case QProcess::Crashed:
            errorMsg = "进程崩溃";
            break;
        case QProcess::Timedout:
            errorMsg = "进程超时";
            break;
        case QProcess::WriteError:
            errorMsg = "写入错误";
            break;
        case QProcess::ReadError:
            errorMsg = "读取错误";
            break;
        default:
            errorMsg = "未知错误";
            break;
    }
    
    emit errorOccurred(errorMsg);
    emit finished(false, errorMsg);
}
