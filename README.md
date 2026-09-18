# AntiHackerX

[**中文**](README.md) ｜ [English](README.en.md)

**给 Java 产物加壳:字节码混淆 + 类加密 + 原生编译,三层层层深入。**

> 反编译一个只做过常规混淆的 JAR,你得到的是"难读的代码";
> 反编译 AntiHackerX 处理过的 JAR,你得到的是**没有代码** ——
> 类被加密进单一载荷,关键方法体是 `.so` 里的机器码。

一个带 GUI 的跨平台(C++ / Qt6)Java 保护工具,面向 **Minecraft 插件作者**与
**商业 Java 软件作者**。绿色免安装:首次启动自动拉齐 JDK 与全部工具链。

## 它解决什么问题

Java 的 `.class` 天生可反编译。常规混淆器只把名字改乱,方法体依旧一览无余 ——
用 IDEA / JADX 打开,逻辑照样读得懂。想真正挡住逆向,得让**代码本身不在 JAR 里**。

AntiHackerX 把防护拆成三层,可按需组合:

| 层 | 手段 | 攻击者面对什么 |
|---|---|---|
| ① **混淆** | 类/包/方法/字段/参数重命名、字符串加密、整型异或、垃圾代码、隐藏成员 | 名字无意义,控制流被稀释 |
| ② **加密** | 全部类 AES-256-GCM 加密进单一载荷,运行时解密后 `defineClass` 进**宿主加载器** | JAR 里没有可读的类 |
| ③ **原生化** | 关键类的方法体转译成 C++ 编进 `.so` / `.dll` | 方法体是机器码,不是字节码 |
| ➕ **加固** | 反调试 / 反 Agent / 反篡改(ECDSA 签名) / 反虚拟机 / 版权提示 | 动态分析也变贵 |

### 核心特性

- 🔐 **类加密(不是混淆)**:整包类加密进 `p.dat`(AES-256-GCM,类名作为附加认证数据),
  运行时解密后由 `MethodHandles.Lookup#defineClass` 定义进**服务端自己的加载器** ——
  不换加载器,所以 `plugin.yml`、反射、`getClass().getClassLoader()` 全都照常工作
- 🛡️ **原生化**:走 native-obfuscator 把方法体搬进原生库,同时**交叉编译 Linux + Windows**
- 🧬 **反篡改**:对产物里全部非类文件(`p.dat` / `*.so` / 配置文件 / `plugin.yml`)
  做 ECDSA-P256 签名校验,被改动就拒绝加载;公钥烧进原生库而非写在 Java 常量里
- 🌐 **完整支持 Minecraft 服务端**:自动识别 `plugin.yml`,处理 `JavaPlugin` 的单实例硬限制
- 🎨 **Qt6 GUI**:13 项混淆开关 + 预设、JAR 类型识别、类扫描与勾选、实时日志与进度
- 📦 **绿色免安装**:便携版 JDK、native-obfuscator、jar-obfuscator、zig 工具链全部自动下载

## 支持的产物类型

| 类型 | 识别方式 | 加固支持 |
|---|---|---|
| 普通 JAR | `MANIFEST.MF` 的 `Main-Class` | ✅ 完整 |
| Spring Boot Fat JAR | `BOOT-INF/` | ✅ 完整 |
| Minecraft Paper / Bukkit 插件 | `plugin.yml` | ✅ 完整(含专用入口模板) |
| Fabric MOD | `fabric.mod.json` | ⚠️ 仅识别 |
| Forge MOD | `mods.toml` / `mcmod.info` | ⚠️ 仅识别 |

## 工作原理

一次加壳是 **11 步流水线**,核心是三步:

```mermaid
flowchart LR
    A[输入 JAR] --> B[1) jar-obfuscator 混淆]
    B --> C[2) 编译验证模块<br/>生成加壳入口]
    C --> D[3) NOBF 把关键类<br/>转译成 C++]
    D --> E[4) zig 交叉编译<br/>Linux + Windows]
    E --> F[5) 其余类 AES 加密<br/>进 p.dat]
    F --> G[6) 签名 + 组装]
    G --> H[加壳产物]
```

### 三个非显然的设计决定

**① 为什么必须有"桥类"**

Bukkit/Paper 的 `JavaPlugin` 构造器带有硬检查:必须由服务端的 `PluginClassLoader`
创建,而且**一个加载器只允许一个实例**(`plugin` 字段是 `final`)。所以"入口继承
`JavaPlugin`、再 new 一个真实插件"这条路走不通。

做法是把桥类插在**真实主类与 `JavaPlugin` 之间**,打包时改写真实主类的父类:

