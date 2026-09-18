#include "mainwindow.h"
#include "app_logger.h"
#include "runtime_bootstrap.h"
#include "pack_progress_dialog.h"
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QStatusBar>
#include <QDateTime>
#include <QSettings>
#include <QScrollArea>
#include <QScreen>
#include <QDir>
#include <QCoreApplication>
#include <QApplication>
#include <QFileInfo>
#include <QThread>
#include <QMimeData>
#include <QUrl>
#include <QList>
#include <QTimer>
#include <QCheckBox>
#include <QRadioButton>
#include <QGridLayout>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QColor>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      inputFilePath(""),
      outputDirPath(""),
      controller(nullptr)
{
    // 创建控制器
    controller = new ObfuscatorController(this);
    
    // 连接信号
    connect(controller, &ObfuscatorController::progressUpdated,
            this, &MainWindow::onObfuscatorProgress);
    connect(controller, &ObfuscatorController::logMessage,
            this, &MainWindow::onObfuscatorLog);
    connect(controller, &ObfuscatorController::finished,
            this, &MainWindow::onObfuscatorFinished);
    connect(controller, &ObfuscatorController::errorOccurred,
            this, &MainWindow::onObfuscatorError);
    
    // 加壳流水线
    pipeline = new PackerPipeline(this);
    connect(pipeline, &PackerPipeline::staged,
            this, &MainWindow::onPipelineStaged);
    connect(pipeline, &PackerPipeline::log,
            this, &MainWindow::onPipelineLog);
    connect(pipeline, &PackerPipeline::finished,
            this, &MainWindow::onPipelineFinished);
    
    // 实时进度窗。提前建好但先不显示,每次开跑前 reset()。
    progressDialog = new PackProgressDialog(this);
    connect(progressDialog, &PackProgressDialog::stopRequested,
            pipeline, &PackerPipeline::requestStop);
    
    setupUI();
    loadSettings();
    
    // 启用拖拽
    setAcceptDrops(true);
    
    // 设置窗口属性
    setWindowTitle("AntiHackerX - Java 混淆加壳工具");

    // 窗口尺寸按当前屏幕可用区域自适应 ——
    // 1080P 开了系统缩放(125%/150%)后逻辑分辨率可能只有 1280x720,
    // 写死 840 高会超出屏幕、底部按钮直接看不见。
    {
        QSize want(1060, 840);
        if (const QScreen *scr = screen()) {
            const QRect avail = scr->availableGeometry();
            want.setWidth(qMin(want.width(), avail.width() - 48));
            want.setHeight(qMin(want.height(), avail.height() - 96));
        }
        resize(want);
        setMinimumSize(qMin(720, want.width()), qMin(420, want.height()));
    }
    
    // 状态栏
    statusBar()->showMessage("就绪");
}

MainWindow::~MainWindow() {
    saveSettings();

    // 停止进程
    if (controller && controller->isRunning()) {
        controller->stop();
    }
    if (pipeline && pipeline->isRunning()) {
        pipeline->requestStop();
    }
    // Qt 会自动管理子对象的内存
}

void MainWindow::applyRuntime(const QString &javaExe,
                              const QString &javaHome,
                              const QString &jarPath,
                              const QStringList &bootLog) {
    runtimeJavaExe = javaExe;
    runtimeJavaHome = javaHome;

    // 把启动引导阶段的日志回放到日志面板,用户能看到装了什么、装在哪
    if (!bootLog.isEmpty()) {
        appendLog("===== 启动环境检查 =====");
        for (const QString &line : bootLog) {
            appendLog(line);
        }
        appendLog("=========================");
    }

    // 指定 Java 运行时(避免依赖裸 "java" 命令)
    if (!javaExe.isEmpty()) {
        controller->setJavaExecutable(javaExe);
        controller->setJavaHome(javaHome);
    }

    // 优先使用引导阶段已经确认好的 jar
    if (!jarPath.isEmpty() && QFileInfo::exists(jarPath)) {
        controller->setObfuscatorJarPath(jarPath);
        appendLog(QString("找到 native-obfuscator: %1").arg(jarPath));
        return;
    }

    // 兜底:再按旧规则和配置搜索一次
    loadSettings();
}

void MainWindow::setupUI() {
    createMenuBar();
    createCentralWidget();
}

void MainWindow::createMenuBar() {
    // 文件菜单
    QMenu *fileMenu = menuBar()->addMenu("文件(&F)");
    
    QAction *openAction = new QAction("打开 JAR 文件(&O)...", this);
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::onSelectInputFile);
    fileMenu->addAction(openAction);
    
    fileMenu->addSeparator();
    
    QAction *exitAction = new QAction("退出(&X)", this);
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);
    fileMenu->addAction(exitAction);
    
    // 工具菜单
    QMenu *toolsMenu = menuBar()->addMenu("工具(&T)");
    
    QAction *settingsAction = new QAction("设置(&S)...", this);
    toolsMenu->addAction(settingsAction);
    
    QAction *redownloadAction = new QAction("重新下载运行依赖(&R)...", this);
    connect(redownloadAction, &QAction::triggered, this, &MainWindow::onRedownloadDependency);
    toolsMenu->addAction(redownloadAction);
    
    QAction *reinstallJavaAction = new QAction("重新安装 Java 运行环境(&J)...", this);
    connect(reinstallJavaAction, &QAction::triggered, this, &MainWindow::onReinstallJavaRuntime);
    toolsMenu->addAction(reinstallJavaAction);
    
    // 帮助菜单
    QMenu *helpMenu = menuBar()->addMenu("帮助(&H)");
    
    QAction *aboutAction = new QAction("关于(&A)...", this);
    connect(aboutAction, &QAction::triggered, [this]() {
        QMessageBox::about(this, "关于 AntiHackerX",
            "AntiHackerX v1.0.0\n\n"
            "Java 程序防逆向加密混淆工具\n"
            "基于 native-obfuscator\n\n"
            "许可证: GNU GPL v3.0\n"
            "© 2026 H3K4");
    });
    helpMenu->addAction(aboutAction);
}

