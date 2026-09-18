# ObfuscatorController 模块文档

## 概述

`ObfuscatorController` 是 AntiHackerX 中负责调用外部 native-obfuscator-mod 进程的核心模块。通过进程隔离的方式调用,native-obfuscator 与 AntiHackerX 之间没有代码耦合。

## 架构设计

```
┌─────────────────────────────────────────┐
│         AntiHackerX (GPL-3.0)           │
│  ┌───────────────────────────────────┐  │
│  │    MainWindow (Qt GUI)            │  │
│  └─────────────┬─────────────────────┘  │
│                │ signals/slots           │
│  ┌─────────────▼─────────────────────┐  │
│  │  ObfuscatorController             │  │
│  │  - QProcess 管理                   │  │
│  │  - 参数构建                        │  │
│  │  - 输出解析                        │  │
│  └─────────────┬─────────────────────┘  │
└────────────────┼─────────────────────────┘
                 │ exec/fork
                 │ JSON/line protocol
┌────────────────▼─────────────────────────┐
│  native-obfuscator-mod.jar (GPL 3.0)    │
│  - Java → C++ 转换                       │
│  - JNI 代码生成                          │
└──────────────────────────────────────────┘
```

## 核心类: ObfuscatorController

### 公共接口

#### 构造函数
```cpp
ObfuscatorController(QObject *parent = nullptr);
```

#### 配置方法
```cpp
void setObfuscatorJarPath(const QString &path);
QString getObfuscatorJarPath() const;
```

#### 执行控制
```cpp
bool start(const Config &config);  // 启动混淆处理
void stop();                        // 停止处理
bool isRunning() const;             // 检查运行状态
```

### 配置结构

```cpp
struct Config {
    QString inputJarPath;      // 输入 JAR 文件路径 (必需)
    QString outputDirPath;     // 输出目录路径 (必需)
    QString blacklistPath;     // 黑名单文件路径 (可选)
    QString whitelistPath;     // 白名单文件路径 (可选)
    QString librariesDir;      // 依赖库目录 (可选)
    Platform platform;         // 目标平台 (HotSpot/StdJava/Android)
    bool useAnnotations;       // 是否使用注解
    QString customLibDir;      // 自定义库目录 (可选)
    bool debugMode;            // 调试模式
};
```

### 平台枚举

```cpp
enum class Platform {
    HotSpot,    // Oracle/OpenJDK HotSpot JVM (默认)
    StdJava,    // 标准 Java 运行时
    Android     // Android 平台 (Dalvik/ART)
};
```

### 信号 (Signals)

```cpp
void progressUpdated(int progress);              // 进度更新 (0-100)
void logMessage(const QString &message);         // 日志消息
void finished(bool success, const QString &msg); // 处理完成
void errorOccurred(const QString &error);        // 错误发生
```

## 使用示例

### 基本用法

```cpp
// 1. 创建控制器
ObfuscatorController *controller = new ObfuscatorController(this);

// 2. 设置 JAR 路径
controller->setObfuscatorJarPath("/path/to/native-obfuscator-mod.jar");

// 3. 连接信号
connect(controller, &ObfuscatorController::progressUpdated,
        this, [](int progress) {
    qDebug() << "进度:" << progress << "%";
});

connect(controller, &ObfuscatorController::logMessage,
        this, [](const QString &msg) {
    qDebug() << msg;
});

connect(controller, &ObfuscatorController::finished,
        this, [](bool success, const QString &msg) {
    if (success) {
        qDebug() << "成功:" << msg;
    } else {
        qDebug() << "失败:" << msg;
    }
});

// 4. 配置参数
ObfuscatorController::Config config;
config.inputJarPath = "/path/to/input.jar";
config.outputDirPath = "/path/to/output";
config.blacklistPath = "/path/to/blacklist.txt";
config.platform = ObfuscatorController::Platform::HotSpot;
config.useAnnotations = false;
config.debugMode = false;

// 5. 启动处理
if (!controller->start(config)) {
    qDebug() << "启动失败";
}
```

### 与 MainWindow 集成

```cpp
class MainWindow : public QMainWindow {
    Q_OBJECT
    
public:
    MainWindow(QWidget *parent = nullptr) {
        controller = new ObfuscatorController(this);
        
        // 连接信号到 UI
        connect(controller, &ObfuscatorController::progressUpdated,
                progressBar, &QProgressBar::setValue);
        
        connect(controller, &ObfuscatorController::logMessage,
                this, &MainWindow::appendLog);
        
        connect(controller, &ObfuscatorController::finished,
                this, &MainWindow::onProcessFinished);
    }
    
private slots:
    void onStartClicked() {
        ObfuscatorController::Config config;
        config.inputJarPath = inputLineEdit->text();
        config.outputDirPath = outputLineEdit->text();
        config.platform = ObfuscatorController::Platform::HotSpot;
        
        controller->start(config);
    }
    
private:
    ObfuscatorController *controller;
    QProgressBar *progressBar;
};
```

