#include "downloader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QtMath>

namespace DownloadUtil {

QString humanBytes(qint64 bytes) {
    if (bytes < 0) {
        return QStringLiteral("未知");
    }
    const double kb = bytes / 1024.0;
    if (kb < 1024.0) {
        return QString::number(kb, 'f', 1) + " KB";
    }
    const double mb = kb / 1024.0;
    if (mb < 1024.0) {
        return QString::number(mb, 'f', 2) + " MB";
    }
    return QString::number(mb / 1024.0, 'f', 2) + " GB";
}

QString humanSpeed(double kbps) {
    if (kbps <= 0.0 || !qIsFinite(kbps)) {
        return QStringLiteral("--");
    }
    if (kbps < 1024.0) {
        return QString::number(kbps, 'f', 1) + " KB/s";
    }
    return QString::number(kbps / 1024.0, 'f', 2) + " MB/s";
}

QString humanEta(int seconds) {
    if (seconds < 0) {
        return QStringLiteral("--");
    }
    if (seconds < 60) {
        return QString("%1 秒").arg(seconds);
    }
    const int minutes = seconds / 60;
    const int rest = seconds % 60;
    if (minutes < 60) {
        return QString("%1 分 %2 秒").arg(minutes).arg(rest);
    }
    return QString("%1 时 %2 分").arg(minutes / 60).arg(minutes % 60);
}

} // namespace DownloadUtil

namespace {

/// 重试前等待多久(毫秒):2s / 4s / 8s …
int backoffMsFor(int attempt) {
    return qMin(1000 * (1 << qBound(0, attempt, 4)), 15000);
}

/// 这个 HTTP 状态值得重试吗?
/// 5xx / 408 / 429 以及"连状态码都没拿到"(0)算瞬时故障;
/// 其它 4xx(404 资源不存在、403 被拒)重试也没意义。
bool isRetryableStatus(int httpStatus) {
    if (httpStatus == 0) {
        return true;
    }
    if (httpStatus == 408 || httpStatus == 429) {
        return true;
    }
    return httpStatus >= 500;
}

} // namespace

Downloader::Downloader(QObject *parent)
    : QObject(parent),
      manager(new QNetworkAccessManager(this)),
      stallTimer(new QTimer(this)),
      retryTimer(new QTimer(this)) {
    // 优先使用系统代理配置(Windows/Linux/macOS 的系统代理会被自动读取)
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    
    stallTimer->setSingleShot(true);
    connect(stallTimer, &QTimer::timeout, this, &Downloader::onStallTimeout);

    retryTimer->setSingleShot(true);
    connect(retryTimer, &QTimer::timeout, this, &Downloader::beginAttempt);
}

Downloader::~Downloader() {
    if (reply) {
        // 析构期间不要再触发信号
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
        reply = nullptr;
    }
    delete outFile;
    outFile = nullptr;
}

bool Downloader::isRunning() const {
    return active;
}

void Downloader::setMaxRetries(int retries) {
    maxRetries = qMax(0, retries);
}

void Downloader::setStallTimeoutMs(int ms) {
    stallTimeoutMs = qMax(5000, ms);
}

void Downloader::start(const QUrl &url, const QString &destFilePath) {
    if (isRunning()) {
        emit failed(QStringLiteral("已有下载任务正在进行"));
        return;
    }

    currentUrl = url;
    destPath = destFilePath;
    resumeOffset = 0;
    receivedBytes = 0;
    totalBytes = -1;
    attempt = 0;
    offsetChecked = false;
    aborted = false;
    settled = false;
    active = true;

    // 目标目录
    const QFileInfo destInfo(destPath);
    QDir parentDir(destInfo.absolutePath());
    if (!parentDir.exists() && !parentDir.mkpath(".")) {
        fail(QString("无法创建目录: %1").arg(parentDir.absolutePath()));
        return;
    }

    // 每次全新下载都从零开始,避免残留尾部数据
    if (destInfo.exists() && !QFile::remove(destPath)) {
        fail(QString("无法删除已存在的文件: %1").arg(destPath));
        return;
    }

    sessionElapsed.start();
    beginAttempt();
}