void MainWindow::createCentralWidget() {
    QWidget *centralWidget = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    
    // ===== 文件选择区域 =====
    QGroupBox *fileGroup = new QGroupBox("文件选择", centralWidget);
    QVBoxLayout *fileLayout = new QVBoxLayout(fileGroup);
    
    // 输入文件
    QHBoxLayout *inputLayout = new QHBoxLayout();
    lblInputFile = new QLabel("未选择文件", fileGroup);
    lblInputFile->setStyleSheet("QLabel { padding: 5px; border: 1px solid #ccc; background: #f9f9f9; }");
    btnSelectInput = new QPushButton("选择输入 JAR", fileGroup);
    inputLayout->addWidget(new QLabel("输入文件:"), 0);
    inputLayout->addWidget(lblInputFile, 1);
    inputLayout->addWidget(btnSelectInput, 0);
    fileLayout->addLayout(inputLayout);
    
    // 输出目录
    QHBoxLayout *outputLayout = new QHBoxLayout();
    lblOutputDir = new QLabel("未选择目录", fileGroup);
    lblOutputDir->setStyleSheet("QLabel { padding: 5px; border: 1px solid #ccc; background: #f9f9f9; }");
    btnSelectOutput = new QPushButton("选择输出目录", fileGroup);
    outputLayout->addWidget(new QLabel("输出目录:"), 0);
    outputLayout->addWidget(lblOutputDir, 1);
    outputLayout->addWidget(btnSelectOutput, 0);
    fileLayout->addLayout(outputLayout);
    
    mainLayout->addWidget(fileGroup);
    
    // ===== JAR 信息显示区域 =====
    QGroupBox *jarInfoGroup = new QGroupBox("JAR 文件信息", centralWidget);
    QVBoxLayout *jarInfoLayout = new QVBoxLayout(jarInfoGroup);
    
    // 类型标签
    QHBoxLayout *typeLayout = new QHBoxLayout();
    typeLayout->addWidget(new QLabel("文件类型:"));
    lblJarType = new QLabel("未识别");
    lblJarType->setStyleSheet("QLabel { font-weight: bold; color: #666; }");
    typeLayout->addWidget(lblJarType);
    typeLayout->addStretch();
    jarInfoLayout->addLayout(typeLayout);
    
    // 详细信息
    txtJarDetails = new QTextEdit(jarInfoGroup);
    txtJarDetails->setReadOnly(true);
    txtJarDetails->setMaximumHeight(56);
    txtJarDetails->setStyleSheet("QTextEdit { font-size: 9pt; background: #f5f5f5; }");
    txtJarDetails->setPlainText("将 JAR 文件拖拽到此窗口,或点击\"选择输入 JAR\"按钮");
    jarInfoLayout->addWidget(txtJarDetails);
    
    mainLayout->addWidget(jarInfoGroup);
    
    // ===== Minecraft Paper 插件加固配置 =====
    grpPaperPlugin = createPaperPluginGroup(centralWidget);
    // 配置面板很长(防护选项 + 混淆选项 + 要隐藏的类表格),小屏上会把下方的
    // 「开始加壳」挤出屏幕。给它一个限高的滚动容器:面板内部滚动,
    // 按钮与进度条永远留在可见位置。
    {
        QScrollArea *cfgScroll = new QScrollArea(centralWidget);
        cfgScroll->setWidgetResizable(true);
        cfgScroll->setFrameShape(QFrame::NoFrame);
        cfgScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        cfgScroll->setWidget(grpPaperPlugin);
        int maxH = 520;
        if (const QScreen *scr = screen()) {
            maxH = qMax(260, scr->availableGeometry().height() * 58 / 100);
        }
        cfgScroll->setMaximumHeight(maxH);
        mainLayout->addWidget(cfgScroll, 1);
    }

    // ===== 打包设置 =====
    QGroupBox *packGroup = new QGroupBox("打包设置", centralWidget);
    QGridLayout *packLayout = new QGridLayout(packGroup);
    packLayout->setHorizontalSpacing(10);

    packLayout->addWidget(new QLabel("目标平台:", packGroup), 0, 0);
    cmbTarget = new QComboBox(packGroup);
    cmbTarget->addItem(QStringLiteral("Windows x64(.dll)"));
    cmbTarget->addItem(QStringLiteral("Linux x64(.so)"));
    cmbTarget->addItem(QStringLiteral("两者都产出"));
    cmbTarget->setCurrentIndex(2);
    cmbTarget->setToolTip(
        "原生库要编成哪个平台的。只选一个能省一半编译时间;\n"
        "两个都装进同一个 JAR,运行时按当前系统自动选。");
    packLayout->addWidget(cmbTarget, 0, 1);

    packLayout->addWidget(new QLabel("产物:", packGroup), 0, 2);
    lblOutputJar = new QLabel("-", packGroup);
    lblOutputJar->setStyleSheet(
        "QLabel { padding: 4px; border: 1px solid #ccc; background: #f9f9f9; }");
    lblOutputJar->setTextInteractionFlags(Qt::TextSelectableByMouse);
    packLayout->addWidget(lblOutputJar, 0, 3, 1, 3);

    // ---- 服务端 API / 依赖 ---- 
    // Bukkit 插件对服务端 API 是 provided 作用域,插件 JAR 里没有 org.bukkit.*,
    // 而验证模块的 Paper 入口要 extends JavaPlugin —— 不给这份就编不过。
    packLayout->addWidget(new QLabel("服务端 API:", packGroup), 1, 0);
    lblExtraLibs = new QLabel(QStringLiteral("未指定(普通 JAR 不需要)"), packGroup);
    lblExtraLibs->setStyleSheet(
        "QLabel { padding: 4px; border: 1px solid #ccc; background: #f9f9f9; }");
    lblExtraLibs->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lblExtraLibs->setToolTip(
        "Paper/Bukkit 插件**必须**指定,否则验证模块编译不过。\n"
        "指到服务端 JAR(paper-xxx.jar)或服务端的 libraries/ 目录即可。");
    packLayout->addWidget(lblExtraLibs, 1, 1, 1, 3);

    QPushButton *btnExtraFile = new QPushButton(QStringLiteral("选 JAR..."), packGroup);
    QPushButton *btnExtraDir = new QPushButton(QStringLiteral("选目录..."), packGroup);
    QPushButton *btnExtraClear = new QPushButton(QStringLiteral("清空"), packGroup);
    btnExtraFile->setToolTip("选服务端 JAR / paper-api jar,可多选");
    btnExtraDir->setToolTip("选服务端的 libraries/ 目录,会递归展开其中的 jar");
    packLayout->addWidget(btnExtraFile, 1, 4);
    packLayout->addWidget(btnExtraDir, 1, 5);
    packLayout->addWidget(btnExtraClear, 1, 6);
    connect(btnExtraFile, &QPushButton::clicked, this, &MainWindow::onSelectExtraLibs);
    connect(btnExtraDir, &QPushButton::clicked, this, &MainWindow::onSelectExtraLibsDir);
    connect(btnExtraClear, &QPushButton::clicked, this, &MainWindow::onClearExtraLibs);
    packLayout->setColumnStretch(3, 1);

    mainLayout->addWidget(packGroup);
    
    // ===== 操作按钮区域 =====
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    btnStart = new QPushButton("开始混淆", centralWidget);
    btnStart->setEnabled(false);
    btnStart->setStyleSheet("QPushButton { padding: 10px; font-size: 14px; font-weight: bold; }");
    
    btnStop = new QPushButton("停止", centralWidget);
    btnStop->setEnabled(false);
    btnStop->setStyleSheet("QPushButton { padding: 10px; font-size: 14px; }");
    
    buttonLayout->addStretch();
    buttonLayout->addWidget(btnStart);
    buttonLayout->addWidget(btnStop);
    buttonLayout->addStretch();
    mainLayout->addLayout(buttonLayout);
    
    // ===== 进度条 =====
    progressBar = new QProgressBar(centralWidget);
    progressBar->setVisible(false);
    mainLayout->addWidget(progressBar);
    
    // ===== 日志区域 =====
    QGroupBox *logGroup = new QGroupBox("日志输出", centralWidget);
    QVBoxLayout *logLayout = new QVBoxLayout(logGroup);
    txtLog = new QTextEdit(logGroup);
    txtLog->setReadOnly(true);
    txtLog->setStyleSheet("QTextEdit { font-family: 'Courier New', monospace; font-size: 10pt; }");
    logLayout->addWidget(txtLog);

    // 按钮必须显式接线 —— 之前漏了输入/输出/开始这三个,点了完全没反应,
    // 只有菜单里的「打开」能用。
    connect(btnSelectInput, &QPushButton::clicked, this, &MainWindow::onSelectInputFile);
    connect(btnSelectOutput, &QPushButton::clicked, this, &MainWindow::onSelectOutputDir);
    connect(btnStart, &QPushButton::clicked, this, &MainWindow::onStartObfuscation);
    connect(btnStop, &QPushButton::clicked, this, &MainWindow::onStopObfuscation);
    
    setCentralWidget(centralWidget);
    
    // 初始日志
    appendLog("AntiHackerX 已启动");
    appendLog("等待选择输入文件和输出目录...");
}