## 命令行参数映射

ObfuscatorController 自动将配置映射到命令行参数:

| Config 字段 | 命令行参数 | 示例 |
|-------------|-----------|------|
| inputJarPath | 位置参数1 | `input.jar` |
| outputDirPath | 位置参数2 | `output/` |
| blacklistPath | `-b <path>` | `-b config/blacklist.txt` |
| whitelistPath | `-w <path>` | `-w config/whitelist.txt` |
| librariesDir | `-l <dir>` | `-l libs/` |
| platform | `-p <platform>` | `-p hotspot` |
| useAnnotations | `-a` | `-a` |
| customLibDir | `--custom-lib-dir <dir>` | `--custom-lib-dir mylibs/` |
| debugMode | `--debug` | `--debug` |

完整命令示例:
```bash
java -jar native-obfuscator-mod.jar input.jar output/ \
    -b config/blacklist.txt \
    -l libs/ \
    -p hotspot \
    -a
```

## 输出解析

### 进度提取

控制器会自动解析 stdout 中的进度信息:

```
Processing 45/100 classes...   → 发射 progressUpdated(45)
Transpiling 80/100 methods...  → 发射 progressUpdated(80)
```

正则表达式: `(\d+)/(\d+)`

### 日志转发

所有 stdout 和 stderr 输出都会通过 `logMessage` 信号转发:

```cpp
void onProcessReadyReadStdOut() {
    QString output = QString::fromUtf8(process->readAllStandardOutput());
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    
    for (const QString &line : lines) {
        parseOutputLine(line);  // 提取进度
        emit logMessage(line);   // 转发日志
    }
}
```

## 错误处理

### 启动前检查

```cpp
bool start(const Config &config) {
    // 1. 检查 JAR 文件
    if (!QFileInfo::exists(obfuscatorJarPath)) {
        emit errorOccurred("找不到 native-obfuscator-mod.jar");
        return false;
    }
    
    // 2. 检查输入文件
    if (!QFileInfo::exists(config.inputJarPath)) {
        emit errorOccurred("找不到输入文件");
        return false;
    }
    
    // 3. 创建输出目录
    if (!outputDir.exists() && !outputDir.mkpath(".")) {
        emit errorOccurred("无法创建输出目录");
        return false;
    }
    
    // 4. 启动进程...
}
```

### 进程错误

```cpp
void onProcessError(QProcess::ProcessError error) {
    QString errorMsg;
    switch (error) {
        case QProcess::FailedToStart:
            errorMsg = "无法启动进程 (检查 Java 是否已安装)";
            break;
        case QProcess::Crashed:
            errorMsg = "进程崩溃";
            break;
        // ...
    }
    emit errorOccurred(errorMsg);
    emit finished(false, errorMsg);
}
```

### 退出码检查

```cpp
void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        emit finished(true, "处理成功完成");
    } else {
        emit finished(false, QString("处理失败 (退出码: %1)").arg(exitCode));
    }
}
```

## 生命周期管理

### 启动流程

1. **验证配置** → 检查文件、目录
2. **创建进程** → `new QProcess(this)`
3. **连接信号** → `readyReadStandardOutput`, `finished`, `errorOccurred`
4. **构建参数** → `buildArguments(config)`
5. **启动进程** → `process->start("java", args)`
6. **设置状态** → `running = true`

### 停止流程

1. **发送终止信号** → `process->terminate()`
2. **等待退出** → `process->waitForFinished(3000)`
3. **强制杀死** → `process->kill()` (如果超时)
4. **重置状态** → `running = false`

### 析构流程

```cpp
~ObfuscatorController() {
    if (process) {
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished();
        }
        delete process;
    }
}
```

## 线程安全

- **主线程运行**: 所有操作在 Qt 主线程中执行
- **信号机制**: 使用 Qt signals/slots 确保线程安全
- **进程隔离**: 外部进程独立运行,不影响 GUI 响应

## 部署要求

### 运行时依赖

1. **Java 运行时**: JRE 8+ 或 JDK 8+
   ```bash
   java -version  # 确认已安装
   ```

2. **native-obfuscator-mod.jar**: GPL 3.0 组件
   - 默认位置: `lib/native-obfuscator-mod.jar`
   - 备用位置: `tools/native-obfuscator.jar`
   - 自定义位置: 通过 `setObfuscatorJarPath()` 设置

### 目录结构

