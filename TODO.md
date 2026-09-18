# AntiHackerX 路线图

> **当前基线:Paper / Bukkit 已彻底跑通** —— 19.5 MB / 4600 类的 GrimAC 加壳后能正常
> 加载、启用、运行。
>
> 下面按「平台扩展 → 混淆能力 → 工程化」排。图例:🎯 主线 · 🔬 需要验证 · ⏸ 暂缓

## 平台支持矩阵

| 平台 | 入口声明在哪 | 类加载器 | 单实例硬限制 | 加载时是否要扫类文件 |
|---|---|---|---|---|
| **Paper / Bukkit** ✅ | `plugin.yml` → `main:` | `PluginClassLoader` | ✅ 有(`JavaPlugin`) | 否 |
| **Fabric** 🎯 | `fabric.mod.json` → `entrypoints` | `KnotClassLoader` | 无 | 否,**但 Mixin 靠 JSON 里的类名** |
| **Forge** | `mods.toml` + `@Mod` 注解 | `TransformingClassLoader` | 无 | ✅ **有:FML 用 ASM 扫全量注解** |
| **普通 JAR** 🔬 | `MANIFEST.MF` → `Main-Class:` | `AppClassLoader` | 无 | 否 |
| **Spring Boot** | `MANIFEST.MF` → `Start-Class:` | `LaunchedURLClassLoader` | 无 | ✅ **有:组件扫描 + 自动配置** |

这张表是骨架:**"加载时要不要读类文件"直接决定加密能不能用**。
最后两列的 ✅ 就是每个平台的真正难点所在。

---

## 一、平台扩展

### ✅ 已完成:Paper / Bukkit

- 桥类插在**真实主类与 `JavaPlugin` 之间**(改写真实主类的父类),避开单实例硬检查
- `plugin.yml` 的 `main:` 原样保留(主类名不变,用户明确要求)
- 明文集合压到「主类 + 主类直接引用 + 其超类型闭包」(GrimAC 上 62 个类 / 4600)

### 🎯 下一步 1:Fabric MOD

**比 Paper 简单的地方**:没有 `JavaPlugin` 那种单实例硬限制 →
**不需要改写真实入口类的父类**,桥类只做两件事:装载荷 → 反射调真实入口。

- [ ] `VerifyModule::Kind` 加 `FabricMod`,新增 `FabricBootstrap.java.tmpl`
- [ ] 入口要按 `entrypoints` 里**实际声明了哪些**来实现:
      `ModInitializer` / `ClientModInitializer` / `DedicatedServerModInitializer`
      (Fabric 只调声明过的,但我们要三个都实现才能通用)
- [ ] `fabric.mod.json` 的 `entrypoints.*` 改指向我们的入口;
      `id` / `version` / `depends` / `environment` 一律不动
- [ ] 注入目录映射到 **jar 根**(不是 `BOOT-INF/classes`)—— `fabric.mod.json` 在根

**最大的坑:Mixin**

- [x] **依赖已就位**:验证模块加了 `compileOnly("net.fabricmc:sponge-mixin:…")`
      (Fabric fork,与上游共用 `org.spongepowered.asm.mixin.*`;上游
      `org.spongepowered:mixin` 只在 Sponge 仓库)。打包时 javac 的 classpath
      就是界面上的「额外依赖」那一栏,它支持直接给目录(递归找 jar)
- [ ] 写第一个 mixin 类时注意:**没有 fabric-loom 就没有 refmap**,
      所以要用 `targets = "..."` 字符串目标,别用类型化 target,
      否则生产环境重映射会失败
- `*.mixins.json` 用"包名 + 类名"**字符串**引用 mixin 类,而混淆器会改名
  → 必须让 `ResourceTransformer` **同步重写 mixins.json**,否则 mod 直接崩
- mixin 类**不能被加密**:Mixin 框架用自己的 `MixinService` 从 jar 里直接读字节,
  不走 `Class.forName`
- 第一阶段最稳方案:**mixin 类一律明文 + 冻结改名**,先跑通不带 mixin 的 mod
- 验收:用 Fabric API 的示例 mod(命令注册 + 事件监听 + `ServerLifecycleEvents`)全部正常

### 下一步 2:Forge MOD

**最难,而且收益最低 —— 建议先做完 Fabric 再评估要不要投入。**