void MainWindow::loadSettings() {
    // 检测 native-obfuscator.jar 位置
    QString jarPath;
    
    // 1. 优先看 <EXE 同级>/libs/ —— 这是启动引导下载依赖时放置的位置
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString libsPath = RuntimeBootstrap::jarPathForRoot(appDir);
    if (QFileInfo::exists(libsPath)) {
        jarPath = libsPath;
    }
    
    // 2. 检查 lib/ 目录
    QString libPath = appDir + "/../lib/native-obfuscator-mod.jar";
    if (jarPath.isEmpty() && QFileInfo::exists(libPath)) {
        jarPath = libPath;
    }
    
    // 3. 检查 tools/ 目录 (向后兼容)
    if (jarPath.isEmpty()) {
        QString toolsPath = appDir + "/../tools/native-obfuscator.jar";
        if (QFileInfo::exists(toolsPath)) {
            jarPath = toolsPath;
        }
    }
    
    // 4. 从配置读取
    if (jarPath.isEmpty()) {
        QSettings settings;
        jarPath = settings.value("obfuscator/path", "").toString();
    }
    
    if (!jarPath.isEmpty() && QFileInfo::exists(jarPath)) {
        controller->setObfuscatorJarPath(jarPath);
        appendLog(QString("找到 native-obfuscator: %1").arg(jarPath));
    } else {
        appendLog("[警告] 未找到 native-obfuscator.jar");
        appendLog("可通过菜单 工具 → 重新下载运行依赖 自动获取");
    }

    // ---- 恢复上次的界面选择 ----
    QSettings settings;
    if (cmbTarget) {
        const int target = settings.value("ui/targetPlatform", 2).toInt();
        if (target >= 0 && target < cmbTarget->count()) {
            cmbTarget->setCurrentIndex(target);
        }
    }
    if (chkAntiDebug)     chkAntiDebug->setChecked(settings.value("ui/antiDebug", true).toBool());
    if (chkAntiAgent)     chkAntiAgent->setChecked(settings.value("ui/antiAgent", true).toBool());
    if (chkAntiTamper)    chkAntiTamper->setChecked(settings.value("ui/antiTamper", false).toBool());
    if (chkAntiVmRun)     chkAntiVmRun->setChecked(settings.value("ui/antiVmRun", true).toBool());
    if (chkVMProtect)     chkVMProtect->setChecked(settings.value("ui/vmProtect", false).toBool());
    if (chkCopyright)     chkCopyright->setChecked(settings.value("ui/copyright", true).toBool());
    if (chkHideMainClass) chkHideMainClass->setChecked(settings.value("ui/hideMainClass", true).toBool());
    if (radEncryptDeep)   radEncryptDeep->setChecked(settings.value("ui/deepObfuscation", true).toBool());
    if (radEncryptOnly)   radEncryptOnly->setChecked(!settings.value("ui/deepObfuscation", true).toBool());

    // 混淆选项(逐项)。键名带 obf/ 前缀 —— 之前存过一套"全开"的旧键,
    // 换前缀后旧存档自然失效,用户能得到新的安全默认。
    const auto restoreObf = [&settings](QCheckBox *box, const char *key, bool def) {
        if (box) {
            box->setChecked(settings.value(QLatin1String(key), def).toBool());
        }
    };
    restoreObf(chkObfClassName,     "ui/obf/className", true);
    restoreObf(chkObfPackageName,   "ui/obf/packageName", false);
    restoreObf(chkObfMethodName,    "ui/obf/methodName", false);
    restoreObf(chkObfFieldName,     "ui/obf/fieldName", false);
    restoreObf(chkObfParamName,     "ui/obf/paramName", false);
    restoreObf(chkObfDeleteDebug,   "ui/obf/deleteDebug", false);
    restoreObf(chkObfEncryptString, "ui/obf/encryptString", true);
    restoreObf(chkObfAdvanceString, "ui/obf/advanceString", false);
    restoreObf(chkObfXor,           "ui/obf/xor", false);
    restoreObf(chkObfJunk,          "ui/obf/junk", true);
    restoreObf(chkObfHideMethod,    "ui/obf/hideMethod", true);
    restoreObf(chkObfHideField,     "ui/obf/hideField", true);
    restoreObf(chkObfAiNotice,      "ui/obf/aiNotice", false);
    if (cmbJunkLevel) {
        const int lvl = settings.value("ui/obf/junkLevel", 2).toInt();
        cmbJunkLevel->setCurrentIndex(qBound(0, lvl - 1, cmbJunkLevel->count() - 1));
    }

    // 输出目录:只在用户当初**手动选过**的情况下恢复。
    // 没选过就让它继续跟随输入 JAR —— 否则上次的目录会把默认行为顶掉。
    const QString lastOut = settings.value("ui/outputDir", "").toString();
    if (!lastOut.isEmpty() && QFileInfo::exists(lastOut)) {
        outputDirPath = lastOut;
        userChoseOutputDir = true;
        lblOutputDir->setText(lastOut);
        lblOutputDir->setToolTip(lastOut);
    }

    // 服务端 API 依赖(Paper 插件必填),上次用过就接着用
    extraClassPath = settings.value("ui/extraClassPath").toStringList();
    updateExtraLibsLabel();

    updatePackSummary();
}

void MainWindow::saveSettings() {
    QSettings settings;
    if (cmbTarget)      settings.setValue("ui/targetPlatform", cmbTarget->currentIndex());
    if (chkAntiDebug)   settings.setValue("ui/antiDebug", chkAntiDebug->isChecked());
    if (chkAntiAgent)   settings.setValue("ui/antiAgent", chkAntiAgent->isChecked());
    if (chkAntiTamper)  settings.setValue("ui/antiTamper", chkAntiTamper->isChecked());
    if (chkAntiVmRun)   settings.setValue("ui/antiVmRun", chkAntiVmRun->isChecked());
    if (chkVMProtect)   settings.setValue("ui/vmProtect", chkVMProtect->isChecked());
    if (chkCopyright)   settings.setValue("ui/copyright", chkCopyright->isChecked());
    if (chkHideMainClass) settings.setValue("ui/hideMainClass", chkHideMainClass->isChecked());
    if (radEncryptDeep) settings.setValue("ui/deepObfuscation", radEncryptDeep->isChecked());

    // 混淆选项
    const auto saveObf = [&settings](QCheckBox *box, const char *key) {
        if (box) {
            settings.setValue(QLatin1String(key), box->isChecked());
        }
    };
    saveObf(chkObfClassName,     "ui/obf/className");
    saveObf(chkObfPackageName,   "ui/obf/packageName");
    saveObf(chkObfMethodName,    "ui/obf/methodName");
    saveObf(chkObfFieldName,     "ui/obf/fieldName");
    saveObf(chkObfParamName,     "ui/obf/paramName");
    saveObf(chkObfDeleteDebug,   "ui/obf/deleteDebug");
    saveObf(chkObfEncryptString, "ui/obf/encryptString");
    saveObf(chkObfAdvanceString, "ui/obf/advanceString");
    saveObf(chkObfXor,           "ui/obf/xor");
    saveObf(chkObfJunk,          "ui/obf/junk");
    saveObf(chkObfHideMethod,    "ui/obf/hideMethod");
    saveObf(chkObfHideField,     "ui/obf/hideField");
    saveObf(chkObfAiNotice,      "ui/obf/aiNotice");
    if (cmbJunkLevel) {
        settings.setValue("ui/obf/junkLevel", cmbJunkLevel->currentIndex() + 1);
    }

    // 只在用户手动选过输出目录时才记住它;没选过就下次继续跟随输入 JAR
    if (userChoseOutputDir) {
        settings.setValue("ui/outputDir", outputDirPath);
    } else {
        settings.remove("ui/outputDir");
    }
    settings.setValue("ui/extraClassPath", extraClassPath);
}

void MainWindow::onRedownloadDependency() {
    if (controller->isRunning()) {
        QMessageBox::information(this, "提示", "任务执行中,请先停止再重下依赖。");
        return;
    }

    appendLog("===== 手动重新下载运行依赖 =====");
    statusBar()->showMessage("正在下载运行依赖...");

    // 进度由 RuntimeBootstrap 自己的模态进度窗展现,这里不要设 WaitCursor
    // —— 那是全局覆盖光标,会把下载窗的按钮也变成"等待",看起来像卡死。
    RuntimeBootstrap bootstrap(this);
    connect(&bootstrap, &RuntimeBootstrap::logMessage, this, &MainWindow::appendLog);
    const bool ok = bootstrap.refreshNativeObfuscator();

    if (ok) {
        controller->setObfuscatorJarPath(bootstrap.obfuscatorJarPath());
        appendLog(QString("依赖已更新: %1").arg(bootstrap.obfuscatorJarPath()));
        statusBar()->showMessage("依赖更新完成", 3000);
        QMessageBox::information(this, "提示",
                                 "运行依赖已更新到最新版:\n" + bootstrap.obfuscatorJarPath());
    } else {
        appendLog(QString("[ERROR] 依赖更新失败: %1").arg(bootstrap.errorMessage()));
        statusBar()->showMessage("依赖更新失败", 5000);
    }
}

void MainWindow::onReinstallJavaRuntime() {
    if (controller->isRunning()) {
        QMessageBox::information(this, "提示", "任务执行中,请先停止再重装 Java。");
        return;
    }

    const auto answer = QMessageBox::question(
        this, "重新安装 Java 运行环境",
        "将重新下载 Java 25 便携版并解压到程序目录下的 java/。\n\n"
        "· 来源:Adoptium Temurin(开源)\n"
        "· 体积:约 180~200 MB\n\n是否继续?");
    if (answer != QMessageBox::Yes) {
        return;
    }

    appendLog("===== 重新安装 Java 运行环境 =====");
    statusBar()->showMessage("正在下载 Java...");

    RuntimeBootstrap bootstrap(this);
    connect(&bootstrap, &RuntimeBootstrap::logMessage, this, &MainWindow::appendLog);
    const bool ok = bootstrap.installPortableJava();

    if (ok && bootstrap.refreshJavaDetection()) {
        runtimeJavaExe = bootstrap.javaExecutable();
        runtimeJavaHome = bootstrap.javaHome();
        controller->setJavaExecutable(runtimeJavaExe);
        controller->setJavaHome(runtimeJavaHome);
        appendLog(QString("Java 运行环境已更新: %1").arg(runtimeJavaExe));
        statusBar()->showMessage("Java 更新完成", 3000);
        QMessageBox::information(this, "提示",
                                 "Java 运行环境已更新:\n" + runtimeJavaExe);
    } else {
        appendLog(QString("[ERROR] Java 安装失败: %1").arg(bootstrap.errorMessage()));
        statusBar()->showMessage("Java 安装失败", 5000);
        QMessageBox::warning(this, "错误",
                             "Java 安装失败:\n" + bootstrap.errorMessage());
    }
}