void Downloader::beginAttempt() {
    if (aborted) {
        fail(QStringLiteral("下载已取消"));
        return;
    }

    attemptElapsed.start();
    offsetChecked = false;

    const QIODevice::OpenMode mode =
        resumeOffset > 0 ? (QIODevice::WriteOnly | QIODevice::Append | QIODevice::Unbuffered)
                         : (QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Unbuffered);

    delete outFile;
    outFile = new QFile(destPath, this);
    if (!outFile->open(mode)) {
        const QString reason = outFile->errorString();
        delete outFile;
        outFile = nullptr;
        fail(QString("无法写入文件: %1\n%2").arg(destPath, reason));
        return;
    }

    QNetworkRequest request(currentUrl);
    // GitHub / Adoptium 都会重定向到别的域名,必须显式开启跟随
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QVariant::fromValue(QNetworkRequest::NoLessSafeRedirectPolicy));
    request.setMaximumRedirectsAllowed(20);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QString("%1/%2 (Qt)")
                          .arg(QCoreApplication::applicationName(),
                               QCoreApplication::applicationVersion()));
    request.setRawHeader("Accept", "*/*");

    if (resumeOffset > 0) {
        request.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(resumeOffset) + "-");
    }

    reply = manager->get(request);
    connect(reply, &QNetworkReply::readyRead, this, &Downloader::onReadyRead);
    connect(reply, &QNetworkReply::downloadProgress, this, &Downloader::onDownloadProgress);
    connect(reply, &QNetworkReply::finished, this, &Downloader::onReplyFinished);

    restartStallTimer();
}

void Downloader::restartStallTimer() {
    if (stallTimeoutMs > 0) {
        stallTimer->start(stallTimeoutMs);
    }
}

void Downloader::abort() {
    if (!active || settled) {
        return;
    }
    aborted = true;
    stopTimers();
    if (reply) {
        reply->abort();
    } else {
        // 正在等待重试的空档里被取消
        fail(QStringLiteral("下载已取消"));
    }
}

void Downloader::onReadyRead() {
    if (!reply || !outFile || !active) {
        return;
    }
    if (!reply->isOpen()) {
        return;
    }

    // 第一次真正收到数据时,确认服务器接受不接受续传
    if (!offsetChecked) {
        offsetChecked = true;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (resumeOffset > 0 && status != 206) {
            // 请求了 Range 却拿到 200,说明服务器把整个文件从头发了。
            // 必须清空文件重来,否则会拼出两段内容。
            outFile->close();
            if (!outFile->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Unbuffered)) {
                fail(QString("无法重置已下载的文件: %1").arg(destPath));
                return;
            }
            resumeOffset = 0;
            totalBytes = -1;
        }
    }

    const QByteArray chunk = reply->readAll();
    if (chunk.isEmpty()) {
        return;
    }
    restartStallTimer();

    if (outFile->write(chunk) != chunk.size()) {
        fail(QString("写入磁盘失败: %1").arg(outFile->errorString()));
    }
}

void Downloader::onDownloadProgress(qint64 bytesReceivedInAttempt, qint64 bytesTotalInAttempt) {
    if (!active) {
        return;
    }
    receivedBytes = resumeOffset + bytesReceivedInAttempt;

    if (bytesTotalInAttempt > 0) {
        totalBytes = resumeOffset + bytesTotalInAttempt;
    }

    // 速度按"本次尝试"算,避免上一轮的停滞把平均值拖垮
    const qint64 ms = attemptElapsed.elapsed();
    double kbps = 0.0;
    if (ms > 200 && bytesReceivedInAttempt > 0) {
        kbps = (bytesReceivedInAttempt / 1024.0) / (ms / 1000.0);
    }

    int eta = -1;
    if (kbps > 0.0 && totalBytes > receivedBytes) {
        eta = static_cast<int>((totalBytes - receivedBytes) / 1024.0 / kbps);
    }

    emit progress(receivedBytes, totalBytes, kbps, eta);
}