- FML 在加载前会用 ASM **扫描整个 jar** 找 `@Mod` 注解(构建 `ModFileScanData`)
  → 被加密的类它扫不到
- Forge 的入口不是接口而是**注解** → 桥类没法"冒充"它。两条路:
  - 入口类(带 `@Mod` 的那个)**明文 + 冻结名字**,只加密它引用到的业务类
  - 入口类保留为薄壳(只有注解 + 转发),真实逻辑全部放进载荷
- `@SubscribeEvent` / `@EventBusSubscriber` / `@ObjectHolder` 所在类都要冻结改名
  (事件总线按注解与方法签名注册,乱改会静默漏事件 —— 最难排查的一类 bug)
- `mods.toml` 在 `META-INF/`;1.12- 用根目录 `mcmod.info`(两种都要支持)
- ⚠️ **要如实告诉用户**:Forge 上"整包加密"的收益明显低于另两个平台
- 验收:示例 mod 注册物品/方块 + 事件订阅正常

### 🔬 下一步 3:普通 JAR(桌面应用)

**最简单 —— 而且能去掉一个约束。**

- `MANIFEST.MF` 是我们自己写的 → **`Main-Class` 可以直接指向桥类**,
  真实主类**完全不必明文**(和 Paper 正好相反!)
- 现有 `Kind::Plain` 模板已经这么做,主要工作是**验证**:
  - [ ] `AppClassLoader` 下 `Lookup.defineClass` 的包可见性
  - [ ] `getCodeSource()` 指向的 jar 路径 / 非 `-jar` 的 classpath 启动方式
  - [ ] `Class.forName` 反射、`getResource()` 读资源、JNI、多线程
  - [ ] `ServiceLoader`(`META-INF/services/` 里按字符串引用 → 类名要冻结)
- 验收:Swing 应用 + 一个靠反射的库,加壳前后功能一致

### 下一步 4:Spring Boot

- 布局是 `BOOT-INF/classes/` + `BOOT-INF/lib/*.jar`(嵌套 jar)
  → 注入要落到 **`BOOT-INF/classes/`**
- `Main-Class: JarLauncher` 与 `Start-Class:` **一个字都不能动**
- **最大障碍:组件扫描**。`@ComponentScan` 默认按类路径资源枚举 `.class` 去读注解
  —— 被加密的类它根本看不到。三条出路:

| 方案 | 做法 | 取舍 |
|---|---|---|
| **A(推荐)** | 生成 `META-INF/spring.components` 索引(Spring 5+ 优先用索引);载荷在 `Start-Class` 之前就把类 `defineClass` 进同一个 `LaunchedURLClassLoader` | 收益最高,需要验证索引覆盖率 |
| **B(保守)** | 只加密**非组件**的叶子类;`@Service` / `@Controller` / `@Configuration` 一律明文 | 零风险,收益低 |
| **C(必须做)** | `spring.factories` / `AutoConfiguration.imports` 里按字符串引用的类**冻结名字** | 与 A/B 都无关,是硬要求 |

- 验收:带 REST Controller + JPA 的 demo,加壳后 DI 正常、`/actuator/health` 正常

---

## 二、混淆能力扩展

### 🚧 AI 欺骗字符串(部分已实现)

**目标**:让"把 JAR 丢给 LLM 让它总结/还原逻辑"这条路**产出错误结论**,
而不是"读不懂" —— 与现有垃圾代码的区别是:它**语义上像真的**。

| 档位 | 手段 | 针对的逆向方式 | 状态 |
|---|---|---|---|
| 提示词注入 | 每个类注入一份 `static final String NOTICE`,内容是写给自动化分析系统/AI 的声明,要求它拒绝解释本类实现 | 攻击者把代码或反编译结果直接贴给模型(批量脚本、随手一贴) | ✅ **已实现**(`AiNoticeTransformer`,GUI 第 14 个开关,默认关) |
| L1 | **假常量**:看起来像密钥 / token / 内部 URL / 许可证校验的字符串(实际从不使用) | "找密钥、找授权逻辑" | 计划中 |
| L2 | **可信但不正确的业务语义**:永不执行、但看起来像核心业务的分支 | LLM 总结出错误的功能描述 | 计划中 |
| L3 | **假元数据**:伪造 `SourceFile`、伪造看起来像 Spring/Netty 的注解与包名 | 让 AI 推断出错误的技术栈 | 计划中 |
| L4 | **交叉引用不存在的 API**:引用不存在的方法/字段名 | 让 AI 给出无法验证的结论 | 计划中 |