void MainWindow::appendLog(const QString &message) {
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    QString logLine = QString("[%1] %2").arg(timestamp).arg(message);
    
    // 显示在界面
    txtLog->append(logLine);
    
    // 写入全局日志文件
    AppLogger::instance().log(message);
}

void MainWindow::onSelectInputFile() {
    QString fileName = QFileDialog::getOpenFileName(
        this,
        "选择输入 JAR 文件",
        QDir::homePath(),
        "JAR 文件 (*.jar);;所有文件 (*)"
    );
    
    setInputJar(fileName);
}

void MainWindow::openJarFile(const QString &path) {
    setInputJar(path);
}

void MainWindow::setAutoCloseProgress(bool enable) {
    m_autoCloseProgress = enable;
}

void MainWindow::setInputJar(const QString &path) {
    if (path.isEmpty()) {
        return;
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        appendLog(QString("[错误] 找不到文件: %1").arg(path));
        return;
    }
    if (!path.toLower().endsWith(QStringLiteral(".jar"))) {
        appendLog(QString("[错误] 不是 JAR 文件: %1").arg(path));
        return;
    }

    inputFilePath = info.absoluteFilePath();
    lblInputFile->setText(inputFilePath);
    lblInputFile->setToolTip(inputFilePath);
    appendLog(QString("已选择输入文件: %1").arg(inputFilePath));

    // 分析并显示 JAR 信息
    analyzeAndDisplayJar(inputFilePath);
    applyDefaultOutputDir();
    updatePackSummary();

    // 检查是否可以启动
    if (!outputDirPath.isEmpty() && !controller->getObfuscatorJarPath().isEmpty()) {
        btnStart->setEnabled(true);
    }
}

void MainWindow::onSelectOutputDir() {
    QString dirName = QFileDialog::getExistingDirectory(
        this,
        "选择输出目录",
        QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );
    
    if (!dirName.isEmpty()) {
        outputDirPath = dirName;
        userChoseOutputDir = true;   // 之后不再自动跟随输入 JAR
        lblOutputDir->setText(dirName);
        lblOutputDir->setToolTip(dirName);
        appendLog(QString("已选择输出目录: %1").arg(dirName));
        updatePackSummary();
        
        // 检查是否可以启动
        if (!inputFilePath.isEmpty() && !controller->getObfuscatorJarPath().isEmpty()) {
            btnStart->setEnabled(true);
        }
    }
}

void MainWindow::onStartObfuscation() {
    if (pipeline->isRunning()) {
        return;
    }

    // ----- 把界面上的勾选翻译成流水线配置 -----
    PackerPipeline::Config config;
    QString configError;
    if (!buildPipelineConfig(config, configError)) {
        appendLog(QString("[错误] %1").arg(configError));
        QMessageBox::warning(this, "无法开始", configError);
        return;
    }

    appendLog("===== 开始加壳 =====");
    appendLog(QString("输入    : %1").arg(config.inputJar));
    appendLog(QString("输出    : %1").arg(config.outputJar));
    appendLog(QString("入口模板: %1")
                      .arg(config.verifyOptions.kind == VerifyModule::Kind::BukkitPlugin
                                   ? "Paper 插件(JavaPlugin 委托)"
                                   : "普通 JAR(Main-Class)"));
    appendLog(QString("主类    : %1 -> 由生成的入口拉起").arg(config.originalMainClass));
    appendLog(QString("混淆强度: %1")
                      .arg(config.enableJunk
                                   ? "深度混淆(类名/方法名/字段名/字符串/花指令)"
                                   : "仅加密(类名保形,只做字符串加密)"));

    // 界面上还没接通的开关,必须说清楚,不能让用户以为生效了
    if (chkAntiTamper && chkAntiTamper->isChecked()) {
        appendLog("[提示] 「反篡改」将对产物里全部非类文件(p.dat / *.so / plugin.yml 等)"
                  "做 ECDSA-P256 签名校验,被修改后插件拒绝加载。");
    }
    if (chkVMProtect && chkVMProtect->isChecked()) {
        appendLog("[提示] 「VM 保护」尚未实现,本次忽略。");
    }
    const QStringList hideClasses = selectedHideClasses();
    if (!hideClasses.isEmpty()) {
        appendLog(QString("原生化类: %1 个(会连同验证模块一起编进原生库)")
                          .arg(hideClasses.size()));
    }
    if (chkHideMainClass && !chkHideMainClass->isChecked()) {
        appendLog("[提示] 未勾选「隐藏主类」,但流水线当前始终由生成的入口拉起主类。");
    }

    // ----- 进入忙碌态 -----
    btnStart->setEnabled(false);
    btnSelectInput->setEnabled(false);
    btnSelectOutput->setEnabled(false);
    btnStop->setEnabled(true);

    progressBar->setVisible(true);
    progressBar->setRange(0, 11);   // 真实步数由 staged() 信号里的 total 覆盖
    progressBar->setValue(0);
    statusBar()->showMessage("正在加壳...");

    // 弹出实时进度窗。exec() 开的是嵌套事件循环,流水线在后台线程里跑,
    // 信号照常送达 —— 所以界面不会假死,中止按钮也能点。
    progressDialog->reset();
    pipeline->startAsync(config);
    progressDialog->exec();
}

void MainWindow::onStopObfuscation() {
    if (pipeline->isRunning()) {
        appendLog("用户请求停止...");
        pipeline->requestStop();

        btnStop->setEnabled(false);
        statusBar()->showMessage("正在停止...", 3000);
        return;
    }

    if (controller->isRunning()) {
        appendLog("用户请求停止...");
        controller->stop();

        btnStop->setEnabled(false);
        btnStart->setEnabled(true);
        btnSelectInput->setEnabled(true);
        btnSelectOutput->setEnabled(true);

        statusBar()->showMessage("已停止", 3000);
    }
}

// ===========================================================================
// PackerPipeline 的槽
// ===========================================================================

void MainWindow::onPipelineStaged(int index, int total, const QString &name) {
    progressBar->setRange(0, total);
    progressBar->setValue(index);
    progressDialog->setStage(index, total, name);
    appendLog(QString("[%1/%2] %3").arg(index).arg(total).arg(name));
    statusBar()->showMessage(QString("第 %1/%2 步:%3").arg(index).arg(total).arg(name));
}

void MainWindow::onPipelineLog(const QString &line) {
    // 逐行细节只进进度窗和日志文件。
    // 主面板日志留给"步骤级"信息 —— NOBF 会给每个类打一行,大插件上万行,
    // 全灌进主面板会把有用的东西冲走。
    progressDialog->appendLog(line);
    AppLogger::instance().log(line);
}

void MainWindow::onPipelineFinished(bool ok, const QString &message) {
    appendLog("===== 加壳结束 =====");
    appendLog(message);
    progressDialog->markFinished(ok, message);

    btnStart->setEnabled(true);
    btnSelectInput->setEnabled(true);
    btnSelectOutput->setEnabled(true);
    btnStop->setEnabled(false);
    progressBar->setValue(ok ? progressBar->maximum() : 0);

    statusBar()->showMessage(ok ? "加壳完成" : "加壳失败", 5000);
    // 不再另弹 QMessageBox —— 进度窗自己就是结果展示,弹两个反而碍事

    if (m_autoCloseProgress) {
        progressDialog->accept();
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    }
}

void MainWindow::onObfuscatorProgress(int progress) {
    progressBar->setValue(progress);
}

void MainWindow::onObfuscatorLog(const QString &message) {
    appendLog(message);
}

void MainWindow::onObfuscatorFinished(bool success, const QString &message) {
    appendLog("===== 处理完成 =====");
    appendLog(message);
    
    // 恢复按钮
    btnStart->setEnabled(true);
    btnSelectInput->setEnabled(true);
    btnSelectOutput->setEnabled(true);
    btnStop->setEnabled(false);
    
    progressBar->setValue(success ? 100 : 0);
    
    statusBar()->showMessage(success ? "处理完成" : "处理失败", 3000);
    
    if (success) {
        QMessageBox::information(this, "提示", "混淆处理已完成！\n\n输出目录: " + outputDirPath);
    } else {
        QMessageBox::warning(this, "错误", "混淆处理失败:\n" + message);
    }
}

