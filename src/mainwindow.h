#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QPushButton>
#include <QTextEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QProgressBar>
#include <QStringList>
#include <QCheckBox>
#include <QRadioButton>
#include <QTableWidget>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include "obfuscator_controller.h"
#include "jar_analyzer.h"
#include "class_scanner.h"
#include "verify_module.h"
#include "packer_pipeline.h"

class PackProgressDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    /**
     * 注入启动引导阶段确定的运行环境。
     * @param javaExe   要使用的 java 主程序(便携版或系统版)
     * @param javaHome  对应的 JAVA_HOME
     * @param jarPath   native-obfuscator.jar 的路径
     * @param bootLog   引导阶段产生的日志,回放到日志面板
     */
    void applyRuntime(const QString &javaExe,
                      const QString &javaHome,
                      const QString &jarPath,
                      const QStringList &bootLog);

    /** 直接打开一个 JAR(命令行参数 / 文件关联 / 拖拽都走这里) */
    void openJarFile(const QString &path);

    /** 自测钩子:加壳结束后自动关窗并退出。
     *  无桌面环境下没人点「关闭」,不设这个就只能干等超时。 */
    void setAutoCloseProgress(bool enable);

protected:
    // 拖拽事件
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void onSelectInputFile();
    void onSelectOutputDir();
    void onStartObfuscation();
    void onStopObfuscation();
    void onRedownloadDependency();
    void onReinstallJavaRuntime();

    // Minecraft Paper 插件配置
    void onScanClasses();
    void onSelectAllConvertible();
    void onSelectNoneClasses();
    void onSelectRecommendedClasses();

    // 额外类路径(Bukkit/Paper 插件必填)
    void onSelectExtraLibs();
    void onSelectExtraLibsDir();
    void onClearExtraLibs();
    
    // ObfuscatorController 的槽
    void onObfuscatorProgress(int progress);
    void onObfuscatorLog(const QString &message);
    void onObfuscatorFinished(bool success, const QString &message);
    void onObfuscatorError(const QString &error);

    // PackerPipeline 的槽
    void onPipelineStaged(int index, int total, const QString &name);
    void onPipelineLog(const QString &line);
    void onPipelineFinished(bool ok, const QString &message);