```
AntiHackerX/
├── bin/
│   └── AntiHackerX              # 可执行文件
├── lib/
│   └── native-obfuscator-mod.jar  # GPL 组件 (独立部署)
└── config/
    ├── blacklist.txt            # 黑名单配置
    └── whitelist.txt            # 白名单配置
```

## 许可证隔离验证

### 进程边界检查

```bash
# 运行时检查进程树
ps aux | grep -E "AntiHackerX|java.*obfuscator"

# 应该看到两个独立进程:
# xiaofan  12345  AntiHackerX           (GPL-3.0)
# xiaofan  12346  java -jar native-obfuscator-mod.jar  (GPL 3.0)
```

### 静态链接检查

```bash
# 确认 AntiHackerX 未链接 GPL 库
ldd bin/AntiHackerX | grep -i obfuscator
# 应该没有输出 (无静态/动态链接)

# 确认符号表不包含 GPL 代码
nm bin/AntiHackerX | grep -i obfuscator
# 应该没有输出 (无符号引用)
```

## 扩展开发

### 添加新平台支持

```cpp
enum class Platform {
    HotSpot,
    StdJava,
    Android,
    GraalVM,     // 新增
    OpenJ9       // 新增
};

QString platformToString(Platform platform) const {
    switch (platform) {
        case Platform::GraalVM:
            return "graalvm";
        case Platform::OpenJ9:
            return "openj9";
        // ...
    }
}
```

### 添加新配置选项

```cpp
struct Config {
    // 现有字段...
    
    bool enableOptimization;   // 新增: 优化开关
    int compressionLevel;      // 新增: 压缩级别
};

QStringList buildArguments(const Config &config) {
    // ...
    if (config.enableOptimization) {
        args << "--optimize";
    }
    if (config.compressionLevel > 0) {
        args << "--compression" << QString::number(config.compressionLevel);
    }
}
```

### 自定义输出解析

```cpp
void parseOutputLine(const QString &line) {
    // 现有解析...
    
    // 自定义: 提取警告
    if (line.contains("WARNING:", Qt::CaseInsensitive)) {
        emit warningDetected(line);  // 新增信号
    }
    
    // 自定义: 提取统计信息
    QRegularExpression statsRegex(R"(Processed (\d+) classes in (\d+)ms)");
    QRegularExpressionMatch match = statsRegex.match(line);
    if (match.hasMatch()) {
        emit statisticsAvailable(match.captured(1).toInt(),
                                  match.captured(2).toInt());
    }
}
```

## 调试技巧

### 启用详细日志

```cpp
ObfuscatorController::Config config;
config.debugMode = true;  // 启用 --debug 参数
controller->start(config);
```

### 捕获完整输出

```cpp
connect(controller, &ObfuscatorController::logMessage,
        this, [](const QString &msg) {
    QFile logFile("obfuscation.log");
    if (logFile.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&logFile);
        stream << QDateTime::currentDateTime().toString(Qt::ISODate)
               << " " << msg << "\n";
    }
});
```

### 检查进程状态

```cpp
if (controller->isRunning()) {
    qDebug() << "进程 PID:" << controller->process->processId();
    qDebug() << "进程状态:" << controller->process->state();
}
```

## 常见问题

### Q: 如何检测 Java 是否已安装?

```cpp
QProcess javaCheck;
javaCheck.start("java", {"-version"});
if (!javaCheck.waitForFinished() || javaCheck.exitCode() != 0) {
    QMessageBox::critical(this, "错误", 
        "未检测到 Java 运行时环境\n请安装 JRE 8 或更高版本");
}
```

### Q: 如何处理大文件超时?

```cpp
// 不设置超时,让进程自然完成
process->start("java", args);
process->waitForStarted(-1);  // 无限等待启动
// 进程完成时 finished 信号会自动触发
```

### Q: 如何支持取消操作?

```cpp
void MainWindow::onCancelClicked() {
    if (QMessageBox::question(this, "确认", "确定要取消当前操作?") 
        == QMessageBox::Yes) {
        controller->stop();  // 调用 stop() 方法
    }
}
```

## 性能考虑

- **进程开销**: 每次调用启动新进程 (~100ms)
- **内存隔离**: 主进程和 Java 进程独立内存空间
- **CPU 使用**: Java 进程会占用大量 CPU (C++ 编译阶段)
- **建议**: 对于批量处理,复用同一进程或使用队列

## 后续改进

1. **守护进程模式**: 保持 Java 进程常驻,通过 IPC 通信
2. **JSON 协议**: 更结构化的进度/日志输出
3. **增量更新**: 只处理变化的类
4. **并行处理**: 多个输入文件并发处理
5. **缓存机制**: 缓存已转换的类

---

**版本**: 1.0  
**更新时间**: 2024-01-15  
**维护者**: H3K4