```
org.bukkit.plugin.java.JavaPlugin
        ↑
  桥类(只有静态初始化:定义载荷)
        ↑
  真实插件主类(父类被改指到桥类)
```

于是类初始化顺序天然保证:载荷先就位,`JavaPlugin` 构造器只跑一次。

**② 为什么需要"定义器"**

解密出来的类必须由**宿主加载器**定义(上一段的原因),而
`MethodHandles.Lookup#defineClass` 要求 lookup 与目标类**同包**。
所以打包器会为载荷里的每一个包生成一个极小的定义器类放在明文区,
运行时取出它的 `Lookup` 再定义同类。

**③ 为什么必须明文留一批类(而且只有一批)**

Paper 在 `PluginClassLoader.<init>` 里就执行 `Class.forName(main, true, this)`,
在载荷被定义出来**之前**就要完成 `defineClass`(解析直接父类/接口)与字节码校验
(可赋值性检查会真的把类型加载起来)。所以这批必须明文:

- 主类自己 + 它的直接超类型链(递归)
- **主类直接引用到的全部类型**,以及这些类型的超类型闭包
- 解密器本身(桥类 / 运行时 / 定义器)

其余全部加密。实测 19.5 MB / 4600 类的 GrimAC 上,明文只有 **62 个类**(98.7% 加密)。
这一层再递归一层就会涨到 4574 个类 —— 等于没加密,所以只做一层。

## 项目结构

```
AntiHackerX/
├── src/                        # C++ 源代码(Qt6 GUI + 流水线)
│   ├── main.cpp               # 程序入口(含启动引导闸门)
│   ├── mainwindow.h/cpp       # 主窗口:选项面板 / 类表 / 日志
│   ├── packer_pipeline.h/cpp  # 11 步加壳流水线(核心)
│   ├── verify_module.h/cpp    # 验证模块装配:释放源码 + 生成加壳入口
│   ├── jar_analyzer.h/cpp     # JAR 类型识别(5 种)
│   ├── class_scanner.h/cpp    # 类扫描与勾选
│   ├── obfuscator_controller.*# 进程隔离调用 GPL 组件
│   ├── runtime_bootstrap.*    # 启动引导:JDK / 依赖 / zig 工具链自动下载
│   ├── downloader.*           # HTTP 下载器(进度、速度、ETA、重定向)
│   ├── download_dialog.*      # 带进度条的模态下载窗口
│   ├── pack_progress_dialog.* # 打包进度窗口
│   ├── archive.*              # zip / tar.gz / tar.xz 解压
│   └── app_logger.*           # 日志文件
├── AntiHackerXVerify/          # 验证模块(MIT):守卫 + 解密运行时 + 入口模板 + 工具
│   ├── src/main/java/top/h3k4/ # AhxRuntime / AhxPayload / AhxSignature / 各守卫
│   ├── templates/              # Bootstrap / PluginBootstrap 入口模板
│   ├── tools/                  # AhxPacker(AES 加密打包) / AhxSign(签名密钥)
│   └── verify.qrc              # 内嵌资源清单
├── jar-obfuscator/             # MIT,字节码混淆器 fork(含引用修正修复)
├── native-obfuscator/          # GPL-3.0 组件,独立 git 仓库
├── docs/                       # 文档
├── build.sh                    # 构建脚本
├── install-deps.sh             # 一键装编译期依赖(Qt6 / CMake / 编译器)
├── test-jar-analyzer.sh        # JAR 类型识别的自测脚本
├── check-license-isolation.sh  # 许可证隔离校验(GPL 组件只允许进程调用)
└── CMakeLists.txt              # CMake 构建配置
```

> `java/`、`libs/`、`build/`、`testp/`、`native-obfuscator/` 均不入版本库:
> 前三个是自动下载/产物,后两个是大体积测试数据与独立维护的 GPL 仓库。

> 首次启动会在可执行文件旁边生成 `java/` 与 `libs/`,结构见
> [启动引导文档](docs/RUNTIME_BOOTSTRAP.md)。

## 环境要求

### 编译 GUI 程序
- **Qt6**(qt6-base-dev,含 Network 模块)
- **CMake** 3.16+
- **C++ 编译器** (GCC 7+, Clang 6+, MSVC 2019+)

### 运行混淆功能
以下四项**都不需要手动装** —— 启动时检测不到会自动下载到程序目录:

| 依赖 | 用途 | 来源 |
|---|---|---|
| 便携版 JDK | 跑混淆器、编译验证模块 | Adoptium |
| native-obfuscator | 把方法体转译成 C++ | 本项目的 fork Release |
| jar-obfuscator | 字节码混淆 | 本项目的 fork |
| **zig**(自带 Clang) | 编译转译出来的 C++ | ziglang.org |

