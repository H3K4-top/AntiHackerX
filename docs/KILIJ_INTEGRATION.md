# kilij 虚拟化层 — 评估报告与集成设计

> **状态**: ✅ 评估完成 | ⏸ 集成**暂缓**(按计划,待产品缺口补齐后再做)
> **评估日期**: 2026-09-17
> **文档目的**: 固化评估数据与集成约束,避免后续重复踩坑

---

## 📋 一、这是什么

[`dannyisbad/kilij`](https://github.com/dannyisbad/kilij) 是一个基于 LLVM 20 新 Pass Manager 的
**编译期代码混淆引擎**,以 `-fpass-plugin` 形式以插件方式挂载。

| 项目 | 值 |
|---|---|
| 许可证 | **Obfuscator-LLVM Release License (UIUC/NCSA)** — 宽松,**允许商用** |
| 义务 | 保留版权声明;不得声称获得 OLLVM / HEIG-VD 背书 |
| 规模 | 24,076 行 C++ |
| 版本 | v0.1.0,仓库创建于 2026-02-12,**历史仅 1 天** |
| 作者自述 | Windows x86_64 完全支持;**Linux "Builds, untested, behavior not guaranteed"** |
| 发布资产 | 仅有 125 MB 的 Windows zip |

### 它提供的能力

- **真 VM 虚拟化**:17 opcode / 8 类型种类 / 3 执行模式(opcode·bb·region)/
  3 层编码(affine + MBA + Feistel)/ 加密 PC / 间接分发 / 虚假 handler
- **12 个经典 IR pass**:`-fla -bcf -split -indbr -mba -sub -obf-const -obf-str
  -indcall -obf-iat -obf-hide-externs`
- **7 族不透明谓词**:collatz / composite / diffsq / minorquad / powmod / qseries / xoreq
- 实测**注册 109 个选项**(用 `opt --load-pass-plugin` 与裸 `opt` 的 `--help-hidden` 做差集得出)

---

## ✅ 二、结论速览

| 维度 | 结论 |
|---|---|
| Linux 可用性 | ✅ **可用**,但需 5 处构建修补 + 共享版 LLVM |
| 正确性 | ✅ 两轮端到端测试输出与未混淆版**完全一致** |
| 保护强度 | ✅ 真 VM 虚拟化,非"改改指令" |
| **性能** | ✅ **不是障碍** — 只保护验证函数时单次开销 0.7~6.8 ms |
| 体积 | ✅ 只膨胀被保护的那一块(~120 KB 级) |
| 许可证 | ✅ NCSA,可商用 |
| **主要限制** | ⚠️ **含"可能抛异常的调用"的函数无法虚拟化**(见第七节) |
| 主要成本 | 构建机需共享版 LLVM 20;生成的 CMake 需改为三段式 |

### 🎯 关键定位(为什么性能不是问题)

本项目的架构是:

```
用户 JAR → 验证模块/无验证模块 打包 JAR → 反编译为 C++ → 编译期混淆 + kilij 虚拟化
        → 加密用户 JAR + 用户选择处理类转 C++ → 编译加壳产出 JAR
```

**被保护的是"许可验证 / 解密函数",它在启动时只跑一次**。验证通过、解密完成后,
用户代码走原有执行路径,**运行速度不受影响**。

> 因此早期"整体慢 4.3×"的测量**测错了对象**,不作为决策依据。
> 真正有意义的指标是**单次验证耗时**,见第六节。

---

## 🔧 三、Linux 构建:5 处必需修补

在 `<kilij>/CMakeLists.txt` 及相关处:

| # | 现象 | 原因 | 修补 |
|---|---|---|---|
| 1 | `find_package(LLVM 20 CONFIG)` 被拒 | LLVM 的 `LLVMConfigVersion.cmake` 用 `"${MAJOR}.${MINOR}"` 比较,只给主版本会得到 `"20."` | 改为 `find_package(LLVM 20.1 CONFIG)` |
| 2 | `check_compiler_flag: C: needs to be enabled before use` | `HandleLLVMOptions.cmake` 需要启用 C 语言 | `project(... LANGUAGES C CXX)` |
| 3 | `links to target "zstd::libzstd_static" but the target was not found` | `LLVMConfig.cmake` 内部的 `find_package(zstd)` 静默失败,系统只有 `libzstd.so.1`,无头文件/CMake 配置 | 手工加 imported target shim |
| 4 | `libLLVMCore.a: error adding symbols: file format not recognized` | 官方归档是 **LTO bitcode**(魔数 `BC C0 DE`),GNU ld 读不了 | 见第四、五节:改用共享版 LLVM |
| 5 | `unable to find library -lstdc++` | clang 自动选中 GCC 12(目录存在但**缺 `libstdc++.so`**) | `--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/11` |

修补 #1~#3 与 #5 属通用问题,**建议上游反馈**。

---

## 🧱 四、工具链要求:必须用**共享版** LLVM(核心约束)

### 为什么不能用官方 tarball

| 检查项 | 官方 `LLVM-20.1.0-Linux-X64.tar.xz` |
|---|---|
| `libLLVM*.so*` | **0 个** |
| `libLLVM*.a` | 212 个,且为 **LTO bitcode** |
| `opt` 二进制 | 180 MB,**静态链接**,LLVM 内嵌其中 |

Pass plugin 的本质要求是**宿主与插件共享同一份 LLVM 实例** —— 插件里的 `cl::opt`
要注册进宿主的选项注册表,`-fla` 之类的参数才能被识别。静态包结构上做不到这一点。

**实测症状**(务必记住这个判据):

```
error: unable to load plugin 'Kilij.so': undefined symbol:
       _ZTVN4llvm2cl11OptionValueINSt7__cxx1112basic_stringI...EEE
```

共 13 个未定义 LLVM 符号(`cl::opt<std::string>` 的 3 个 vtable、`Twine::str`、
`StringRef::lower`、`Intrinsic::getName`、`GlobalVariable` 构造、`raw_fd_ostream` 构造、
`IRBuilderBase::CreateConstrainedFP*`、`DemotePHIToStack`、`DemoteRegToStack`)。

> ⚠️ **教训**:`-shared` 链接**默认允许未定义符号**,所以"生成了 .so"**不等于**"插件可用"。
> 合格判据只有一条 —— `opt --load-pass-plugin=X --help` 能列出插件的选项。

### 免 root 部署共享版 LLVM(已验证可行)

`apt.llvm.org` 的 Debian 包提供真正的 `libLLVM-20.so`,**无需 root**,下载量仅 **114 MB**:

```bash
# 1. 取包索引,解析出所需包
curl -s -x http://127.0.0.1:7890 \
  -o Packages https://apt.llvm.org/jammy/dists/llvm-toolchain-jammy-20/main/binary-amd64/Packages

# 2. 需要的 8 个包(共 ~114 MB):libllvm20, llvm-20-dev, llvm-20, clang-20,
#    libclang-cpp20, libclang-common-20-dev, llvm-20-runtime, llvm-20-linker-tools
# 3. 下载后用 dpkg-deb -x 解到任意目录(无需 root)
dpkg-deb -x libllvm20_*.deb /opt/llvm20
# 4. 所有 .deb 都解到同一前缀,得到:
#    /opt/llvm20/usr/lib/x86_64-linux-gnu/libLLVM-20.so          ← 共享库 ✅
#    /opt/llvm20/usr/lib/llvm-20/lib/cmake/llvm/LLVMConfig.cmake  ← CMake 配置 ✅
#    /opt/llvm20/usr/lib/llvm-20/bin/{clang,clang++,opt,llvm-link,...}
```

> ⚠️ **Debian 打包注意点**:`llvm-20-dev` 里的 `libLTO.so` 是指向 `libLTO.so.20.1` 的
> **悬空符号链接**(该文件被拆到别的包)。`LLVMExports.cmake` 会做存在性检查并 `FATAL_ERROR`。
> 解决:补一个指向 `libLLVM.so.20.1` 的符号链接即可(kilij 不使用 LTO target)。
> 建议写个循环,自动为所有缺失引用补链接。

**还必须满足**:宿主的 `clang`/`opt` 必须与插件**同属一份 LLVM**。
用官方静态 clang 配 Debian 共享插件,是两个实例,`cl::opt` 注册表不通,参数照样不认。

---

## 🏗️ 五、三段式 bitcode 构建管线(集成核心)

### 为什么不能直接用 clang

```bash
clang++ -O2 -fpass-plugin=Kilij.so -mllvm -fla ...
# → clang (LLVM option parsing): Unknown command line argument '-fla'
```

**原因是加载顺序**:clang 在 `CompilerInvocation::CreateFromArgs` 阶段就解析了 `-mllvm` 参数,
而插件要到 frontend `ExecuteAction` 才 dlopen。参数解析时插件的选项还没注册。

> 官方 tarball 静态版还有一个额外症状:`clang` 会直接报
> `unable to load plugin ... undefined symbol`。共享版则表现为"插件加载成功但参数不认"。

### 正确管线

```bash
# ① 编译为 bitcode(优化级别与真实构建一致,例如 -O2)
clang++ -std=c++17 -fPIC -O2 -emit-llvm -c src.cpp -o src.bc

# ② 合并为单一模块(必须!见下)
llvm-link *.bc -o all.bc

# ③ 跑优化管线 + 插件混淆
opt --load-pass-plugin=Kilij.so -passes='default<O2>' \
    -vm-mode=region -vm-select=all -vm-encode=mba -vm-hard \
    all.bc -o all.opt.bc

# ④ 回写为目标文件(必须 -O0!否则重新优化会抹掉混淆)
clang++ -std=c++17 -fPIC -O0 -c all.opt.bc -o all.o
```

### 四个必踩的坑

| # | 坑 | 后果 | 正确做法 |
|---|---|---|---|
| 1 | `opt` 不给 `-passes` | **完全不跑任何 pass**,插件回调不触发,输出与输入逐字节相同 | 必须给管线,如 `-passes='default<O2>'` |
| 2 | 不先 `llvm-link` | 单模块跑 `-O2` 会把 NOBF 跨文件引用的函数当死代码删掉(实测 893 基本块 → 1) | **先合并成单模块** |
| 3 | 用 `-O0` 输入 + `-passes=default<O2>` | 优化先于混淆大规模 DCE,混淆"看起来没生效"(伪结果) | 输入按**真实优化级别**编译(`-O2`) |
| 4 | 混淆后字节码用 `.obc` 之类扩展名 | clang 不识别,**静默无输出**(退出码 0 但文件不存在) | 必须用 `.bc` 扩展名 |

---

## 📊 六、实测数据

测试用例:`@Native` 类中的整数循环(300 次 × 200000 轮),
以及两个真实形态的**一次性验证函数**(TEA 32 轮 × 300 次)。
环境:AMD/Intel x86_64 8 核,LLVM 20.1.8,JDK 17。

### 6.1 混淆能力(IR 层面)

| 参数 | IR 行数 | 基本块 | xor 指令 |
|---|---|---|---|
| 原始 | 5,442 | 748 | 1 |
| `-fla` | 6,379 (+17%) | 763 | 80 |
| `-fla -bcf -mba -sub -split` | **20,615 (+279%)** | **1,778** | **2,248** |
| `-vm-mode=region -vm-select=all` | 14,144 (+160%) | 1,169 | 576 |

VM 模式下**全局变量从 9 个涨到 17 个** —— 这是真在生成字节码数组与解释器 runtime。

### 6.2 体积(整个原生库)

| 版本 | .so 字节 | 倍数 |
|---|---|---|
| 对照(仅 NOBF) | 60,592 | 1.00× |
| VM + fla/bcf/mba/sub | 232,704 | 3.84× |
| 8 个经典 pass | 179,440 | 2.96× |
| 纯 VM | 134,400 | 2.22× |

### 6.3 性能 — 按**真实场景**重测

> 一次性验证函数,单次运行耗时(3 次取最优):

| 验证函数形态 | 对照 | obf(8 经典 pass) | **vm(纯 VM)** | vmcl(VM+4 经典) |
|---|---|---|---|---|
| A: **返回状态码** | 1.03 ms | 6.79 ms | **0.78 ms** | 3.56 ms |
| B: 失败抛异常 | 0.74 ms | 3.67 ms | 0.72 ms | 1.20 ms |

**结论:单次验证最贵 6.8 ms,完全可接受。纯 VM 甚至比对照更快。**
体积:65 KB → 118 KB(纯 VM)/ 216 KB(VM+经典)。

> 📌 参考(不作为决策依据):若把混淆施加到**热循环**上,开销为
> 纯 VM 1.03× / VM+经典 1.71× / 8 经典 pass **4.32×**。
> 同时 NOBF 自身已比等价 Java 慢约 **52×**(印证上游关于性能的警告)。
> 这些数字说明:**不要把经典 pass 施加到热路径上**。

---

## ⚠️ 七、关键限制:`may-unwind call unsupported`

这是**必须绕开的唯一硬限制**。`-vm-report=<file>` 会输出每个函数的处置明细:

```
...__ngen_native_check_1...   selected
...__ngen_native_check_1...   skip    lower_fail:vm: may-unwind call unsupported
```

**同一份验证逻辑,只改一处 —— 失败时抛异常还是返回状态码 —— VM 的处置完全不同:**

| 验证函数写法 | VM 处置 |
|---|---|
| **不抛异常**(返回状态码) | ✅ `selected` → **完整虚拟化**(43 选中 / 0 跳过) |
| **失败抛异常** | ❌ `selected` 后**被丢弃**,只受经典 pass 保护 |

- 叠加 `-fla -bcf -mba -sub` **不能**让 VM 覆盖它(vmcl 里同样 `skip`),
  但经典 pass 仍会变换该函数。
- NOBF 自动生成的 JNI 胶水层**天然处处带异常路径**,实测跳过率约 50%;
  但这些本来也不是保护目标。

### ✅ 设计规则(重要)

> **手写的验证函数应写成"返回状态码、不抛异常",即可获得 VM 虚拟化。**
> 验证函数由我们控制,这条规则完全可执行。

---

## 🎯 八、集成设计(未来落点)

### 8.1 需要改动的文件

| 文件 | 改动 |
|---|---|
| `obfuscator/src/main/resources/sources/CMakeLists.txt.template` | **Clang 分支**(当前为空)改为三段式 bitcode 规则 |
| `obfuscator/src/main/resources/sources/build.sh.template` | 编译流程同步改为三段式 |
| `src/obfuscator_controller.*` | 增加"启用 kilij 虚拟化"开关与参数传递 |
| `src/runtime_bootstrap.*` | 检测/获取共享版 LLVM + Kilij.so + clang |
| `docs/DEPLOYMENT_GUIDE.md` | 补充工具链依赖说明 |

> `CMakeLists.txt.template` 的 Clang 分支当前是**空的**,正是预留的接入位置。

### 8.2 建议的默认参数

```
-vm-mode=region -vm-select=marked -vm-encode=mba -vm-hard
```

- `-vm-select=marked`(而非 `all`)—— 只虚拟化**显式标记**的函数,
  即"用户选择处理类"与验证函数,避免误伤 NOBF 胶水层
- 配合 `-vm-select-path=<子串列表>` 可按源码路径过滤
- 建议加 `-vm-report=<file>` 留档,便于排查覆盖率
- **不要**默认叠加 `-fla -bcf -mba -sub` 到热路径

### 8.3 工具链分发策略(二选一)

1. **随 AntiHackerX 分发**:免 root 解包方案已验证,114 MB 下载 / ~673 MB 解包
2. **按需下载**:沿用 `runtime_bootstrap` 既有模式,给用户"启用虚拟化"时再装

> 注意:这是**打包者机器**的依赖,不是终端用户的依赖。

---

## 🧪 九、复现步骤

```bash
# ── 环境变量 ──
LLVM_ROOT=/opt/llvm20                        # 第四节解包出来的前缀
KILIJ_SO=/path/to/kilij/_build/standalone/Kilij.so

# ── 1. 验证插件可加载(最重要的健康检查) ──
$LLVM_ROOT/usr/lib/llvm-20/bin/opt --load-pass-plugin=$KILIJ_SO --help-hidden \
  | grep -E "^ +--(fla|vm-mode|obf-str)"     # 应列出插件选项

# ── 2. 查看 VM 选择报告 ──
opt --load-pass-plugin=$KILIJ_SO -passes='default<O2>' \
    -vm-mode=region -vm-select=all -vm-encode=mba -vm-hard \
    -vm-report=/tmp/vm.rep all.bc -o out.bc
grep -E "selected|skip" /tmp/vm.rep

# ── 3. 端到端 ──
java -jar libs/native-obfuscator.jar app.jar out/    # 生成 out/cpp
bash 第五节的四步管线                              # 构建 .so
# 把 .so 注入 jar 的资源路径 native0/x64-linux.so 后:
java -cp app.jar demo.App
```

**注入路径规则**(来自生成的 `Loader.java`):
`<首个包名>/<平台>-<os>.so`,例如 `native0/x64-linux.so`。

---

## 🚧 十、已知风险与后续事项

### 风险

1. **仓库成熟度**:创建仅 1 天,零用户、零 issue,作者声明 Linux 未测试。
   → 缓解:vendor 进本项目并自维护(本项目已在维护 native-obfuscator fork,具备能力)。
2. **上游突然变更/停更**:无历史可参照。
3. **VM 覆盖率受 `may-unwind` 限制**:见第七节,可用编码规则规避。

### 后续待办(集成阶段)

- [ ] `CMakeLists.txt.template` Clang 分支改写为三段式
- [ ] `build.sh.template` 同步
- [ ] `runtime_bootstrap` 增加 LLVM/Kilij 获取与检测
- [ ] `obfuscator_controller` 增加虚拟化开关与参数
- [ ] 为"用户选择处理类"设计标记机制(`-vm-select=marked` 需要的注解约定)
- [ ] 向上游反馈第三节的 5 处 Linux 构建问题(#1~#3、#5 为通用问题)

---

## 📚 十一、参考

- kilij: <https://github.com/dannyisbad/kilij>
- Obfuscator-LLVM Release License (UIUC/NCSA)
- `docs/OBFUSCATOR_CONTROLLER.md` — 混淆器进程管理
- `docs/RUNTIME_BOOTSTRAP.md` — 运行时依赖获取
- `docs/LOADER_ANALYSIS.md` — 生成的 Loader 与资源路径规则
