# 发布检查表

> 发版前逐项过一遍。产品方向与功能规划见 [../TODO.md](../TODO.md)。

## 1. native-obfuscator fork 发 **v1.4.8**

- [ ] `native-obfuscator/obfuscator/src/main/java/by/radioegor146/Main.java` 的
      `VERSION` 常量:`1.4.7` → `1.4.8`
- [ ] `MODIFICATIONS.md` 追加一条(本轮改动:`--loader-name` / `--hidden-name`,
      合成隐藏类去掉 `hidden` 子包)
- [ ] `git tag -a v1.4.8 -F -` 然后 **只推这一个标签**
      ⚠️ 绝不 `git push --tags` —— 远端有 22 个上游历史标签,会各建一个 Release
- [ ] 发布后验证:下载产物跑 `--help`,能看见 `--loader-name` / `--hidden-name`

> **为什么必须先做**:本地重建的 jar 版本号仍是 `1.4.7`,与 GitHub latest 同号,
> 更新检查不会重下 ⇒ **用户机器上拿到的还是旧版**(没有命名随机化,
> 两个加壳插件装一起必 `LinkageError`)。

## 2. 决定 fork 归属,并同步下载地址

`src/runtime_bootstrap.cpp` 里写死了个人账号:

```
34:  https://api.github.com/repos/xiaofanforfabric/native-obfuscator/releases/latest
36:  https://github.com/xiaofanforfabric/native-obfuscator/releases
```

- [ ] 决策:fork 留个人账号 / 搬进组织
- [ ] 若搬进组织:上面两处同步改
- [ ] 重新验证整条链路:清空 `libs/`,启动一次,确认能自动下载 + 更新检测正常

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
- [ ] `check-license-isolation.sh` 加进 CI —— 它是"主程序与 GPL 组件只通过
      **进程调用**耦合"的**证据**,开源后这个立场必须能被自动验证
- [ ] 建组织级 `.github` 仓库(放默认社区健康文件)

## 4. 仓库卫生(开源前最后一次过一遍)

- [ ] `testp/`、`native-obfuscator/`、`/java/`、`/libs/`、`build/` 都确认未入库
- [ ] `FanVerify-obf/`(实验产物)要么清理要么 gitignore
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
