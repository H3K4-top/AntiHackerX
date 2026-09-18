# AntiHackerX 项目架构重构方案

> ⚠️ **历史文档 —— 2026-09-17 说明**
>
> 下文写于 AntiHackerX 采用 MIT 许可证的时期,当时的出发点是「避免 GPL 传染」。
> **AntiHackerX 现在自身以 GNU GPL v3.0 授权**(见 `LICENSE`),因此「分两个仓库」
> **不再是许可证要求**,而是保留下来的架构选择:主仓库不含任何 upstream 源码,
> 上游变更不会波及主项目,将来也能独立变更许可证。
> 下文的许可证相关表述已同步更正,其余作为设计记录保留。

## 背景

由于 native-obfuscator 是 GPL 3.0 许可证,为了避免许可证传染,需要将项目分为两个独立仓库:

1. **AntiHackerX** (主项目) - 以 GNU GPL v3.0 授权
2. **native-obfuscator-mod** (修改版) - 继承 GPL 3.0

## 新架构设计

```
┌─────────────────────────────────────────────────────────┐
│                    AntiHackerX                          │
│                  (主项目 - GPL-3.0)                      │
│                                                          │
│  ┌────────────┐    ┌──────────────┐   ┌────────────┐  │
│  │  Qt GUI    │───▶│ 控制器模块    │◀──│ 配置管理   │  │
│  │  (用户界面)│    │ (进程调用)    │   │ (黑白名单) │  │
│  └────────────┘    └──────┬───────┘   └────────────┘  │
│                           │                             │
│                           │ exec/spawn                  │
│                           ▼                             │
└───────────────────────────────────────────────────────┘
                            │
                            │ 进程间调用
                            │ (CLI 接口)
                            ▼
┌─────────────────────────────────────────────────────────┐
│              native-obfuscator-mod                      │
│                  (独立项目 - GPL 3.0)                    │
│                                                          │
│  ┌────────────────────────────────────────────────┐    │
│  │  Java 转译引擎 (基于 radioegor146/NOBF)       │    │
│  │  - 字节码分析                                  │    │
│  │  - C++ 代码生成                                │    │
│  │  - 自定义优化                                  │    │
│  └────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
```

## 通信协议

### AntiHackerX 调用方式

```cpp
// mainwindow.cpp
void MainWindow::onStartObfuscation() {
    // 1. 构建命令
    QStringList args;
    args << "-jar" << obfuscatorPath;
    args << inputFilePath;
    args << outputDirPath;
    args << "-b" << blacklistPath;
    args << "-p" << "hotspot";
    
    // 2. 启动进程
    QProcess *process = new QProcess(this);
    process->setProgram("java");
    process->setArguments(args);
    
    // 3. 连接信号
    connect(process, &QProcess::readyReadStandardOutput, this, &MainWindow::onProcessOutput);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MainWindow::onProcessFinished);
    
    process->start();
}
```

### 输出格式 (JSON)

native-obfuscator-mod 输出 JSON 格式供 AntiHackerX 解析:

```json
{
  "status": "processing",
  "progress": 45,
  "current_file": "com/example/BusinessLogic.class",
  "message": "Transpiling to C++..."
}
```

或使用简单的行协议:
```
PROGRESS:45
FILE:com/example/BusinessLogic.class
LOG:Transpiling to C++...
COMPLETE:success
```

## 项目目录结构

### AntiHackerX (主项目)

```
AntiHackerX/
├── src/
│   ├── main.cpp
│   ├── mainwindow.h/cpp
│   ├── obfuscator_controller.h/cpp  # 新增: 进程控制器
│   ├── config_manager.h/cpp         # 新增: 配置管理
│   └── utils/
├── resources/
│   ├── icons/
│   └── ui/
├── config/
│   ├── default_blacklist.txt
│   └── settings.json
├── docs/
├── CMakeLists.txt
├── build.sh
├── LICENSE (GNU GPL v3.0)
└── README.md
```

### native-obfuscator-mod (独立仓库)

