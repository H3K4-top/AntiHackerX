#include "download_dialog.h"

#include "downloader.h"

#include <QCloseEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

DownloadDialog::DownloadDialog(const QString &title,
                               const QString &intro,
                               QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(title);
    // 引导阶段没有主窗口,做成应用级模态的独立窗口
    setWindowModality(Qt::ApplicationModal);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setMinimumWidth(460);

    auto *root = new QVBoxLayout(this);
    root->setSpacing(10);
    root->setContentsMargins(18, 16, 18, 14);

    lblIntro = new QLabel(intro, this);
    lblIntro->setWordWrap(true);
    root->addWidget(lblIntro);

    lblStage = new QLabel(this);
    QFont stageFont = lblStage->font();
    stageFont.setBold(true);
    lblStage->setFont(stageFont);
    lblStage->setWordWrap(true);
    root->addWidget(lblStage);

    bar = new QProgressBar(this);
    bar->setTextVisible(true);
    bar->setMinimumHeight(22);
    configureBar(false);
    root->addWidget(bar);

    lblDetail = new QLabel(this);
    lblDetail->setWordWrap(true);
    lblDetail->setStyleSheet("color: #555555;");
    root->addWidget(lblDetail);

    root->addStretch(1);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->addStretch(1);
    btnCancel = new QPushButton(QStringLiteral("取消"), this);
    btnCancel->setAutoDefault(false);
    connect(btnCancel, &QPushButton::clicked, this, [this]() {
        if (m_settled) {
            accept();
        } else {
            btnCancel->setEnabled(false);
            emit cancelRequested();
        }
    });
    buttonRow->addWidget(btnCancel);
    root->addLayout(buttonRow);
}

void DownloadDialog::configureBar(bool busy) {
    if (busy) {
        bar->setRange(0, 0); // Qt 的忙碌动画
    } else {
        // QProgressBar 用 int,按千分比缩放以支持任意大文件
        bar->setRange(0, 1000);
        bar->setValue(0);
    }
}

void DownloadDialog::setStage(const QString &stage) {
    lblStage->setText(stage);
}

void DownloadDialog::setProgress(qint64 receivedBytes, qint64 totalBytes) {
    if (m_settled) {
        return;
    }
    if (totalBytes <= 0) {
        configureBar(true);
        return;
    }
    if (bar->maximum() != 1000) {
        configureBar(false);
    }
    const double ratio = static_cast<double>(receivedBytes) / static_cast<double>(totalBytes);
    const int value = qBound(0, static_cast<int>(ratio * 1000.0), 1000);
    bar->setValue(value);
    bar->setFormat(QString("%1%  (%2 / %3)")
                       .arg(QString::number(ratio * 100.0, 'f', 1),
                            DownloadUtil::humanBytes(receivedBytes),
                            DownloadUtil::humanBytes(totalBytes)));
}

void DownloadDialog::setDetail(const QString &detail) {
    if (m_settled) {
        return;
    }
    lblDetail->setText(detail);
}

void DownloadDialog::setBusy(const QString &detail) {
    if (m_settled) {
        return;
    }
    configureBar(true);
    lblDetail->setText(detail);
}

void DownloadDialog::settle(bool success, const QString &message) {
    if (m_settled) {
        return;
    }
    m_settled = true;
    const int previous = (bar->maximum() == 1000) ? bar->value() : 0;
    bar->setRange(0, 1000);
    bar->setValue(success ? 1000 : previous);
    bar->setFormat(success ? QStringLiteral("完成") : QStringLiteral("已停止"));
    lblDetail->setText(message);
    btnCancel->setText(QStringLiteral("关闭"));
    btnCancel->setEnabled(true);
}

void DownloadDialog::closeEvent(QCloseEvent *event) {
    if (!m_settled) {
        // 下载中不允许直接关窗,先走取消流程
        event->ignore();
        btnCancel->setEnabled(false);
        emit cancelRequested();
        return;
    }
    QDialog::closeEvent(event);
}
