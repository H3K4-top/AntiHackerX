# GUI 开发说明

## 当前进度

✅ **已完成**:
- 基础 Qt6 窗口框架
- 文件选择功能 (输入 JAR + 输出目录)
- 日志输出窗口
- 进度条显示
- 菜单栏 (文件/工具/帮助)
- **启动引导**:自动检测 Java、下载便携版 JDK 25、设置 JAVA_HOME
- **依赖自举**:自动从 GitHub Release 取最新版 native-obfuscator 放到 `libs/`
- **调用 native-obfuscator** (`ObfuscatorController`,子进程隔离)

⏳ **待实现**:
- 配置管理 (黑白名单编辑)
- 设置对话框 (目前「设置…」菜单项还没接任何东西)
- 混淆结果校验 / 输出目录自动打开

---

## 启动引导

程序在显示主窗口之前会先保证运行环境可用,细节见
[`RUNTIME_BOOTSTRAP.md`](./RUNTIME_BOOTSTRAP.md)。

```
<安装目录>/
├── AntiHackerX(.exe)
├── java/           便携版 JDK 25(缺 Java 时自动下载,约 190 MB)
└── libs/           native-obfuscator.jar + GPL 许可证文件
```

---

## 构建和运行

### 1. 检查 Qt6 是否安装

```bash
# Ubuntu/Debian
dpkg -l | grep qt6-base-dev

# 如果未安装
sudo apt install qt6-base-dev cmake build-essential

# 验证
qmake6 --version  # 或 qmake --version
```

### 2. 编译项目

```bash
cd /mnt/A/AntiHackerX
./build.sh
```

如果遇到 Qt6 路径问题:
```bash
# 手动指定 Qt6 路径
mkdir -p build && cd build
cmake -DCMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu/cmake/Qt6 ..
cmake --build . --config Release
```

### 3. 运行程序

```bash
./build/bin/AntiHackerX
```

---

## 界面布局

```
┌─────────────────────────────────────────────────────┐
│ 文件(F)  工具(T)  帮助(H)                            │
├─────────────────────────────────────────────────────┤
│                                                      │
│  ┌─ 文件选择 ─────────────────────────────────┐    │
│  │                                             │    │
│  │  输入文件: [________路径________] [选择]    │    │
│  │  输出目录: [________路径________] [选择]    │    │
│  │                                             │    │
│  └─────────────────────────────────────────────┘    │
│                                                      │
│              [ 开始混淆 ]                           │
│                                                      │
│  [==================进度条====================]     │
│                                                      │
│  ┌─ 日志输出 ─────────────────────────────────┐    │
│  │ [2026-09-15 17:30:00] AntiHackerX 已启动   │    │
│  │ 等待选择输入文件和输出目录...               │    │
│  │                                             │    │
│  │                                             │    │
│  │                                             │    │
│  └─────────────────────────────────────────────┘    │
│                                                      │
├─────────────────────────────────────────────────────┤
│ 就绪                                                │
└─────────────────────────────────────────────────────┘
```

---

## 代码结构

### src/main.cpp
- 程序入口
- 初始化 QApplication
- **运行引导闸门**:`RuntimeBootstrap::ensureReady()` 返回 false 就直接退出
- 把探测到的 java / JAVA_HOME / jar 路径注入主窗口(`applyRuntime`)

### src/mainwindow.{h,cpp}
- UI 布局构建、菜单栏、文件选择
- `applyRuntime()` —— 接收引导阶段的成果,回放引导日志
- `loadSettings()` —— 按 `libs/` → `lib/` → `tools/` → QSettings 的顺序找 jar
- 工具菜单里的「重新下载运行依赖」「重新安装 Java 运行环境」

### src/obfuscator_controller.{h,cpp}
- 用 `QProcess` 以**独立进程**方式调用 native-obfuscator
  (GPL-3.0 组件不产生链接,避免许可证传染)
- java 路径与 JAVA_HOME 由外部注入,不再依赖裸 `java` 命令
- 解析 stdout 里的 `n/total` 更新进度条

### src/runtime_bootstrap.{h,cpp}
- 启动期环境编排:Java 检测、便携版安装、依赖下载
- 公开 API:`ensureReady()` / `refreshNativeObfuscator()` /
  `installPortableJava()` / `refreshJavaDetection()`

### src/downloader.{h,cpp}
- 单文件 HTTP(S) 下载器:跟随重定向、流式写盘、速度/ETA、可取消

### src/download_dialog.{h,cpp}
- 带进度条的模态下载窗口

### src/archive.{h,cpp}
- 解压 zip / tar.gz(调用系统工具 + 结果校验,不依赖 Qt 私有头文件)

---

## 下一步开发

### 1. 配置管理界面

「设置…」菜单项目前是空壳,应该接一个对话框管理:
- 黑白名单编辑器
- 平台选择 (hotspot/std_java/android)
- 自定义库目录 / 自定义 java 路径

### 2. 已实现但值得改进的地方

- `ObfuscatorController` 的进度解析还很粗(只匹配 `n/total`),
  native-obfuscator 实际输出未必是这个格式。
- 混淆日志量大时 `QTextEdit::append` 会变慢,换成
  `QPlainTextEdit` + `setMaximumBlockCount` 会稳得多。
- 目前不支持代理。需要的话给 `Downloader` 加一个
  `QNetworkProxyFactory::setUseSystemConfiguration(true)`。

---

## 调试技巧

### 查看 Qt 版本和模块
```bash
qmake6 -query
pkg-config --modversion Qt6Core
```

### 常见问题

**问题 1**: `CMake Error: Could not find Qt6`
```bash
# 解决方案: 设置 CMAKE_PREFIX_PATH
export CMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu/cmake/Qt6
```

**问题 2**: 运行时找不到 Qt 库
```bash
# 解决方案: 设置 LD_LIBRARY_PATH
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

**问题 3**: 中文显示乱码
```cpp
// 在 main.cpp 添加
QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));
```

---

## 打包发布

### Linux AppImage
```bash
# 使用 linuxdeploy
linuxdeploy --executable=AntiHackerX --appdir=AppDir --output=appimage
```

### Windows
```bash
# 使用 windeployqt
windeployqt.exe AntiHackerX.exe
```

### macOS
```bash
# 使用 macdeployqt
macdeployqt AntiHackerX.app -dmg
```

---

## 参考资源

- [Qt6 文档](https://doc.qt.io/qt-6/)
- [Qt Widgets 示例](https://doc.qt.io/qt-6/qtwidgets-examples.html)
- [CMake Qt 集成](https://doc.qt.io/qt-6/cmake-manual.html)
