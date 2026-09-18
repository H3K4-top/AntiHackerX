#include "pack_progress_dialog.h"

#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QScrollBar>
#include <QCloseEvent>

PackProgressDialog::PackProgressDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("正在加壳"));
    setModal(true);
    resize(860, 580);

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setSpacing(8);

    lblStage = new QLabel(QStringLiteral("准备中..."), this);
    lblStage->setStyleSheet(QStringLiteral("QLabel { font-weight: bold; font-size: 11pt; }"));
    root->addWidget(lblStage);

    bar = new QProgressBar(this);
    bar->setRange(0, 0);   // 0..0 = 忙碌指示,拿到真实步数后再换成具体范围
    root->addWidget(bar);

    logView = new QTextEdit(this);
    logView->setReadOnly(true);
    logView->setLineWrapMode(QTextEdit::NoWrap);
    logView->setStyleSheet(
            QStringLiteral("QTextEdit { font-family: 'Courier New', monospace; font-size: 9pt; }"));
    root->addWidget(logView, 1);

    QHBoxLayout *buttons = new QHBoxLayout();
    buttons->addStretch();
    btnStop = new QPushButton(QStringLiteral("中止"), this);
    btnClose = new QPushButton(QStringLiteral("关闭"), this);
    btnClose->setEnabled(false);
    buttons->addWidget(btnStop);
    buttons->addWidget(btnClose);
    root->addLayout(buttons);

    connect(btnStop, &QPushButton::clicked, this, [this] {
        btnStop->setEnabled(false);
        lblStage->setText(QStringLiteral("正在中止..."));
        appendLog(QStringLiteral(">>> 用户请求中止"));
        emit stopRequested();
    });
    connect(btnClose, &QPushButton::clicked, this, [this] {
        m_running = false;
        accept();
    });
}

void PackProgressDialog::reset() {
    m_running = true;
    m_finished = false;
    setWindowTitle(QStringLiteral("正在加壳"));
    lblStage->setText(QStringLiteral("准备中..."));
    bar->setRange(0, 0);
    bar->setValue(0);
    logView->clear();
    btnStop->setVisible(true);
    btnStop->setEnabled(true);
    btnClose->setEnabled(false);
}

void PackProgressDialog::setStage(int index, int total, const QString &name) {
    if (total > 0) {
        bar->setRange(0, total);
        // 当前这一步还没跑完,所以填的是「已完成」的数量
        bar->setValue(index - 1);
    }
    lblStage->setText(QStringLiteral("第 %1/%2 步:%3").arg(index).arg(total).arg(name));
    appendLog(QString());
    appendLog(QStringLiteral("===== [%1/%2] %3 =====").arg(index).arg(total).arg(name));
}

void PackProgressDialog::appendLog(const QString &line) {
    logView->append(line);
    QScrollBar *sb = logView->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void PackProgressDialog::markFinished(bool ok, const QString &message) {
    m_running = false;
    m_finished = true;

    btnStop->setEnabled(false);
    btnStop->setVisible(false);
    btnClose->setEnabled(true);
    btnClose->setDefault(true);
    btnClose->setFocus();

    appendLog(QString());
    if (ok) {
        setWindowTitle(QStringLiteral("加壳完成"));
        lblStage->setText(QStringLiteral("加壳完成"));
        bar->setRange(0, 100);
        bar->setValue(100);
        appendLog(QStringLiteral(">>> 成功:%1").arg(message));
    } else {
        setWindowTitle(QStringLiteral("加壳失败"));
        lblStage->setText(QStringLiteral("加壳失败"));
        appendLog(QStringLiteral(">>> 失败:%1").arg(message));
    }
}

void PackProgressDialog::closeEvent(QCloseEvent *event) {
    if (m_running && !m_finished) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
                this,
                QStringLiteral("任务进行中"),
                QStringLiteral("加壳还在进行,关闭会中止任务。确定要中止吗?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        m_running = false;
        btnStop->setEnabled(false);
        emit stopRequested();
    }
    event->accept();
}
