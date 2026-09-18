# 双仓库部署指南

## 概述

AntiHackerX 采用双仓库架构:自身以 **GNU GPL v3.0** 授权,native-obfuscator 作为独立进程调用,两者之间没有代码耦合。

## 仓库结构

```
┌─────────────────────────────────────────────┐
│  Repository 1: AntiHackerX (主项目)        │
│  - URL: github.com/<you>/AntiHackerX       │
│  - 许可证: GNU GPL v3.0                     │
│  - 语言: C++ (Qt6)                          │
│  - 内容: GUI + 构建系统 + 文档              │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│  Repository 2: native-obfuscator-mod       │
│  - URL: github.com/<you>/native-obfuscator │
│  - 许可证: GPL 3.0                          │
│  - 语言: Java (Gradle)                      │
│  - 内容: .class → .cpp 转换器              │
└─────────────────────────────────────────────┘
```

## 当前状态

### 本地文件系统

```
/mnt/A/AntiHackerX/
├── .git/                    # 主项目 Git 仓库
├── src/                     # C++ 源代码 (GPL-3.0)
├── docs/                    # 文档
├── build/                   # 构建输出
├── native-obfuscator/       # GPL 子项目 (独立 .git)
│   ├── .git/                # ← 独立的 Git 仓库
│   ├── MODIFICATIONS.md     # ← 修改说明
│   ├── obfuscator/          # Java 源代码 (GPL 3.0)
│   └── build.gradle
└── .gitignore               # ← 已排除 native-obfuscator/
```

### Git 状态

- ✅ AntiHackerX 主项目: 本地 Git 仓库 (未推送)
- ✅ native-obfuscator: 已从上游克隆 (独立 .git)
- ✅ .gitignore: 已配置排除 `native-obfuscator/`

## 部署步骤

### 第一步: 创建 GitHub 仓库

#### 1.1 创建主仓库 (AntiHackerX)

```bash
# 在 GitHub 上创建仓库
# 仓库名: AntiHackerX
# 许可证: GNU GPL v3.0
# 描述: Java Anti-Reverse-Engineering Tool with Native Obfuscation

# 本地关联远程仓库
cd /mnt/A/AntiHackerX
git remote add origin git@github.com:<your-username>/AntiHackerX.git

# 推送代码
git add .
git commit -m "Initial commit: Qt6 GUI + ObfuscatorController"
git branch -M main
git push -u origin main
```

#### 1.2 创建子仓库 (native-obfuscator-mod)

```bash
# 在 GitHub 上创建仓库
# 仓库名: native-obfuscator-mod 或 native-obfuscator
# 许可证: GPL-3.0 License
# 描述: Fork of radioegor146/native-obfuscator with improvements

# 本地关联新的远程仓库
cd /mnt/A/AntiHackerX/native-obfuscator
git remote rename origin upstream  # 保留上游链接
git remote add origin git@github.com:<your-username>/native-obfuscator.git

# 推送代码
git add MODIFICATIONS.md
git commit -m "Add: MODIFICATIONS.md - Fork documentation"
git push -u origin master  # 注意: 上游使用 master 分支
```

### 第二步: 配置 Git 子模块 (可选)

如果希望方便管理两个仓库的版本关系,可以使用 Git Submodule:

```bash
cd /mnt/A/AntiHackerX

# 删除现有目录
rm -rf native-obfuscator

# 添加为子模块
git submodule add git@github.com:<your-username>/native-obfuscator.git native-obfuscator

# 提交子模块配置
git add .gitmodules native-obfuscator
git commit -m "Add: native-obfuscator as Git submodule"
git push
```

**注意**: 子模块方式的优缺点:
- ✅ 优点: 版本锁定,克隆时可选 `--recurse-submodules`
- ❌ 缺点: 增加复杂度,用户需要理解子模块概念

**推荐**: 初期使用独立仓库,稳定后再考虑子模块。

### 第三步: 更新文档引用

#### 3.1 更新主项目 README

在 `/mnt/A/AntiHackerX/README.md` 中添加:

```markdown
## 依赖项目

本项目依赖以下独立组件:

- **native-obfuscator-mod**: Java .class → C++ 转换器 (GPL 3.0)
  - 仓库: https://github.com/<your-username>/native-obfuscator
  - 许可证: GPL 3.0
  - 使用方式: 进程隔离调用,无许可证传染

## 构建说明

### 安装依赖

1. 下载预编译的 native-obfuscator-mod.jar:
   ```bash
   wget https://github.com/<your-username>/native-obfuscator/releases/latest/download/native-obfuscator.jar \
        -O lib/native-obfuscator-mod.jar
   ```

2. 或从源码构建:
   ```bash
   git clone https://github.com/<your-username>/native-obfuscator.git
   cd native-obfuscator
   ./gradlew build
   cp obfuscator/build/libs/native-obfuscator-*.jar ../lib/native-obfuscator-mod.jar
   ```
```

#### 3.2 更新 ARCHITECTURE_REFACTOR.md

添加远程仓库 URL:

```markdown
## 仓库地址

- 主项目: https://github.com/<your-username>/AntiHackerX
- GPL 组件: https://github.com/<your-username>/native-obfuscator
```

### 第四步: 发布 Release

#### 4.1 构建 native-obfuscator JAR

```bash
cd /mnt/A/AntiHackerX/native-obfuscator
./gradlew clean build

# 查找输出文件
find . -name "native-obfuscator-*.jar" -type f
# 输出: ./obfuscator/build/libs/native-obfuscator-3.5.4r.jar
```

#### 4.2 创建 GitHub Release

在 native-obfuscator 仓库:

1. 点击 "Releases" → "Create a new release"
2. Tag: `v3.5.4r-mod1`
3. Title: `v3.5.4r-mod1 - Initial Fork`
4. 描述:
   ```markdown
   ## 初始 Fork 版本
   
   基于 radioegor146/native-obfuscator v3.5.4r
   
   ### 变更
   - 添加 MODIFICATIONS.md 文档
   - 无代码修改,与上游保持一致
   
   ### 下载
   - native-obfuscator-3.5.4r.jar (附件)
   
   ### 使用方式
   参见 [MODIFICATIONS.md](MODIFICATIONS.md)
   ```
5. 上传 JAR 文件作为附件
6. 发布

### 第五步: 更新 AntiHackerX 构建脚本

修改 `build.sh` 自动下载 JAR:

```bash
#!/bin/bash
set -e

echo "===== AntiHackerX 构建脚本 ====="

# 检查依赖
if ! command -v java &> /dev/null; then
    echo "错误: 未安装 Java"
    exit 1
fi

if ! command -v cmake &> /dev/null; then
    echo "错误: 未安装 CMake"
    exit 1
fi

# 下载 native-obfuscator-mod (如果不存在)
if [ ! -f "lib/native-obfuscator-mod.jar" ]; then
    echo "正在下载 native-obfuscator-mod.jar..."
    mkdir -p lib
    wget https://github.com/<your-username>/native-obfuscator/releases/latest/download/native-obfuscator-3.5.4r.jar \
         -O lib/native-obfuscator-mod.jar
fi

# 构建主项目
echo "正在配置项目..."
mkdir -p build
cd build
cmake ..

echo "正在编译..."
make -j$(nproc)

echo "===== 构建完成 ====="
echo "可执行文件: build/bin/AntiHackerX"
```

## 开发工作流

### 日常开发 (主项目)

```bash
cd /mnt/A/AntiHackerX

# 修改 C++ 代码
vim src/mainwindow.cpp

# 构建测试
./build.sh
./build/bin/AntiHackerX

# 提交
git add .
git commit -m "Fix: UI bug in main window"
git push
```

### 修改 GPL 组件

```bash
cd /mnt/A/AntiHackerX/native-obfuscator

# 修改 Java 代码
vim obfuscator/src/main/java/by/radioegor146/Obfuscator.java

# 更新文档
vim MODIFICATIONS.md

# 构建测试
./gradlew build

# 提交到子仓库
git add .
git commit -m "Add: JSON progress output"
git push

# 创建新 Release
# (GitHub Web 界面)
```

### 同步上游更新

```bash
cd /mnt/A/AntiHackerX/native-obfuscator

# 拉取上游更新
git fetch upstream
git merge upstream/master

# 解决冲突 (如果有)
# ...

# 推送到你的 fork
git push origin master
```

## 用户安装指南

### 方案 1: 从 Release 下载 (推荐)

```bash
# 1. 克隆主项目
git clone https://github.com/<your-username>/AntiHackerX.git
cd AntiHackerX

# 2. 运行构建脚本 (会自动下载 JAR)
./build.sh

# 3. 运行
./build/bin/AntiHackerX
```