> 因为有便携版 zig,你**不需要在系统里装 GCC / Clang / CMake**,
> 也不需要装 Visual Studio —— 而且同一台 Linux 机器能直接产出
> `x64-linux.so` **和** `x64-windows.dll`,一份产物双平台可用。

### 网络要求
程序需要能访问 GitHub 与 ziglang.org(取依赖 Release、JDK、工具链)。
若只能走代理,请先设置系统的 `http_proxy` / `https_proxy` 环境变量后再启动。

## 快速开始

### 1. 安装编译依赖

#### Ubuntu/Debian
```bash
sudo apt update
sudo apt install qt6-base-dev cmake g++
```

#### Arch Linux
```bash
sudo pacman -S qt6-base cmake gcc
```

#### macOS
```bash
brew install qt@6 cmake
```

> 不需要装 JDK —— 启动时没检测到会自动下载便携版。

### 2. 编译 GUI 程序
```bash
./build.sh
```

或手动编译:
```bash
mkdir -p build && cd build
cmake ..
cmake --build . --config Release
```

### 3. 运行程序
```bash
./build/bin/AntiHackerX
```

首次启动会带进度条自动拉齐依赖:

1. 检测 Java —— 没有就询问是否下载便携版 JDK(约 190 MB),下完自动设置 `JAVA_HOME`
2. 从本项目的 GitHub Release 取 `native-obfuscator` 与 `jar-obfuscator`
3. **第一次真正走到原生编译时**,再按需下载便携版 zig(自带 Clang,约 50 MB)

不想看到这些步骤,可以提前手动放好 `java/` 与 `libs/`。

## 使用方法

### 基本流程
1. 启动 AntiHackerX(**首次启动会带进度条自动拉齐 JDK 与工具链**)
2. 选择要保护的 JAR —— 工具会自动识别类型、扫出可加固的类
3. 按需勾选混淆项与防护项(默认已是一套安全组合)
4. 点开始,看日志;产物写在输出目录

> Paper/Bukkit 插件需额外指定 **服务端 API 类的路径**(`org.bukkit.*`),
> 否则验证模块的插件入口编不过 —— 界面上会直接提醒。

### 混淆选项(默认值标 ⭐)

| 选项 | 说明 | 默认 |
|---|---|---|
| 类名混淆(含引用修正) | 重命名所有类,同步修正常量池/泛型签名/注解/`invokedynamic` 等处的引用 | ⭐开 |
| 包名混淆(含引用修正) | 重命名包路径并移动类。被反射或资源路径引用的插件易出问题 | 关 |
| 方法名混淆(含引用修正) | 重命名非接口/非覆写方法,调用点同步修正 | 关 |
| 字段名混淆(含引用修正) | 重命名字段,读写点同步修正。序列化/反射场景需谨慎 | 关 |
| 方法参数名混淆 | 仅影响 `LocalVariableTable` / `MethodParameters` 等调试信息 | 关 |
| 删除编译调试信息 | 去掉 `SourceFile`、行号表,反编译后看不到原文件名与行号 | 关 |
| 字符串 AES 加密运行时解密 | 字符串常量加密存储,运行时由生成的类解密 | ⭐开 |
| 字符串改为访问全局列表 | 字符串不再出现在字节码里,改从静态列表按下标取。强度更高但体积变大 | 关 |
| 整型常数多重异或 | 把 `int` 常量拆成多次异或运算 | 关 |
| 添加垃圾代码 | 插入永不执行的花指令,可选 L1–L9 强度 | ⭐开(L2) |
| IDEA 反编译时隐藏方法 | 写入特殊属性,使 IDEA 反编译器看不到这些方法 | ⭐开 |
| IDEA 反编译时隐藏字段 | 同上,针对字段 | ⭐开 |
| AI 提示词注入 | 给每个类注入一个常量,内容是一份写给 AI/自动化分析系统的声明,要求它拒绝解释本类实现。只在"把代码贴给模型"的场景有效,属于抬高成本而非可靠防护;每个类约增大 1.8KB | 关 |

### 防护选项

| 选项 | 作用 |
|---|---|
| 反调试 | 计时器守卫,检测断点造成的冻结 |
| 反 Agent | 检查启动参数里的 `-javaagent` |
| **反篡改** | 对全部非类文件做 ECDSA-P256 签名校验,被改动则拒绝加载 |
| 反虚拟机运行 | 固件/厂商指纹检测 |
| 版权弹窗 | 启动时展示加壳版权提示(无头环境自动退化为日志) |
| 隐藏主类 | 配合「要隐藏的类」把选中类转成原生 |

