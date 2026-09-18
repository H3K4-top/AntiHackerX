#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QUrl>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

/**
 * 下载相关的格式化小工具(界面与日志共用)
 */
namespace DownloadUtil {
/// 把字节数格式化成 "1.23 MB" 这样的可读文本
QString humanBytes(qint64 bytes);
/// 把 KB/s 格式化成 "1.23 MB/s" 这样的可读文本
QString humanSpeed(double kbps);
/// 把秒数格式化成 "1分23秒" 这样的可读文本
QString humanEta(int seconds);
}

/**
 * 单文件 HTTP(S) 下载器。
 *
 * 特性:
 *  - 自动跟随重定向(GitHub Release 会 302 到 release-assets.githubusercontent.com;
 *    Adoptium 更是 API → GitHub → release-assets 两次跳转)
 *  - 边下边写盘,不会把整个文件读进内存(JDK 压缩包接近 200 MB)
 *  - 上报进度、瞬时速度与预计剩余时间
 *  - **停滞看门狗**:代理“保持连接但不再出数据”时,`QNetworkReply` 的
 *    transferTimeout 不一定触发(它只要收到零星字节就重置),所以这里自己计时,
 *    超过 stallTimeoutMs 没有新数据就判定失败
 *  - **断点续传重试**:失败后带 Range 头重试,把剩余部分追加到已有文件,
 *    不用从零再下一遍 190 MB。服务器不支持 Range(返回 200)时自动截断重来
 *  - 支持取消
 *
 * 所有信号都在主线程发出,可直接连接界面控件。
 */
class Downloader : public QObject {
    Q_OBJECT

public:
    explicit Downloader(QObject *parent = nullptr);
    ~Downloader() override;

    /// 开始下载。destFilePath 若已存在会被覆盖。
    void start(const QUrl &url, const QString &destFilePath);
    /// 取消下载(会触发 failed("下载已取消"))
    void abort();

    bool isRunning() const;

    /// 网络出错时的最大重试次数(默认 3)
    void setMaxRetries(int retries);
    /// 多久没收到数据就判定为停滞(毫秒,默认 60000)
    void setStallTimeoutMs(int ms);

signals:
    /// bytesTotal 为 -1 表示服务器未提供总长度
    void progress(qint64 bytesReceived, qint64 bytesTotal, double kbps, int etaSeconds);
    /// 正在重试(界面可据此显示“连接中断,正在重试 N/M”)
    void retrying(int attempt, int maxAttempts, const QString &reason);
    void finished(const QString &filePath);
    void failed(const QString &errorMessage);

private slots:
    void onReadyRead();
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onReplyFinished();
    void onStallTimeout();

private:
    void beginAttempt();
    /// 可重试的失败:还能重试就安排重试,否则 fail()
    void handleRetryableFailure(const QString &reason);
    void teardownReply();
    void stopTimers();
    void fail(const QString &message);
    void restartStallTimer();

    QNetworkAccessManager *manager = nullptr;
    QNetworkReply *reply = nullptr;
    QFile *outFile = nullptr;
    QTimer *stallTimer = nullptr;
    QTimer *retryTimer = nullptr;

    QUrl currentUrl;
    QString destPath;
    QElapsedTimer sessionElapsed;   // 整个下载(含重试)的耗时
    QElapsedTimer attemptElapsed;   // 本次尝试的耗时,用于算瞬时速度

    qint64 resumeOffset = 0;        // 已经落盘、本次请求要跳过的字节数
    qint64 receivedBytes = 0;       // 含 resumeOffset 的总接收量
    qint64 totalBytes = -1;
    int attempt = 0;
    int maxRetries = 3;
    int stallTimeoutMs = 60000;
    bool offsetChecked = false;
    bool active = false;
    bool aborted = false;
    bool settled = false;
};

#endif // DOWNLOADER_H