### 方案 2: 从源码完整构建

```bash
# 1. 克隆主项目
git clone https://github.com/<your-username>/AntiHackerX.git
cd AntiHackerX

# 2. 克隆 GPL 组件
git clone https://github.com/<your-username>/native-obfuscator.git
cd native-obfuscator
./gradlew build
cp obfuscator/build/libs/native-obfuscator-*.jar ../lib/native-obfuscator-mod.jar
cd ..

# 3. 构建主项目
./build.sh

# 4. 运行
./build/bin/AntiHackerX
```

### 方案 3: 使用 Git Submodule (如果配置了)

```bash
# 克隆时递归获取子模块
git clone --recurse-submodules https://github.com/<your-username>/AntiHackerX.git
cd AntiHackerX

# 构建 GPL 组件
cd native-obfuscator
./gradlew build
cp obfuscator/build/libs/native-obfuscator-*.jar ../lib/native-obfuscator-mod.jar
cd ..

# 构建主项目
./build.sh
```

## 许可证合规检查

### 自动化检查脚本

创建 `check-license-isolation.sh`:

```bash
#!/bin/bash
# 检查许可证隔离是否正确

echo "===== 许可证隔离检查 ====="

# 1. 检查主项目二进制不包含 GPL 符号
echo "检查 1: 二进制符号表..."
if nm build/bin/AntiHackerX | grep -i obfuscator; then
    echo "❌ 失败: 发现 obfuscator 相关符号"
    exit 1
else
    echo "✅ 通过: 无 GPL 符号引用"
fi

# 2. 检查动态链接库
echo "检查 2: 动态链接..."
if ldd build/bin/AntiHackerX | grep -i obfuscator; then
    echo "❌ 失败: 发现 obfuscator 动态链接"
    exit 1
else
    echo "✅ 通过: 无 GPL 库链接"
fi

# 3. 检查进程调用
echo "检查 3: 进程隔离..."
if grep -r "QProcess" src/obfuscator_controller.cpp > /dev/null; then
    echo "✅ 通过: 使用进程隔离调用"
else
    echo "⚠️  警告: 未找到 QProcess 调用"
fi

echo "===== 检查完成 ====="
```

## 常见问题

### Q: 为什么要分成两个仓库?

A: 这是历史原因 —— 当年 AntiHackerX 用 MIT、GPL 3.0 有 "传染性",靠进程隔离才能保住 MIT。不过 **AntiHackerX 自身现在也是 GPL-3.0**,所以分仓库**不再是许可证要求**。保留它的理由是:主仓库不含任何 upstream 源码,上游变更不会波及主项目,将来也能独立变更许可证。

### Q: 用户需要两次克隆吗?

A: 不需要。构建脚本会自动从 Release 下载预编译的 JAR 文件。只有开发者修改 GPL 组件时才需要克隆第二个仓库。

### Q: 如何更新 native-obfuscator?

A: 主项目用户只需下载新的 JAR 文件。开发者需要在子仓库提交修改并创建新 Release。

### Q: 可以用 Git Submodule 吗?

A: 可以,但会增加复杂度。推荐独立仓库 + Release 分发的方式。

### Q: 如何验证许可证隔离?

A: 运行 `check-license-isolation.sh` 脚本,检查二进制文件不包含 GPL 代码的符号。

## 后续维护

### 定期任务

- [ ] 每月同步上游 native-obfuscator 更新
- [ ] 每次修改后创建新 Release
- [ ] 更新主项目的依赖版本号
- [ ] 检查许可证合规性

### 版本管理

建议使用语义化版本:

- native-obfuscator-mod: `v3.5.4r-mod1`, `v3.5.4r-mod2`, ...
- AntiHackerX: `v1.0.0`, `v1.1.0`, ...

## 总结

✅ **已完成**:
- native-obfuscator 仓库已克隆到本地
- MODIFICATIONS.md 文档已创建
- .gitignore 已配置排除子目录

📋 **下一步**:
1. 在 GitHub 创建两个仓库
2. 推送代码到远程
3. 构建并发布 native-obfuscator JAR
4. 更新主项目文档引用

---

**文档版本**: 1.0  
**更新日期**: 2024-01-15  
**维护者**: H3K4
