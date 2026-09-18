#ifndef DOWNLOAD_DIALOG_H
#define DOWNLOAD_DIALOG_H

#include <QDialog>

class QLabel;
class QProgressBar;
class QPushButton;

/**
 * 启动引导阶段使用的模态下载窗口。
 *
 * 显示:阶段说明(如"正在下载 Java 25 便携版")+ 进度条 + 速度/剩余时间明细。
 * 下载过程中禁用关闭按钮,避免用户误关导致半截文件。
 */
class DownloadDialog : public QDialog {
    Q_OBJECT

public:
    explicit DownloadDialog(const QString &title,
                            const QString &intro,
                            QWidget *parent = nullptr);

    /// 设置当前阶段标题(例如 "正在下载依赖:")
    void setStage(const QString &stage);
    /// 更新进度;totalBytes <= 0 时切到"忙碌"动画模式
    void setProgress(qint64 receivedBytes, qint64 totalBytes);
    /// 更新明细行(速度 / 剩余时间 / 文件名)
    void setDetail(const QString &detail);
    /// 切换到不定进度模式(解压等无法估算的步骤)
    void setBusy(const QString &detail);
    /// 结束态:显示成功或失败说明,取消按钮变为"关闭"
    void settle(bool success, const QString &message);

signals:
    void cancelRequested();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void configureBar(bool busy);

    QLabel *lblIntro = nullptr;
    QLabel *lblStage = nullptr;
    QLabel *lblDetail = nullptr;
    QProgressBar *bar = nullptr;
    QPushButton *btnCancel = nullptr;
    bool m_settled = false;
};

#endif // DOWNLOAD_DIALOG_H
