#ifndef APP_LOGGER_H
#define APP_LOGGER_H

#include <QFile>
#include <QTextStream>
#include <QString>
#include <QDateTime>
#include <QDir>
#include <QCoreApplication>

/// 全局日志管理器(单例)
class AppLogger {
public:
    static AppLogger& instance() {
        static AppLogger logger;
        return logger;
    }
    
    /// 写入一条日志(带时间戳)
    void log(const QString &message) {
        if (!logStream) {
            return;
        }
        
        QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
        QString logLine = QString("[%1] %2").arg(timestamp, message);
        
        *logStream << logLine << "\n";
        logStream->flush();
    }
    
    /// 获取日志文件路径
    QString logFilePath() const {
        if (logFile) {
            return logFile->fileName();
        }
        return QString();
    }
    
private:
    AppLogger() {
        // 日志文件路径:程序目录/logs/antihackerx_YYYYMMDD.log
        QString appDir = QCoreApplication::applicationDirPath();
        QString logDir = QDir(appDir).filePath("logs");
        QDir().mkpath(logDir);
        
        QString date = QDateTime::currentDateTime().toString("yyyyMMdd");
        QString logPath = QDir(logDir).filePath(QString("antihackerx_%1.log").arg(date));
        
        logFile = new QFile(logPath);
        if (logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            logStream = new QTextStream(logFile);
            logStream->setEncoding(QStringConverter::Utf8);
            
            // 写入会话开始标记
            *logStream << "\n==================== 会话开始 " 
                       << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")
                       << " ====================\n";
            logStream->flush();
        } else {
            delete logFile;
            logFile = nullptr;
        }
    }
    
    ~AppLogger() {
        if (logStream) {
            *logStream << "==================== 会话结束 " 
                       << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")
                       << " ====================\n\n";
            logStream->flush();
            delete logStream;
        }
        if (logFile) {
            logFile->close();
            delete logFile;
        }
    }
    
    AppLogger(const AppLogger&) = delete;
    AppLogger& operator=(const AppLogger&) = delete;
    
    QFile *logFile = nullptr;
    QTextStream *logStream = nullptr;
};

#endif // APP_LOGGER_H