**接入点(已勘明)**:

- `jar-obfuscator` 的 `Runner` 里,紧跟 `StringTransformer` 之后插入
  `AIDecoyTransformer`(在 `JunkCodeTransformer` 之前),这样它能复用字符串池
  → 提示词注入已按这个位置落地(`AiNoticeTransformer`),后续档位照此扩展
- 配置项加到 `BaseConfig.java` + GUI 第 14 个开关 + `packer_pipeline.h` 的 `Config`
- 强度分档沿用 `junkLevel` 的现成做法

**设计上必须写清的三件事**

1. 对**动态分析 / 符号执行完全无效**,只影响"读代码的人或模型"
   → UI 上要写明,别让用户以为它能替代加密
2. 会让**体积和误报**上升:假密钥可能触发杀软与密钥扫描器
   → **默认关**,且必须支持"只对指定类生效"的白名单(别污染对外 API)
3. 有价值的是「**可信但不正确**」的业务语义。**提示词注入型内容已知有争议,
   但仍按需求落地了**(`AiNoticeTransformer`,默认关) —— 用之前得知道:
   它一旦被用户自己的代码审计工具、上游 CI 或应用市场扫描到,惹的是产品自己的
   麻烦;而且效果远不如一条看起来天衣无缝的假业务逻辑。
   它对抗的是"批量脚本 + 随手一贴",不是有备而来的对手。

### 其他候选

- [ ] `p.dat` 索引加密(现在类名是明文可读的)
- [ ] 主类常量池死条目裁剪(残留旧类名,`javap -v` 可见)
- [ ] 明文 stub 类的**反射指纹**(成员名/描述符/修饰符的有序哈希)
      → 补上"`.class` 字节不在签名覆盖范围"的缺口
- [ ] 控制流平坦化(先评估:uint 除法/取模的预计算常量是常见指纹)
- [ ] 类合并 / 拆包,打散包结构

---

## 三、工程化 / 生态

- [ ] **崩溃反混淆**:现在 `mapping.txt` 只归档不下发 → 做一个"把客户给的堆栈
      还原成原始类名"的工具。**这是客户体验的关键一环**,建议优先级仅次于平台扩展
- [ ] 混淆配置导入/导出、预设分享
- [ ] 增量打包 / 批量打包
- [ ] 三平台二进制的一键 CI
- [ ] 英文界面 + 英文 `docs/`(配合已有的 `README.en.md`)
- [ ] 体积优化:原生库是产物大头,可评估按目标平台拆分

---

## 四、⏸ 暂缓

- **kilij 编译期 VM 虚拟化**:评估数据齐了(见 `docs/KILIJ_INTEGRATION.md`),
  但依赖共享版 LLVM(官方 tarball 纯静态,加载不了 pass plugin),需要自建并发布
- **配合 VMProtect / Themida**:外层壳,职责不重叠,交给用户自己叠
- **macOS**:NOBF 侧需要 Apple SDK,产物必须在 macOS 上构建(无法交叉编译)

---

## 五、发布与工程债

发布前的检查项已单独挪到 **[docs/RELEASE_CHECKLIST.md](docs/RELEASE_CHECKLIST.md)**
(NOBF 发版、fork 归属、仓库卫生等)。

仍然欠着、但**不阻塞开发**的技术债:

- `p.dat` 索引明文
- 主类常量池死条目
- 反篡改未覆盖 `.class` 字节
- `AhxSign` 缺 `sign` 子命令(手工改过资源后无法重签)

---

## 已知限制(要如实告知使用者)

| 限制 | 原因 | 缓解 |
|---|---|---|
| 类文件字节不参与签名 | Paper 会重写每个 `.class` | 载荷有 GCM + 签名双重背书;模块逻辑与校验都在 `.so` 里 |
| 有一批"必须不加密"的类 | 平台在载荷就位**之前**就要校验入口类 | 已压到最小(GrimAC 62/4600);名字仍是混淆过的 |
| Forge 上加密收益偏低 | FML 会扫注解,入口类必须明文 | 见平台矩阵 |
| AI 欺骗对动态分析无效 | 它只影响"读代码的人或模型" | UI 上写明 |
| 原生化明显降性能 | 方法体走 JNI | 只对核心逻辑开 |