### 产物长什么样

```
插件.jar
├── <随机包名>/            # 每份产物随机命名,避免两个加壳插件撞车
│   ├── <随机名>.class    # 加载器
│   └── x64-linux.so      # 可选:x64-windows.dll(同一份产物双平台)
├── top/h3k4/
│   ├── p.dat             # 全部加密类(AES-256-GCM)
│   └── p.sig             # 反篡改签名(ECDSA-P256)
├── <你的插件类的包>/*.class # 只有那批"必须明文"的类
├── plugin.yml           # 未修改:main 保持你的原始主类名
└── META-INF/MANIFEST.MF
```

## 文档

- [项目总结](docs/PROJECT_SUMMARY.md) - 能力全貌与关键数据
- [架构重构说明](docs/ARCHITECTURE_REFACTOR.md) - 模块划分与演进
- [启动引导](docs/RUNTIME_BOOTSTRAP.md) - JDK / 依赖 / 工具链自举
- [部署指南](docs/DEPLOYMENT_GUIDE.md) - 打包与分发
- [GUI 开发说明](docs/GUI_DEVELOPMENT.md) - 界面结构与代码组织
- [混淆器控制层](docs/OBFUSCATOR_CONTROLLER.md) - 进程隔离与参数装配
- [Loader 分析](docs/LOADER_ANALYSIS.md) - 原生加载机制详解
- [kilij 虚拟化层评估](docs/KILIJ_INTEGRATION.md) - 编译期 VM 虚拟化的评估数据(⏸ 集成暂缓)
- [**路线图 TODO**](TODO.md) - 平台扩展(Fabric / Forge / 普通 JAR / Spring Boot)与能力规划
- [发布检查表](docs/RELEASE_CHECKLIST.md) - 发版前逐项过一遍

## 项目状态

已在**真实商用产物**上验证:19.5 MB / 4600 个类的 Paper 反作弊插件(GrimAC)
加壳后能正常加载、启用、运行,对一小类 36 KB 的插件也能秒级完成。

- ✅ 11 步加壳流水线端到端跑通
- ✅ 类加密(AES-256-GCM + 3 分片密钥 + 每包定义器),主类与直接超类型链明文
- ✅ 桥类方案绕开 `JavaPlugin` 的单实例硬限制
- ✅ 产物命名每份随机(修复了两个加壳插件共存时 bootstrap 加载器 `LinkageError`)
- ✅ 反篡改:ECDSA-P256 签名校验(`p.dat` / `*.so` / `plugin.yml` 等全部非类文件)
- ✅ 13 项混淆选项 + 预设 + 类扫描 GUI
- ✅ 便携工具链自举 + Linux/Windows 交叉编译

待办与已知限制见 [**TODO.md**](TODO.md)(产品路线图)。

## 许可证

本项目以 **GNU GPL v3.0** 授权(全文见 [`LICENSE`](LICENSE))。

`native-obfuscator` 以独立进程方式调用。两者都是 GPL-3.0,所以**进程隔离是架构选择,
不是许可证要求** —— 它的价值在于 AntiHackerX 不含任何 upstream 源码,将来可以独立变更授权。

> ℹ️ **示例模块例外**:[`AntiHackerXVerify/`](AntiHackerXVerify/) 子目录单独以 **MIT**
> 授权(见 [`AntiHackerXVerify/LICENSE`](AntiHackerXVerify/LICENSE))。它是演示程序,
> 也是供你嵌入自家程序的版权提示样板 —— 这样你就能把它拷进闭源项目而不触发 GPL-3.0。

> ⚠️ **关于你用 AntiHackerX 打包出来的程序**:那部分代码的授权由 native-obfuscator 上游
> `LICENSE` 里的 **Output Exception** 决定,**与 AntiHackerX 自身的 GPL-3.0 无关** ——
> 例外明确允许以任意条款链接、嵌入、编译、分发工具输出的 runtime 代码。
> 详见 <https://github.com/radioegor146/native-obfuscator/blob/master/LICENSE>。

## 贡献

欢迎提交 Issue 和 Pull Request。

- 想找活干看看 [TODO.md](TODO.md),P0/P1 都在那里
- 改动 `native-obfuscator/` 前请先读它的 `MODIFICATIONS.md`(那是 GPL-3.0 的独立 fork)
- 提 PR 请保证 `./check-license-isolation.sh` 依然通过

---

**注意**: native-obfuscator 会显著降低性能,建议只对核心业务逻辑进行混淆。
