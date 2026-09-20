# 发布检查表

> 发版前逐项过一遍。产品方向与功能规划见 [../TODO.md](../TODO.md)。

## 0. 推 tag 即自动构建(已配好)

`.github/workflows/release.yml`,推 `v*` 标签触发,产出四份东西并建 Release:

| 产物 | 说明 |
|---|---|
| `AntiHackerX-<tag>-win64.zip` | MSYS2/MinGW 编译,内含 Qt 运行库,解压即用 |
| `AntiHackerX-<tag>-linux-x64.tar.gz` | ubuntu-22.04(glibc 2.35 / Qt 6.2.4) |
| `AntiHackerX-<tag>-linux-arm64.tar.gz` | arm 运行器 |
| `jar-obfuscator-2.0.1-jar-with-dependencies.jar` | **文件名不能改**,流水线按固定名找它 |

另外会生成 `SHA256SUMS.txt`。Linux 那个 job 里还会跑
`check-license-isolation.sh` —— 许可证隔离被破坏时**直接卡住发布**。

- [ ] 试跑:先手动触发一次(`workflow_dispatch`,不发 Release)确认四个 job 都绿
- [ ] **写发布说明**:` .github/release-notes/<标签>.md`(没有这个文件时
      Release 正文会退化成自动生成的提交列表,流水线会打 `::warning::`)
- [ ] 再推标签:`git tag -a v1.0.0 -F -` → `git push origin v1.0.0`
      ⚠️ **不要再删这个标签** —— 删标签会把对应的 Release 一并删掉(产物也没了),
      要重发请另推一个新版本号
- [ ] 发布后确认流水线末尾的 `Release OK: <url> 草稿=False 产物数=5` 这行自检
- [ ] 发布后下 win64 的包,在没装 Qt 的机器上解压跑一次

## 1. native-obfuscator fork 发 **v1.4.8**

- [x] `native-obfuscator/obfuscator/src/main/java/by/radioegor146/Main.java` 的
      `VERSION` 常量:`1.4.7` → `1.4.8`
- [x] `MODIFICATIONS.md` 追加一条(本轮改动:`--loader-name` / `--hidden-name`,
      合成隐藏类去掉 `hidden` 子包)
- [x] `git tag -a v1.4.8 -F -` 然后 **只推这一个标签**
      ⚠️ 绝不 `git push --tags` —— 本地有 29 个上游历史标签,会各建一个 Release
- [x] 发布后验证:下载产物跑 `--help`,能看见 `--loader-name` / `--hidden-name`

> **实际结果**(2026-09-18):`master = 4e92b2a`,标签 `v1.4.8` → `4e92b2a`,
> CI 一次通过。资产:`native-obfuscator-v1.4.8.zip`(2.5 MB,内含
> `native-obfuscator.jar` + `LICENSE`/`NOTICE`/`README.md`/`MODIFICATIONS.md`,
> 满足 GPL 分发要求)、`native-obfuscator-v1.4.8-src.zip`、`SHA256SUMS.txt`。
> 实测 jar 的 `--version` 报 `1.4.8`,与标签号一致。

> ⚠️ **仍然待办**:AntiHackerX **没有**版本过期检查 —— 它只在
> `libs/native-obfuscator.jar` 不存在时才下载(`runtime_bootstrap.cpp` 里
> `if (QFileInfo(m_jarPath).size() > 0) { 跳过 }`)。所以**已经用过旧版的用户
> 不会自动升级**,得先实现「取 `tag_name` 与本地 `--version` 比对、不一致就重下」。
> (`Main.java` 的注释声称有这个对比,那是早期设想,与现状不符。)

## 2. 决定 fork 归属,并同步下载地址

`src/runtime_bootstrap.cpp` 里写死了个人账号:

```
34:  https://api.github.com/repos/xiaofanforfabric/native-obfuscator/releases/latest
36:  https://github.com/xiaofanforfabric/native-obfuscator/releases
```

- [x] 决策:**fork 留在个人账号**(`xiaofanforfabric/native-obfuscator`)
      ⇒ 两处 URL **无需改动**,`releases/latest` 已指向 v1.4.8
- [x] —(不适用:不搬组织)
- [ ] **补版本过期检查**(见第 1 节的待办):现在清空 `libs/` 能自动下载新版,
      但**已有旧 jar 的机器不会重下** —— 这个必须实现,否则老用户升了个寂寞
- [ ] 顺带把 `kFallbackReleaseTag` 从 `v1.4.0` 更新(API 限流时会回退到它,
      而 v1.4.0 还没有命名随机化)

## 3. 建 GitHub 组织与仓库

- [ ] 建组织(Free 套餐足够),开启 **Require two-factor authentication**
- [ ] 建 4 个仓库:

| 仓库 | 许可证 | 备注 |
|---|---|---|
| `AntiHackerX` | 见根 `LICENSE` | 主程序(Qt6/C++) |
| `AntiHackerXVerify` | **MIT** | 验证模块,可单独复用 |
| `jar-obfuscator` | **MIT** | fork,**保留上游署名**(4ra1n / Jar Analyzer Team) |
| `native-obfuscator` | **GPL-3.0** | fork,保留 `LICENSE` + `MODIFICATIONS.md` |

- [ ] 仓库简介与 Topics(**可直接复制**):
  - About:`给 Java 产物加壳:字节码混淆 + 类加密 + 原生编译 | JAR protector with class encryption and native compilation`
  - Topics:`java` `obfuscator` `jar` `anti-decompile` `code-protection` `qt6` `cpp` `minecraft-plugin` `paper-plugin`
- [x] `check-license-isolation.sh` 加进 CI —— 它是"主程序与 GPL 组件只通过
      **进程调用**耦合"的**证据**,开源后这个立场必须能被自动验证
      (在 `release.yml` 的 `gui-linux` job 里,`linux-x64` 上构建完后跑;
      它失败会**直接卡住 Release**)
- [ ] 建组织级 `.github` 仓库(放默认社区健康文件)

## 4. 仓库卫生(开源前最后一次过一遍)

- [x] `testp/`、`native-obfuscator/`、`/java/`、`build/` 都确认未入库
      (`libs/` 只入库 `libs/minizip/` —— 它是 CMake 直接编译的内嵌源码,
      没有它谁都构建不起来;`.gitignore` 里必须是 `/libs/*` 而非 `/libs/`)
- [x] `FanVerify-obf/`(实验产物)要么清理要么 gitignore
- [ ] **扫历史提交**:确认从没提交过真实插件 JAR、签名私钥、`.gradle` 缓存
      (如有 → `git filter-repo` 清理,别只在新提交里删)
- [ ] 主程序首个 Release:三平台二进制 + SHA-256 校验和

## 5. 文档

- [ ] `CHANGELOG.md`(从 `native-obfuscator/MODIFICATIONS.md` 与历代会话的关键修复整理)
- [ ] `CONTRIBUTING.md`(构建方式、代码风格、PR 要求)
- [ ] `SECURITY.md`(漏洞上报渠道 —— 一个做安全的项目尤其需要)
- [ ] **中英两份 README 保持同步**:改 `README.md` 时记得改 `README.en.md`
      (可加个 CI 检查两边的章节标题与表格行数是否一致,防止单边漂移)
- [ ] README 加截图 / GIF:选项面板、类扫描表、Paper 上加载成功的日志