private:
    void setupUI();
    void createMenuBar();
    void createCentralWidget();
    void loadSettings();
    void saveSettings();
    /** 选定输入 JAR 后要做的一整套事:记下来、分析、同步输出目录与按钮状态。
     *  菜单/按钮/拖拽/命令行四条路都汇到这里,避免又漏接一处。 */
    void setInputJar(const QString &path);
    void appendLog(const QString &message);
    void analyzeAndDisplayJar(const QString &jarPath);
    void updateJarInfo(const JarAnalysisResult &result);

    // Minecraft Paper 插件配置面板
    QGroupBox *createPaperPluginGroup(QWidget *parent);
    void updateClassTable(const ClassScanResult &result);
    void setClassTableCheckState(bool checked, bool onlyConvertible);
    QStringList selectedHideClasses() const;

    // 从 UI 的「防护选项」收集配置,供打包时装配验证模块
    VerifyModule::Options currentVerifyOptions() const;

    // 由输入 JAR + 输出目录推导出最终产物路径
    QString buildOutputJarPath() const;
    // 用户没手动选过输出目录时,让它跟着输入 JAR 走
    void applyDefaultOutputDir();
    // 刷新「打包设置」里的产物预览,并对缺失的外部工具给出提示
    void updatePackSummary();
    // 刷新额外类路径的显示
    void updateExtraLibsLabel();
    // 把 UI 上的勾选翻译成流水线配置(返回 false 时 errorMessage 说明原因)
    bool buildPipelineConfig(PackerPipeline::Config &config, QString &errorMessage) const;
    
    // UI 组件
    QPushButton *btnSelectInput;
    QPushButton *btnSelectOutput;
    QPushButton *btnStart;
    QPushButton *btnStop;
    QLabel *lblInputFile;
    QLabel *lblOutputDir;
    QLabel *lblJarType;        // JAR 类型显示
    QTextEdit *txtJarDetails;  // JAR 详细信息显示
    QTextEdit *txtLog;
    QProgressBar *progressBar;

    // ===== Minecraft Paper 插件配置控件 =====
    QGroupBox *grpPaperPlugin;      // 整个配置组(仅 Paper 插件时启用)

    // 防护选项
    QCheckBox *chkAntiDebug;        // 反调试
    QCheckBox *chkAntiAgent;        // 反 Agent
    QCheckBox *chkAntiTamper;       // 反篡改
    QCheckBox *chkAntiVmRun;        // 反虚拟机内运行
    QCheckBox *chkVMProtect;        // VM 保护
    QCheckBox *chkCopyright;        // 启动时展示加壳版权提示

    // 对类的处理方式(二选一)
    QRadioButton *radEncryptOnly;   // 仅加密
    QRadioButton *radEncryptDeep;   // 加密与深度混淆

    // 主类
    QCheckBox *chkHideMainClass;    // 隐藏主类

    // ===== 混淆选项(逐项可选) =====
    QGroupBox *grpObfuscation = nullptr;      // 整个混淆选项组
    QCheckBox *chkObfClassName = nullptr;     // 类名混淆(含引用修正)
    QCheckBox *chkObfPackageName = nullptr;   // 包名混淆(含引用修正)
    QCheckBox *chkObfMethodName = nullptr;    // 方法名混淆(含引用修正)
    QCheckBox *chkObfFieldName = nullptr;     // 字段名混淆(含引用修正)
    QCheckBox *chkObfParamName = nullptr;     // 方法参数名混淆
    QCheckBox *chkObfDeleteDebug = nullptr;   // 删除编译调试信息
    QCheckBox *chkObfEncryptString = nullptr; // 字符串 AES 加密运行时解密
    QCheckBox *chkObfAdvanceString = nullptr; // 字符串改为访问全局列表
    QCheckBox *chkObfXor = nullptr;           // 整型常数多重异或混淆
    QCheckBox *chkObfJunk = nullptr;          // 添加垃圾代码
    QComboBox *cmbJunkLevel = nullptr;        // 垃圾代码级别(1-9)
    QCheckBox *chkObfHideMethod = nullptr;    // IDEA 反编译时隐藏方法
    QCheckBox *chkObfHideField = nullptr;     // IDEA 反编译时隐藏字段
    QCheckBox *chkObfAiNotice = nullptr;      // AI 提示词注入(防 AI 辅助破解)

    // 要隐藏的类
    QPushButton *btnScanClasses;
    QPushButton *btnSelectAllClasses;
    QPushButton *btnSelectNoneClasses;
    QPushButton *btnSelectRecommended;
    QLabel *lblScanSummary;
    QTableWidget *tblClasses;

    // ===== 打包设置控件 =====
    QComboBox *cmbTarget;           // 目标平台(Windows / Linux / 两者)
    QLabel *lblOutputJar;           // 产物路径预览
    QLabel *lblExtraLibs;           // 额外类路径预览

    /** 服务端 JAR / Bukkit-API / libraries 目录。
     *  Bukkit 插件对服务端 API 是 provided 作用域,插件 JAR 里根本没有
     *  org.bukkit.* —— 不给这份,验证模块的 Paper 入口编不过。 */
    QStringList extraClassPath;

    // 最近一次 JAR 分析结果(决定用哪种入口模板)
    JarAnalysisResult currentJarResult;

    // 扫描结果缓存
    ClassScanResult classScanResult;
    // 当前 JAR 识别出的主类(用于推导插件根包)
    QString currentPluginMainClass;
    
    // 数据
    QString inputFilePath;
    QString outputDirPath;
    /** 用户是否手动选过输出目录。没选过就让它始终跟着输入 JAR 走,
     *  这样大多数人根本不用碰这个按钮。 */
    bool userChoseOutputDir = false;
    
    // 启动引导阶段确定下来的运行环境
    QString runtimeJavaExe;
    QString runtimeJavaHome;
    
    // 控制器
    ObfuscatorController *controller;

    // 加壳流水线(真正干活的那个)
    PackerPipeline *pipeline;

    // 加壳过程中的实时进度窗(模态)
    PackProgressDialog *progressDialog;

    /** 见 setAutoCloseProgress() */
    bool m_autoCloseProgress = false;
};

#endif // MAINWINDOW_H
