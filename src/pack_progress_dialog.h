#ifndef PACK_PROGRESS_DIALOG_H
#define PACK_PROGRESS_DIALOG_H

#include <QDialog>
#include <QString>

class QLabel;
class QProgressBar;
class QPushButton;
class QTextEdit;
class QCloseEvent;

/**
 * 加壳过程的实时进度窗。
 *
 * <p>为什么不复用主面板上的进度条和日志框:</p>
 * <ul>
 *   <li>加壳要跑十几个外部工具,几分钟很正常。期间主界面是被禁用的,
 *       进度条上的变化用户看不见 —— 看起来就像卡死了。</li>
 *   <li>NOBF 对每个类都打一行日志,大插件动辄上万行;写在主面板会把
 *       有用的信息全冲走,用户没法回头读。</li>
 * </ul>
 *
 * <p>用法:先 {@link reset()},再 {@link exec()}。流水线在后台线程里跑,
 * 信号通过 {@link setStage()} / {@link appendLog()} / {@link markFinished()}
 * 送进来,exec() 的嵌套事件循环照常分发。</p>
 */
class PackProgressDialog : public QDialog {
    Q_OBJECT

public:
    explicit PackProgressDialog(QWidget *parent = nullptr);

    /** 开始新一轮,清空上一轮的内容 */
    void reset();

    /** 进入第 index 步(从 1 开始) */
    void setStage(int index, int total, const QString &name);

    /** 追加一行实时日志(自动滚到底) */
    void appendLog(const QString &line);

    /** 结束:停用中止按钮,放开关闭按钮,标题改成结果 */
    void markFinished(bool ok, const QString &message);

signals:
    /** 用户点了「中止」(或关闭时确认中止) */
    void stopRequested();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QLabel *lblStage = nullptr;
    QProgressBar *bar = nullptr;
    QTextEdit *logView = nullptr;
    QPushButton *btnStop = nullptr;
    QPushButton *btnClose = nullptr;

    bool m_running = false;
    /** 任务已结束时允许直接关窗,不再追问「要中止吗」 */
    bool m_finished = false;
};

#endif // PACK_PROGRESS_DIALOG_H