void MainWindow::onObfuscatorError(const QString &error) {
    appendLog(QString("[ERROR] %1").arg(error));
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    // 检查是否包含文件
    if (event->mimeData()->hasUrls()) {
        QList<QUrl> urls = event->mimeData()->urls();
        if (!urls.isEmpty()) {
            QString filePath = urls.first().toLocalFile();
            // 只接受 .jar 文件
            if (filePath.toLower().endsWith(".jar")) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent *event) {
    QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty()) {
        return;
    }
    
    QString filePath = urls.first().toLocalFile();
    if (!filePath.toLower().endsWith(".jar")) {
        QMessageBox::warning(this, "错误", "请拖入 JAR 文件!");
        return;
    }
    
    // 和菜单/按钮/命令行走同一条路
    setInputJar(filePath);
    
    event->acceptProposedAction();
}

void MainWindow::analyzeAndDisplayJar(const QString &jarPath) {
    appendLog("正在分析 JAR 文件...");
    
    // 分析 JAR
    JarAnalysisResult result = JarAnalyzer::analyze(jarPath);
    currentJarResult = result;
    currentPluginMainClass = result.mainClass;
    
    if (!result.isValid) {
        appendLog(QString("[错误] %1").arg(result.errorMessage));
        lblJarType->setText("无效文件");
        lblJarType->setStyleSheet("QLabel { font-weight: bold; color: #d32f2f; }");
        txtJarDetails->setPlainText(QString("错误: %1").arg(result.errorMessage));
        return;
    }
    
    // 更新显示
    updateJarInfo(result);
    
    // 记录日志
    appendLog(QString("识别为: %1").arg(result.typeName));
    if (!result.mainClass.isEmpty()) {
        appendLog(QString("  主类/入口: %1").arg(result.mainClass));
    }
    if (!result.pluginName.isEmpty()) {
        appendLog(QString("  名称: %1").arg(result.pluginName));
    }
    if (!result.version.isEmpty()) {
        appendLog(QString("  版本: %1").arg(result.version));
    }

    // Paper 插件:自动扫描可转换为 C++ 的类,并同步配置面板
    if (result.type == JarType::MinecraftPaperPlugin) {
        if (grpPaperPlugin) {
            QString title = "Minecraft Paper 插件加固配置";
            if (!result.pluginName.isEmpty()) {
                title += QString(" —— %1").arg(result.pluginName);
            }
            grpPaperPlugin->setTitle(title);
        }
        appendLog("检测到 Paper 插件,开始扫描可加固的类...");
        onScanClasses();
    } else {
        // 其他类型:清空类列表并给出说明
        if (tblClasses) {
            tblClasses->setRowCount(0);
        }
        if (lblScanSummary) {
            lblScanSummary->setText(
                QString("当前文件类型为「%1」,Paper 插件加固配置仅适用于 Paper 插件。")
                    .arg(result.typeName));
        }
        classScanResult = ClassScanResult();
    }
}

void MainWindow::updateJarInfo(const JarAnalysisResult &result) {
    // 设置类型标签
    lblJarType->setText(result.typeName);
    
    // 根据类型设置颜色
    QString color;
    switch (result.type) {
        case JarType::PlainJar:
            color = "#1976d2"; // 蓝色
            break;
        case JarType::MinecraftPaperPlugin:
            color = "#f57c00"; // 橙色
            break;
        case JarType::MinecraftFabricMod:
            color = "#7b1fa2"; // 紫色
            break;
        case JarType::MinecraftForgeMod:
            color = "#388e3c"; // 绿色
            break;
        case JarType::SpringBoot:
            color = "#00897b"; // 青色
            break;
        default:
            color = "#616161"; // 灰色
            break;
    }
    
    lblJarType->setStyleSheet(QString("QLabel { font-weight: bold; color: %1; }").arg(color));
    
    // 设置详细信息
    QString detailsText;
    
    if (!result.pluginName.isEmpty()) {
        detailsText += QString("名称: %1\n").arg(result.pluginName);
    }
    
    if (!result.version.isEmpty()) {
        detailsText += QString("版本: %1\n").arg(result.version);
    }
    
    if (!result.mainClass.isEmpty()) {
        detailsText += QString("主类: %1\n").arg(result.mainClass);
    }
    
    if (!result.details.isEmpty()) {
        detailsText += "\n详细信息:\n";
        for (const QString &detail : result.details) {
            detailsText += QString("• %1\n").arg(detail);
        }
    }
    
    if (detailsText.isEmpty()) {
        detailsText = "无详细信息";
    }
    
    txtJarDetails->setPlainText(detailsText.trimmed());
}

// ===========================================================================
// Minecraft Paper 插件加固配置
// ===========================================================================

QGroupBox *MainWindow::createPaperPluginGroup(QWidget *parent) {
    QGroupBox *group = new QGroupBox("Minecraft Paper 插件加固配置", parent);
    QVBoxLayout *groupLayout = new QVBoxLayout(group);
    groupLayout->setSpacing(4);

    // ---------- 防护选项 ----------
    QGroupBox *protectGroup = new QGroupBox("防护选项", group);
    QGridLayout *protectLayout = new QGridLayout(protectGroup);
    protectLayout->setHorizontalSpacing(20);

    chkAntiDebug = new QCheckBox("反调试", protectGroup);
    chkAntiDebug->setToolTip("检测并阻止调试器附加到 JVM 进程");

    chkAntiAgent = new QCheckBox("反 Agent", protectGroup);
    chkAntiAgent->setToolTip("阻止 Java Agent 动态注入(instrumentation)");

    chkAntiTamper = new QCheckBox("反篡改", protectGroup);
    chkAntiTamper->setToolTip("校验插件文件完整性,被修改后拒绝加载");

    chkAntiVmRun = new QCheckBox("反虚拟机运行", protectGroup);
    chkAntiVmRun->setToolTip("检测虚拟机 / 沙箱环境,拒绝在其中运行");

    chkVMProtect = new QCheckBox("VM 保护", protectGroup);
    chkVMProtect->setToolTip("把关键方法转换为自定义虚拟机字节码后执行");

    chkCopyright = new QCheckBox("版权弹窗", protectGroup);
    chkCopyright->setToolTip(
            "启动时展示加壳程序的版权与许可证信息。\n"
            "有桌面环境弹窗;服务器等无头环境自动改为写日志。\n"
            "Paper 插件一律只写日志 —— 弹框会阻塞插件启用。");
    chkCopyright->setChecked(true);

    protectLayout->addWidget(chkAntiDebug, 0, 0);
    protectLayout->addWidget(chkAntiAgent, 0, 1);
    protectLayout->addWidget(chkAntiTamper, 0, 2);
    protectLayout->addWidget(chkAntiVmRun, 1, 0);
    protectLayout->addWidget(chkVMProtect, 1, 1);
    protectLayout->addWidget(chkCopyright, 1, 2);
    protectLayout->setColumnStretch(3, 1);

    groupLayout->addWidget(protectGroup);

    // ---------- 对类的处理方式 + 主类 ----------
    QHBoxLayout *rowLayout = new QHBoxLayout();

    QGroupBox *modeGroup = new QGroupBox("对类的处理方式(二选一)", group);
    QVBoxLayout *modeLayout = new QVBoxLayout(modeGroup);
    radEncryptOnly = new QRadioButton("仅加密", modeGroup);
    radEncryptOnly->setToolTip("只对类做加密处理,保持原有结构,兼容性最好");
    radEncryptDeep = new QRadioButton("加密与深度混淆", modeGroup);
    radEncryptDeep->setToolTip("类名混淆 + 垃圾代码(L2) + 隐藏方法/字段 + 字符串加密。\n"
                               "包名/方法名/字段名/参数名保持原样 —— 实测这几项最容易和反射、序列化、第三方库冲突。\n"
                               "需要时可在下方的「混淆选项」里逐项打开。");
    radEncryptDeep->setChecked(true);
    modeLayout->addWidget(radEncryptOnly);
    modeLayout->addWidget(radEncryptDeep);
    modeLayout->addStretch();

    QGroupBox *mainClassGroup = new QGroupBox("主类", group);
    QVBoxLayout *mainClassLayout = new QVBoxLayout(mainClassGroup);
    chkHideMainClass = new QCheckBox("隐藏主类", mainClassGroup);
    chkHideMainClass->setToolTip("启用后由本工具生成的入口拉起您的类,原有主类会被混淆隐藏");
    QLabel *mainHint = new QLabel("启用该选项后,由本工具拉起您的类,\n原先的主类会被混淆隐藏。", mainClassGroup);
    mainHint->setStyleSheet("QLabel { color: #757575; font-size: 9pt; }");
    mainClassLayout->addWidget(chkHideMainClass);
    mainClassLayout->addWidget(mainHint);
    mainClassLayout->addStretch();

    rowLayout->addWidget(modeGroup, 1);
    rowLayout->addWidget(mainClassGroup, 1);
    groupLayout->addLayout(rowLayout);

    // ---------- 混淆选项(逐项可选) ----------
    // 「对类的处理方式」那两个单选按钮现在只是**预设**:
    // 选它们会一次性勾好下面这些复选框,之后仍可逐项手改。
    grpObfuscation = new QGroupBox("混淆选项", group);
    QGridLayout *obfLayout = new QGridLayout(grpObfuscation);
    obfLayout->setSpacing(4);
    obfLayout->setContentsMargins(8, 4, 8, 6);

    const auto makeObfCheck = [&](const QString &text, const QString &tip, bool def) {
        QCheckBox *box = new QCheckBox(text, grpObfuscation);
        box->setToolTip(tip);
        box->setChecked(def);
        return box;
    };

    chkObfClassName = makeObfCheck(
            "类名混淆(含引用修正)",
            "重命名所有类,并同步修正常量池、泛型签名、注解、invokedynamic 等处的引用。关闭后类名保持原样。",
            true);
    chkObfPackageName = makeObfCheck(
            "包名混淆(含引用修正)",
            "重命名包路径(类被移动到新包),引用一并修正。插件若被反射/资源路径引用会出问题,默认关。",
            false);
    chkObfMethodName = makeObfCheck(
            "方法名混淆(含引用修正)",
            "重命名非接口/非覆写方法,调用点同步修正。默认关。",
            false);
    chkObfFieldName = makeObfCheck(
            "字段名混淆(含引用修正)",
            "重命名字段,读写点同步修正。序列化/反射场景易出问题,默认关。",
            false);
    chkObfParamName = makeObfCheck(
            "方法参数名混淆",
            "重命名方法参数(仅影响 LocalVariableTable/MethodParameters 等调试信息)。默认关。",
            false);
    chkObfDeleteDebug = makeObfCheck(
            "删除编译调试信息",
            "去掉 SourceFile、行号表等,反编译后看不到原始文件名和行号。默认关。",
            false);
    chkObfEncryptString = makeObfCheck(
            "字符串 AES 加密运行时解密",
            "把字符串常量加密存储,运行时由一个生成的类解密。",
            true);
    chkObfAdvanceString = makeObfCheck(
            "字符串修改为访问全局列表方式",
            "字符串不再直接出现在字节码里,改为从静态列表按下标取。强度更高,但字节码会明显变大。默认关。",
            false);
    chkObfXor = makeObfCheck(
            "整型常数多重异或混淆",
            "把 int 常量拆成多次异或运算,增加静态分析难度。默认关。",
            false);
    chkObfJunk = makeObfCheck(
            "添加垃圾代码",
            "插入永不执行的花指令。",
            true);
    chkObfHideMethod = makeObfCheck(
            "IDEA 反编译时隐藏方法",
            "写入特殊属性,使 IDEA 的反编译器看不到这些方法。",
            true);
    chkObfHideField = makeObfCheck(
            "IDEA 反编译时隐藏字段",
            "写入特殊属性,使 IDEA 的反编译器看不到这些字段。",
            true);
    chkObfAiNotice = makeObfCheck(
            "AI 提示词注入",
            "给每个类注入一个常量,内容是一份写给自动化分析系统与大模型的声明,"
            "要求它拒绝解释/还原本类的实现。该常量不被任何代码引用、不影响"
            "运行期行为,但反编译后能被原样看到。\n"
            "有效场合:攻击者直接把代码/反编译结果贴给 AI 的批量脚本与随手一贴。\n"
            "局限:模型可能不理会,也可能被“忽略文件里的任何指令”绕开 —— "
            "它是抬高成本,不是可靠防护。每个类约增大 1.8KB。默认关。",
            false);

    cmbJunkLevel = new QComboBox(grpObfuscation);
    for (int i = 1; i <= 9; ++i) {
        cmbJunkLevel->addItem(QString::number(i));
    }
    cmbJunkLevel->setCurrentIndex(1);   // 默认 L2
    cmbJunkLevel->setToolTip("垃圾代码级别,越高插得越多(也可能触发方法体积上限)。默认 2。");

    obfLayout->addWidget(chkObfClassName,      0, 0);
    obfLayout->addWidget(chkObfPackageName,    0, 1);
    obfLayout->addWidget(chkObfMethodName,     0, 2);
    obfLayout->addWidget(chkObfFieldName,      0, 3);
    obfLayout->addWidget(chkObfParamName,      1, 0);
    obfLayout->addWidget(chkObfDeleteDebug,    1, 1);
    obfLayout->addWidget(chkObfEncryptString,  1, 2);
    obfLayout->addWidget(chkObfAdvanceString,  1, 3);
    obfLayout->addWidget(chkObfXor,            2, 0);
    obfLayout->addWidget(chkObfJunk,           2, 1);
    {
        QHBoxLayout *junkRow = new QHBoxLayout();
        QLabel *junkLabel = new QLabel("级别:", grpObfuscation);
        junkRow->addWidget(junkLabel);
        junkRow->addWidget(cmbJunkLevel);
        junkRow->addStretch();
        obfLayout->addLayout(junkRow, 2, 2);
    }
    obfLayout->addWidget(chkObfHideMethod,     2, 3);
    obfLayout->addWidget(chkObfHideField,      3, 0);
    obfLayout->addWidget(chkObfAiNotice,       3, 1);
    groupLayout->addWidget(grpObfuscation);

    // 两个单选按钮 = 预设:切换时批量勾选下面的复选框
    // 「深度融合」只开实测最安全的那几项:类名 + 垃圾代码(L2) + 隐藏方法/字段 + 字符串加密。
    // 包名/方法名/字段名/参数名混淆一律不动 —— 它们与反射、序列化、第三方库冲突最多。
    const auto applyPreset = [this](bool deep) {
        if (chkObfClassName)     chkObfClassName->setChecked(deep);
        if (chkObfPackageName)   chkObfPackageName->setChecked(false);
        if (chkObfMethodName)    chkObfMethodName->setChecked(false);
        if (chkObfFieldName)     chkObfFieldName->setChecked(false);
        if (chkObfParamName)     chkObfParamName->setChecked(false);
        if (chkObfXor)           chkObfXor->setChecked(false);
        if (chkObfJunk)          chkObfJunk->setChecked(deep);
        if (chkObfHideMethod)    chkObfHideMethod->setChecked(deep);
        if (chkObfHideField)     chkObfHideField->setChecked(deep);
        if (chkObfEncryptString) chkObfEncryptString->setChecked(true);  // 两种模式都加密字符串
        if (chkObfDeleteDebug)   chkObfDeleteDebug->setChecked(false);
        if (cmbJunkLevel)        cmbJunkLevel->setCurrentIndex(deep ? 1 : 0);   // L2 / L1
    };
    connect(radEncryptDeep, &QRadioButton::toggled, this, [applyPreset](bool on) {
        if (on) {
            applyPreset(true);
        }
    });
    connect(radEncryptOnly, &QRadioButton::toggled, this, [applyPreset](bool on) {
        if (on) {
            applyPreset(false);
        }
    });
    applyPreset(true);

    // ---------- 要隐藏的类 ----------
    QGroupBox *classGroup = new QGroupBox("要隐藏的类", group);
    QVBoxLayout *classLayout = new QVBoxLayout(classGroup);

    QHBoxLayout *toolLayout = new QHBoxLayout();
    btnScanClasses = new QPushButton("扫描可转换为 C++ 的类", classGroup);
    btnSelectRecommended = new QPushButton("仅勾选插件类", classGroup);
    btnSelectRecommended->setToolTip("勾选可转换、且不属于第三方库的类");
    btnSelectAllClasses = new QPushButton("全选", classGroup);
    btnSelectNoneClasses = new QPushButton("全不选", classGroup);

    toolLayout->addWidget(btnScanClasses);
    toolLayout->addSpacing(16);
    toolLayout->addWidget(btnSelectRecommended);
    toolLayout->addWidget(btnSelectAllClasses);
    toolLayout->addWidget(btnSelectNoneClasses);
    toolLayout->addStretch();
    classLayout->addLayout(toolLayout);

    lblScanSummary = new QLabel(
        "尚未扫描。选择 Paper 插件 JAR 后会自动扫描,也可手动点击左侧按钮。", classGroup);
    lblScanSummary->setWordWrap(true);
    lblScanSummary->setStyleSheet("QLabel { color: #616161; }");
    classLayout->addWidget(lblScanSummary);

    tblClasses = new QTableWidget(0, 4, classGroup);
    tblClasses->setHorizontalHeaderLabels({"隐藏", "类名", "可转换方法", "状态"});
    tblClasses->verticalHeader()->setVisible(false);
    tblClasses->setSelectionBehavior(QAbstractItemView::SelectRows);
    tblClasses->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tblClasses->setMaximumHeight(150);
    tblClasses->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tblClasses->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    tblClasses->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    tblClasses->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    classLayout->addWidget(tblClasses);

    groupLayout->addWidget(classGroup);

    // ---------- 信号连接 ----------
    connect(btnScanClasses, &QPushButton::clicked,
            this, &MainWindow::onScanClasses);
    connect(btnSelectRecommended, &QPushButton::clicked,
            this, &MainWindow::onSelectRecommendedClasses);
    connect(btnSelectAllClasses, &QPushButton::clicked,
            this, &MainWindow::onSelectAllConvertible);
    connect(btnSelectNoneClasses, &QPushButton::clicked,
            this, &MainWindow::onSelectNoneClasses);

    return group;
}

void MainWindow::onScanClasses() {
    if (inputFilePath.isEmpty()) {
        QMessageBox::information(this, "提示", "请先选择或拖入一个 JAR 文件。");
        return;
    }

    appendLog("正在扫描可转换为 C++ 的类...");
    QApplication::setOverrideCursor(Qt::WaitCursor);

    const QString rootPackage = ClassScanner::rootPackageOf(currentPluginMainClass);
    classScanResult = ClassScanner::scan(inputFilePath, rootPackage);

    QApplication::restoreOverrideCursor();

    if (!classScanResult.isValid) {
        appendLog(QString("[错误] 类扫描失败: %1").arg(classScanResult.errorMessage));
        lblScanSummary->setText(QString("扫描失败:%1").arg(classScanResult.errorMessage));
        tblClasses->setRowCount(0);
        return;
    }

    if (!classScanResult.detectedRootPackage.isEmpty()) {
        appendLog(QString("插件根包(自动推断): %1,该包之外的类已标记为第三方库")
                      .arg(classScanResult.detectedRootPackage));
    }

    appendLog(QString("类扫描完成:%1").arg(classScanResult.summaryText()));
    updateClassTable(classScanResult);
}

void MainWindow::onSelectAllConvertible() {
    setClassTableCheckState(true, true);
    appendLog(QString("已勾选全部可转换的类(%1 个)。").arg(selectedHideClasses().size()));
}

void MainWindow::onSelectNoneClasses() {
    setClassTableCheckState(false, false);
    appendLog("已取消勾选所有类。");
}

void MainWindow::onSelectRecommendedClasses() {
    int checked = 0;
    for (int row = 0; row < tblClasses->rowCount(); ++row) {
        QTableWidgetItem *checkItem = tblClasses->item(row, 0);
        if (!checkItem) {
            continue;
        }

        const bool convertible = checkItem->data(Qt::UserRole).toBool();
        const bool thirdParty = checkItem->data(Qt::UserRole + 1).toBool();
        const bool recommended = convertible && !thirdParty;

        if (!convertible) {
            checkItem->setCheckState(Qt::Unchecked);
            continue;
        }

        checkItem->setCheckState(recommended ? Qt::Checked : Qt::Unchecked);
        if (recommended) {
            ++checked;
        }
    }

    appendLog(QString("已按推荐勾选插件自身的类(%1 个),第三方库已排除。").arg(checked));
}

void MainWindow::updateClassTable(const ClassScanResult &result) {
    tblClasses->setRowCount(0);
    tblClasses->setRowCount(result.classes.size());

    lblScanSummary->setText(
        QString("%1 —— 默认已勾选插件自身的类(灰色为第三方库,不可转换的类无法勾选)")
            .arg(result.summaryText()));

    for (int row = 0; row < result.classes.size(); ++row) {
        const ScannedClass &sc = result.classes.at(row);

        // 第 0 列:勾选框
        QTableWidgetItem *checkItem = new QTableWidgetItem();
        if (sc.convertible) {
            checkItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            checkItem->setCheckState(sc.thirdParty ? Qt::Unchecked : Qt::Checked);
        } else {
            // 不可转换:禁止勾选
            checkItem->setFlags(Qt::ItemIsEnabled);
            checkItem->setCheckState(Qt::Unchecked);
        }
        checkItem->setData(Qt::UserRole, sc.convertible);
        checkItem->setData(Qt::UserRole + 1, sc.thirdParty);
        tblClasses->setItem(row, 0, checkItem);

        // 第 1 列:类名
        QTableWidgetItem *nameItem = new QTableWidgetItem(sc.className);
        nameItem->setToolTip(sc.entryPath);
        if (sc.thirdParty) {
            nameItem->setForeground(QColor("#9e9e9e"));
            nameItem->setText(QString("%1  [第三方库]").arg(sc.className));
        } else if (sc.convertible) {
            nameItem->setForeground(QColor("#1976d2"));
        } else {
            nameItem->setForeground(QColor("#bdbdbd"));
        }
        tblClasses->setItem(row, 1, nameItem);

        // 第 2 列:可转换方法数
        QTableWidgetItem *methodItem = new QTableWidgetItem(
            QString("%1 / %2").arg(sc.convertibleMethods).arg(sc.totalMethods));
        methodItem->setTextAlignment(Qt::AlignCenter);
        tblClasses->setItem(row, 2, methodItem);

        // 第 3 列:状态说明
        QTableWidgetItem *stateItem = new QTableWidgetItem(sc.reason);
        stateItem->setToolTip(sc.reason);
        if (sc.convertible) {
            stateItem->setForeground(QColor("#2e7d32"));
        } else {
            stateItem->setForeground(QColor("#9e9e9e"));
        }
        tblClasses->setItem(row, 3, stateItem);
    }
}

void MainWindow::setClassTableCheckState(bool checked, bool onlyConvertible) {
    for (int row = 0; row < tblClasses->rowCount(); ++row) {
        QTableWidgetItem *checkItem = tblClasses->item(row, 0);
        if (!checkItem) {
            continue;
        }

        const bool convertible = checkItem->data(Qt::UserRole).toBool();

        if (!convertible) {
            checkItem->setCheckState(Qt::Unchecked);
            continue;
        }
        // onlyConvertible 为 true 时不影响结果(此处本就可转换),
        // 保留参数以便后续扩展(例如按包名筛选)。
        Q_UNUSED(onlyConvertible);
        checkItem->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
}

QStringList MainWindow::selectedHideClasses() const {
    QStringList classes;
    for (int row = 0; row < tblClasses->rowCount(); ++row) {
        QTableWidgetItem *checkItem = tblClasses->item(row, 0);
        QTableWidgetItem *nameItem = tblClasses->item(row, 1);
        if (!checkItem || !nameItem) {
            continue;
        }
        if (checkItem->checkState() == Qt::Checked) {
            classes << nameItem->text().section(QStringLiteral("  ["), 0, 0);
        }
    }
    return classes;
}

// ===========================================================================
// 防护选项 -> 验证模块配置
// ===========================================================================

VerifyModule::Options MainWindow::currentVerifyOptions() const {
    VerifyModule::Options options;
    options.antiDebug = chkAntiDebug && chkAntiDebug->isChecked();
    options.antiAgent = chkAntiAgent && chkAntiAgent->isChecked();
    options.antiTamper = chkAntiTamper && chkAntiTamper->isChecked();
    options.antiVirtualMachine = chkAntiVmRun && chkAntiVmRun->isChecked();
    // 没建这个控件时默认开(版权提示是产品要求,默认打开)
    options.showCopyright = !chkCopyright || chkCopyright->isChecked();

    // 入口模板跟着 JAR 类型走:Paper 插件要用 JavaPlugin 委托,
    // 普通 JAR 用标准 Main-Class。
    options.kind = (currentJarResult.type == JarType::MinecraftPaperPlugin)
            ? VerifyModule::Kind::BukkitPlugin
            : VerifyModule::Kind::Plain;
    return options;
}

// ===========================================================================
// UI 勾选 -> PackerPipeline 配置
// ===========================================================================

QString MainWindow::buildOutputJarPath() const {
    if (inputFilePath.isEmpty() || outputDirPath.isEmpty()) {
        return QString();
    }
    const QFileInfo info(inputFilePath);
    return QDir(outputDirPath).filePath(info.completeBaseName() + QStringLiteral("-packed.jar"));
}

void MainWindow::applyDefaultOutputDir() {
    if (userChoseOutputDir || inputFilePath.isEmpty()) {
        return;
    }
    const QString dir = QFileInfo(inputFilePath).absolutePath();
    if (outputDirPath == dir) {
        return;
    }
    outputDirPath = dir;
    const QString shown = dir + QStringLiteral("   (默认与输入同目录)");
    lblOutputDir->setText(shown);
    lblOutputDir->setToolTip(dir);
    appendLog(QString("输出目录默认设为输入 JAR 所在目录: %1").arg(dir));
    appendLog("(想换目录就点「选择输出目录」,换过之后就不再自动跟随)");
}

void MainWindow::updatePackSummary() {
    if (!lblOutputJar) {
        return;
    }
    const QString out = buildOutputJarPath();
    lblOutputJar->setText(out.isEmpty() ? QStringLiteral("-") : out);
}

void MainWindow::updateExtraLibsLabel() {
    if (!lblExtraLibs) {
        return;
    }
    if (extraClassPath.isEmpty()) {
        lblExtraLibs->setText(QStringLiteral("未指定(普通 JAR 不需要)"));
        lblExtraLibs->setToolTip(
                "Paper/Bukkit 插件**必须**指定,否则验证模块编译不过。");
        return;
    }
    lblExtraLibs->setText(QStringLiteral("%1 项:%2")
                                  .arg(extraClassPath.size())
                                  .arg(extraClassPath.first()));
    lblExtraLibs->setToolTip(extraClassPath.join(QLatin1Char('\n')));
}

void MainWindow::onSelectExtraLibs() {
    const QStringList files = QFileDialog::getOpenFileNames(
            this,
            QStringLiteral("选择服务端 JAR / API 依赖"),
            QDir::homePath(),
            QStringLiteral("Java 归档 (*.jar *.zip);;所有文件 (*)"));
    if (files.isEmpty()) {
        return;
    }
    for (const QString &file : files) {
        if (!extraClassPath.contains(file)) {
            extraClassPath << file;
        }
    }
    updateExtraLibsLabel();
    appendLog(QStringLiteral("已添加 %1 个依赖 JAR").arg(files.size()));
}

void MainWindow::onSelectExtraLibsDir() {
    const QString dir = QFileDialog::getExistingDirectory(
            this,
            QStringLiteral("选择服务端的 libraries 目录"),
            QDir::homePath(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dir.isEmpty()) {
        return;
    }
    if (!extraClassPath.contains(dir)) {
        extraClassPath << dir;
    }
    updateExtraLibsLabel();
    appendLog(QStringLiteral("已添加依赖目录: %1").arg(dir));
}

void MainWindow::onClearExtraLibs() {
    extraClassPath.clear();
    updateExtraLibsLabel();
}

bool MainWindow::buildPipelineConfig(PackerPipeline::Config &config,
                                     QString &errorMessage) const {
    if (inputFilePath.isEmpty()) {
        errorMessage = QStringLiteral("请先选择要处理的 JAR 文件。");
        return false;
    }
    if (outputDirPath.isEmpty()) {
        errorMessage = QStringLiteral("请先选择输出目录。");
        return false;
    }
    if (currentPluginMainClass.isEmpty()) {
        errorMessage = QStringLiteral("没能从 JAR 里识别出主类,无法生成加壳入口。");
        return false;
    }

    config = PackerPipeline::defaultConfig();

    // ---- Java 工具链:必须和引导阶段选定的运行时同源 ----
    // 混用不同 JDK 的 java/javac 会出现 "class file has wrong version" 之类怪问题。
    QString javaBinDir;
    if (!runtimeJavaExe.isEmpty()) {
        config.javaExe = runtimeJavaExe;
        javaBinDir = QFileInfo(runtimeJavaExe).absolutePath();
    } else {
        config.javaExe = QStringLiteral("java");
    }
    const auto sibling = [&javaBinDir](const QString &name) {
        return javaBinDir.isEmpty() ? name : QDir(javaBinDir).filePath(name);
    };
    config.javacExe = sibling(QStringLiteral("javac"));
    config.jarExe = sibling(QStringLiteral("jar"));

    config.javaHome = runtimeJavaHome;
    if (config.javaHome.isEmpty()) {
        config.javaHome = javaBinDir.isEmpty()
                ? qEnvironmentVariable("JAVA_HOME")
                : QFileInfo(javaBinDir + QStringLiteral("/..")).absoluteFilePath();
    }

    // ---- 外部工具存在性检查 ----
    // 只检查绝对路径:像 "javac" 这种裸名字交给 PATH 去解析。
    const auto missing = [](const QString &path) {
        return path.isEmpty()
                || (path.contains(QLatin1Char('/')) && !QFileInfo::exists(path));
    };
    if (missing(config.javacExe)) {
        errorMessage = QStringLiteral(
                "找不到 javac:%1\n\n"
                "加壳需要完整的 JDK(不只是 JRE)来编译验证模块。\n"
                "请用菜单里的「重新安装 Java 运行时」获取带编译器的一版。")
                               .arg(config.javacExe);
        return false;
    }
    if (missing(config.obfuscatorJar)) {
        errorMessage = QStringLiteral(
                "找不到 jar-obfuscator:%1\n\n请用菜单里的「重新下载依赖」获取。")
                               .arg(config.obfuscatorJar);
        return false;
    }
    // NOBF 的路径以引导阶段确认过的为准
    const QString bootNobf = controller ? controller->getObfuscatorJarPath() : QString();
    if (!bootNobf.isEmpty() && QFileInfo::exists(bootNobf)) {
        config.nobfJar = bootNobf;
    }
    if (missing(config.nobfJar)) {
        errorMessage = QStringLiteral(
                "找不到 native-obfuscator:%1\n\n请用菜单里的「重新下载依赖」获取。")
                               .arg(config.nobfJar);
        return false;
    }
    if (missing(config.zigExe)) {
        errorMessage = QStringLiteral(
                "找不到 zig 交叉编译器:%1\n\n"
                "原生库要用 zig 交叉编译,请用菜单里的「重新下载依赖」获取。")
                               .arg(config.zigExe);
        return false;
    }
    // ahxPackerSrc 允许为空 —— 流水线会退回到内嵌资源

    // ---- 目标平台 ----
    switch (cmbTarget ? cmbTarget->currentIndex() : 2) {
    case 0:  config.target = PackerPipeline::Target::WindowsX64; break;
    case 1:  config.target = PackerPipeline::Target::LinuxX64;   break;
    default: config.target = PackerPipeline::Target::Both;       break;
    }

    // ---- 混淆选项:逐项来自界面上的复选框 ----
    // (上面那两个单选按钮只是预设,真正的取值一律以复选框为准)
    if (chkObfClassName) {
        config.enableClassName = chkObfClassName->isChecked();
        config.enablePackageName = chkObfPackageName->isChecked();
        config.enableMethodName = chkObfMethodName->isChecked();
        config.enableFieldName = chkObfFieldName->isChecked();
        config.enableParamName = chkObfParamName->isChecked();
        config.enableDeleteCompileInfo = chkObfDeleteDebug->isChecked();
        config.enableEncryptString = chkObfEncryptString->isChecked();
        config.enableAdvanceString = chkObfAdvanceString->isChecked();
        config.enableXor = chkObfXor->isChecked();
        config.enableJunk = chkObfJunk->isChecked();
        config.junkLevel = cmbJunkLevel ? (cmbJunkLevel->currentIndex() + 1) : 5;
        config.enableHideMethod = chkObfHideMethod->isChecked();
        config.enableHideField = chkObfHideField->isChecked();
        config.enableAiNotice = chkObfAiNotice->isChecked();

        // 关掉类名混淆后主类不会被改名,但仍然要靠 plugin.yml 找到它 ——
        // 这条路径上游已经能处理(映射表里没有主类条目时按原名继续)。
    }

    // ---- 其余 ----
    config.inputJar = QFileInfo(inputFilePath).absoluteFilePath();
    config.outputJar = buildOutputJarPath();
    config.originalMainClass = currentPluginMainClass;
    config.verifyOptions = currentVerifyOptions();

    // 「要隐藏的类」→ 这些类会被 NOBF 转成本地代码。
    // 表里给的是原始类名,流水线会在改名映射里把它们翻成混淆后的名字。
    config.hideClasses = selectedHideClasses();

    // Paper/Bukkit 插件必须给服务端 API,否则验证模块编不过
    config.extraClassPath = extraClassPath;
    if (config.verifyOptions.kind == VerifyModule::Kind::BukkitPlugin
        && extraClassPath.isEmpty()) {
        errorMessage = QStringLiteral(
                "这是 Paper/Bukkit 插件,但没指定服务端 API。\n\n"
                "插件 JAR 里不含 org.bukkit.*(那是服务端在运行时提供的),\n"
                "不给一份的话验证模块编译不过。\n\n"
                "请在「打包设置 → 服务端 API」里选服务端 JAR(如 paper-xxx.jar),\n"
                "或者选服务端的 libraries/ 目录。");
        return false;
    }

    return true;
}