```
native-obfuscator-mod/
├── obfuscator/
│   ├── src/
│   │   └── main/java/re146/nobf/  # 修改版源码
│   └── build.gradle
├── annotations/
├── gradle/
├── docs/
│   └── MODIFICATIONS.md            # 你的修改说明
├── LICENSE (GPL 3.0)
└── README.md
```

## 部署方式

### 方式 1: 独立二进制 (推荐)
```
用户安装目录/
├── AntiHackerX              # C++ GUI 主程序
├── lib/
│   └── native-obfuscator-mod.jar  # GPL 组件
└── config/
    ├── blacklist.txt
    └── whitelist.txt
```

### 方式 2: 在线下载
AntiHackerX 首次运行时自动下载 native-obfuscator-mod.jar

### 方式 3: 用户自行提供
用户从 GitHub Release 下载 GPL 组件,AntiHackerX 只提供路径配置

## 许可证说明

### AntiHackerX (GPL-3.0)

```
GNU General Public License v3.0
Copyright (C) 2024-2026 H3K4
SPDX-License-Identifier: GPL-3.0-only

AntiHackerX invokes native-obfuscator across a process boundary only.
Both are GPL-3.0, so the boundary is an architectural choice rather than a
licensing requirement: it keeps AntiHackerX free of upstream source, and
leaves it free to be relicensed independently later.
```

### native-obfuscator-mod (继承 GPL 3.0)

```
GNU General Public License v3.0

Based on radioegor146/native-obfuscator
Modifications by H3K4
```

## 开发工作流

### 1. Fork native-obfuscator
```bash
# 在 GitHub 上 fork radioegor146/native-obfuscator
# 创建新仓库 native-obfuscator-mod
```

### 2. 修改和构建
```bash
cd native-obfuscator-mod
# 进行你的修改
./gradlew build
# 生成 build/libs/obfuscator.jar
```

### 3. 在 AntiHackerX 中配置
```cpp
// config_manager.cpp
QString ConfigManager::getObfuscatorPath() {
    // 默认路径
    QString defaultPath = QDir::currentPath() + "/lib/native-obfuscator-mod.jar";
    
    // 从配置读取
    QSettings settings;
    return settings.value("obfuscator/path", defaultPath).toString();
}
```

## 用户文档说明

### README.md 应包含:

```markdown
## 依赖说明

AntiHackerX 需要配合 native-obfuscator-mod 使用:

1. 从 [Releases](https://github.com/yourname/native-obfuscator-mod/releases) 
   下载最新版本

2. 放置到 `lib/` 目录或在设置中指定路径

3. native-obfuscator-mod 使用 GPL 3.0 许可证,
   请确保遵守相关条款
```

## 优势

✅ **代码隔离**: AntiHackerX 不含 upstream 源码,可独立变更许可证
✅ **独立开发**: 两个项目可以独立迭代
✅ **灵活部署**: 用户可以选择性安装 GPL 组件
✅ **合规性**: 明确声明依赖关系和许可证
✅ **可替换性**: 未来可以支持其他混淆引擎

## 风险和注意事项

⚠️ **不要静态链接**: AntiHackerX 不能静态链接 GPL 代码
⚠️ **进程隔离**: 必须通过进程间调用,不能直接调用 GPL 库
⚠️ **分发说明**: README 必须清晰说明 GPL 组件的获取方式
⚠️ **版本兼容**: 需要定义清晰的 CLI 接口版本

## 实施步骤

### Phase 1: 拆分项目 (1天)
- [ ] 在 AntiHackerX 中移除 tools/native-obfuscator.jar
- [ ] 创建 native-obfuscator-mod 仓库
- [ ] Fork 并添加你的修改

### Phase 2: 实现控制器 (2天)
- [ ] 创建 ObfuscatorController 类
- [ ] 实现进程启动和通信
- [ ] 解析输出和错误处理

### Phase 3: 配置管理 (1天)
- [ ] 支持用户指定 jar 路径
- [ ] 自动检测和下载
- [ ] 配置界面

### Phase 4: 文档和测试 (1天)
- [ ] 更新 README
- [ ] 许可证声明
- [ ] 集成测试

---

下一步: 我帮你实现 ObfuscatorController 模块?