void Downloader::onStallTimeout() {
    if (!active || settled) {
        return;
    }
    handleRetryableFailure(
        QString("连接停滞:%1 秒没有收到新数据").arg(stallTimeoutMs / 1000));
}

void Downloader::onReplyFinished() {
    if (!active || settled || !reply) {
        return;
    }

    // abort() 之后 reply 已关闭,再 readAll() 会刷 "device not open" 警告
    const bool wasAborted = aborted;
    const QNetworkReply::NetworkError netError = reply->error();
    const QString netErrorText = reply->errorString();
    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // 收尾:把缓冲区剩下的读干净
    if (!wasAborted && reply->isOpen()) {
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty() && outFile && outFile->isOpen()) {
            outFile->write(tail);
        }
    }

    if (outFile) {
        outFile->close();
    }

    if (wasAborted) {
        teardownReply();
        QFile::remove(destPath);
        fail(QStringLiteral("下载已取消"));
        return;
    }

    if (netError != QNetworkReply::NoError) {
        // 用户主动取消在上面已经提前返回了,所以走到这里的错误都值得重试
        // (超时、代理截断、DNS 抖动……),重试次数上限由 handleRetryableFailure 把关
        const QString reason = httpStatus > 0
                                   ? QString("网络中断: %1(HTTP %2)").arg(netErrorText).arg(httpStatus)
                                   : QString("网络中断: %1").arg(netErrorText);
        teardownReply();
        handleRetryableFailure(reason);
        return;
    }

    if (httpStatus >= 400) {
        const QString reason = QString("服务器返回 HTTP %1").arg(httpStatus);
        if (isRetryableStatus(httpStatus)) {
            teardownReply();
            handleRetryableFailure(reason);
            return;
        }
        teardownReply();
        fail(reason);
        return;
    }

    const qint64 written = QFileInfo(destPath).size();
    if (written <= 0) {
        teardownReply();
        handleRetryableFailure(QStringLiteral("下载内容为空,可能被代理截断"));
        return;
    }

    if (totalBytes > 0 && written < totalBytes) {
        // 连接看着正常但提前结束,和停滞一样处理
        teardownReply();
        handleRetryableFailure(
            QString("下载不完整:只有 %1 / %2")
                .arg(DownloadUtil::humanBytes(written), DownloadUtil::humanBytes(totalBytes)));
        return;
    }

    stopTimers();
    active = false;
    settled = true;
    teardownReply();
    emit finished(destPath);
}

void Downloader::handleRetryableFailure(const QString &reason) {
    if (aborted) {
        fail(QStringLiteral("下载已取消"));
        return;
    }
    stopTimers();

    if (attempt >= maxRetries) {
        fail(QStringLiteral("%1(已重试 %2 次)").arg(reason).arg(maxRetries));
        return;
    }

    // 把已落盘的部分留下来,下一轮用 Range 接着下
    const qint64 onDisk = QFileInfo(destPath).size();
    resumeOffset = onDisk > 0 ? onDisk : 0;

    ++attempt;
    const int waitMs = backoffMsFor(attempt);
    emit retrying(attempt, maxRetries, reason);

    retryTimer->start(waitMs);
}

void Downloader::teardownReply() {
    if (outFile) {
        if (outFile->isOpen()) {
            outFile->close();
        }
        outFile->deleteLater();
        outFile = nullptr;
    }
    if (reply) {
        reply->disconnect(this);
        reply->deleteLater();
        reply = nullptr;
    }
}

void Downloader::stopTimers() {
    stallTimer->stop();
    retryTimer->stop();
}

void Downloader::fail(const QString &message) {
    if (settled) {
        return;
    }
    settled = true;
    active = false;
    stopTimers();
    teardownReply();
    emit failed(message);
}
