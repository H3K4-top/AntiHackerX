/*
 * AntiHackerX 打包工具 —— 把目标 JAR 的类加密成载荷,并注入加壳入口。
 *
 * SPDX-License-Identifier: MIT
 * 许可证全文见仓库中的 LICENSE。
 *
 * 用法:
 *   java AhxPacker <输入jar> <输出jar> <入口目录> <密钥hex> <新Main-Class> [<桥类全名>] [--sign-key <私钥文件>]
 *
 * 做过的事:
 *   1. 把输入 JAR 里所有 .class 加密进单一载荷 <载荷路径>(明文不写进输出);
 *   2. 把 <入口目录> 下的**所有文件**原样注入 —— 包括验证模块的 stub class、
 *      native0/Loader.class、以及 native0/x64-linux.so / x64-windows.dll;
 *   3. 输入 JAR 的其余资源原样复制;若存在 plugin.yml,把其 main 改指向加壳入口;
 *   4. 写新的 MANIFEST(默认 Main-Class 指向加壳入口);
 *   5. 给了 --sign-key 就再写一份 top/h3k4/p.sig:对每个非 .class、
 *      非 META-INF/ 条目算 SHA-256,整表用 ECDSA P-256 签名。
 *
 * 载荷格式必须与 ahx.runtime.AhxPayload 保持一致。
 *
 * 编译运行:
 *   java AhxPacker.java <输入jar> <输出jar> <入口class目录> <密钥hex> <新Main-Class>
 */
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.KeyFactory;
import java.security.MessageDigest;
import java.security.PrivateKey;
import java.security.SecureRandom;
import java.security.Signature;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.jar.Attributes;
import java.util.jar.Manifest;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

import javax.crypto.Cipher;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

public final class AhxPacker {

    private static final String MAGIC = "AHXPAY01";   // 恰好 8 字节,必须与 AhxPayload 一致
    private static final String PAYLOAD_ENTRY = "top/h3k4/p.dat";
    private static final int NONCE_LENGTH = 12;
    private static final int TAG_BITS = 128;

    /** 反篡改签名块。格式必须与 AhxSignature 严格一致。 */
    private static final String SIG_MAGIC = "AHXSIG01";
    private static final String SIGN_ENTRY = "top/h3k4/p.sig";
    private static final int SIG_VERSION = 1;

    private AhxPacker() {
    }

    public static void main(String[] args) throws Exception {
        // 尾部选项先摘出来,剩下的才是位置参数。
        final List<String> positional = new ArrayList<String>();
        String signKeyPath = "";
        // 必须明文的额外类(Fabric 的 mixin 类):内部名,逗号分隔
        String keepPlainArg = "";
        // 资源覆盖:entryPath=文件路径。用于改写 fabric.mod.json 这类元数据 ——
        // 在 Java 里手工拼 JSON 太危险,交给调用方(Qt 那边有真 JSON 解析器)。
        final List<String> resourceOverrides = new ArrayList<String>();
        for (int i = 0; i < args.length; i++) {
            if ("--sign-key".equals(args[i]) && i + 1 < args.length) {
                signKeyPath = args[++i];
            } else if ("--keep-plain".equals(args[i]) && i + 1 < args.length) {
                keepPlainArg = args[++i];
            } else if ("--set-resource".equals(args[i]) && i + 1 < args.length) {
                resourceOverrides.add(args[++i]);
            } else {
                positional.add(args[i]);
            }
        }
        if (positional.size() < 5) {
            System.err.println("用法: AhxPacker <输入jar> <输出jar> <入口class目录> <密钥hex> "
                               + "<Main-Class> [<桥类全名>] [--sign-key <私钥文件>]");
            System.exit(1);
        }

        final File srcJar = new File(positional.get(0));
        final File outJar = new File(positional.get(1));
        final File bootstrapDir = new File(positional.get(2));
        final String keyHex = positional.get(3);
        final String newMainClass = positional.get(4);
        // 第 6 个参数:Bukkit/Paper 专用。
        //
        // 服务端只允许一个 JavaPlugin 实例(PluginClassLoader 里有硬检查,
        // 而且它把一个 final 字段绑在那个实例上),所以我们不能"再造一个真实插件
        // 实例"。于是改成把**真实主类的父类**换成这个桥类:
        //   JavaPlugin <- 桥(<clinit> 里定义载荷类) <- 真实主类
        // 桥的静态初始化在真实主类的构造器之前跑,于是所有载荷类在
        // 真实主类开始干活时就已就位,而 JavaPlugin 的构造器只会被执行一次。
        final String bridgeClass = positional.size() > 5 ? positional.get(5).trim() : "";

        if (!srcJar.isFile()) {
            System.err.println("输入 JAR 不存在: " + srcJar);
            System.exit(2);
        }
        if (!bootstrapDir.isDirectory()) {
            System.err.println("入口 class 目录不存在: " + bootstrapDir);
            System.exit(2);
        }

        final SecretKeySpec key = new SecretKeySpec(hexToBytes(keyHex), "AES");
        final SecureRandom random = new SecureRandom();

        // ---------- 1. 先收集要原样注入的文件 ----------
        //    必须在读输入 JAR 之前做:输入 JAR 里可能含有同名类
        //    (验证模块工程自己就带着守卫源码,编译出的 JAR 里自然也有),
        //    那些必须跳过加密,否则会与注入的明文类重名。
        final Map<String, byte[]> injected = new LinkedHashMap<String, byte[]>();
        collectFiles(bootstrapDir, bootstrapDir, injected);

        final java.util.Set<String> injectedNames = new java.util.HashSet<String>();
        for (String name : injected.keySet()) {
            if (name.endsWith(".class")) {
                injectedNames.add(name.substring(0, name.length() - ".class".length()));
            }
        }

        final String bridgeInternal = bridgeClass.replace('.', '/');
        int retargeted = 0;

        // ---------- 2. 先把输入 JAR 整个读进内存 ----------
        //    之前是一遍过边读边分流,但“哪些类必须明文”得先看完全部才能算出来
        //    (主类的超类型链可能引到任意一个类),所以拆成两遍。
        final Map<String, byte[]> allClasses = new LinkedHashMap<String, byte[]>();
        final Map<String, byte[]> resources = new LinkedHashMap<String, byte[]>();
        int skipped = 0;
        int multiRelease = 0;

        ZipFile zip = new ZipFile(srcJar);
        try {
            Enumeration<? extends ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement();
                if (entry.isDirectory()) {
                    continue;
                }
                final String name = entry.getName();
                if ("META-INF/MANIFEST.MF".equalsIgnoreCase(name)) {
                    continue;   // 清单由我们重写
                }
                final byte[] data = readAll(zip.getInputStream(entry));

                if (name.equals(PAYLOAD_ENTRY)) {
                    continue;   // 输入里已有的旧载荷,丢弃
                }
                if (name.endsWith(".class")) {
                    // 多版本 JAR:META-INF/versions/9/... 下的类只是同一批类的
                    // JDK9+ 变体,而且那条路径不是合法的 Java 包名 ——
                    // 加密进去后运行时也算不出来(defineClass 拒绝 META-INF.versions...),
                    // 所以直接丢掉;同名类在 JAR 根上本来就有。
                    if (name.regionMatches(true, 0, "META-INF/versions/", 0, 18)) {
                        ++multiRelease;
                        continue;
                    }
                    final String internalName = name.substring(0, name.length() - ".class".length());
                    if (injectedNames.contains(internalName)) {
                        ++skipped;   // 由注入版本取代
                        continue;
                    }
                    allClasses.put(internalName, data);
                } else {
                    if (injected.containsKey(name) || isMetadataToRewrite(name)) {
                        ++skipped;   // 由注入版本取代 / 由本类重写
                        continue;
                    }
                    resources.put(name, data);
                }
            }
        } finally {
            zip.close();
        }

        // ---------- 3. 钉住主类、主类直接引用到的类型,以及它们的超类型链 ----------
        //
        // 这些类必须**明文**留在 JAR 里,不能进载荷。原因不是“为了方便”,
        // 而是 JVM 的时序硬约束。Paper 在 PluginClassLoader.<init> 里就做了
        //
        //   Class.forName(pluginDescription.getMain(), /*initialize=*/true, this)
        //
        // 这一次调用里,JVM 会先 defineClass(解析直接父类与直接超接口)、
        // 再 link(字节码校验)、最后 initialize(跑 <clinit>)。而载荷是在
        // **桥类的 <clinit>** 里才被定义进去的 —— 也就是最后那一步。
        //
        // 所以校验期需要的一切都必须能由宿主加载器从 JAR 里直接找到:
        //
        //   ① 主类自己 —— plugin.yml 的 main 要能被 forName 到;
        //   ② 主类的直接超类与直接超接口(递归)—— defineClass 立即解析它们;
        //   ③ 主类字节码里出现过的**所有类型引用** —— 字节码校验器在
        //      `areturn/putfield/调用` 这类要做“可赋值性检查”的地方,
        //      会把类型真的加载起来(和“纯引用是懒解析”并不矛盾:
        //      懒解析的是**执行期**的解析,校验期的可赋值性检查是另外一回事)。
        //
        //   ④ ③里那些类自己的超类型链 —— 它们被加载时就地解析父类/接口。
        //
        // 注意 ③ 只需要一层:被校验器拉进来的类只是“被加载”,不会被**校验**,
        // 所以不需要再递归它们的引用(递归会滚成整个插件,实测 GrimAC
        // 一层是 62 个类,全递归是 4574 个类 —— 那等于没加密)。
        // ---------- 3. 主类以输入 JAR 自带的 plugin.yml 为准 ----------
        //
        // 不要去用映射表“猜”主类(jar-obfuscator 生成的 mapping 是
        // `原名 -> 混淆名`,而我们手里的名字可能已经是混淆名了 —— 一旦猜错,
        // plugin.yml 就会被写成一个永不存在的名字,服务端直接
        // InvalidPluginException: ClassNotFoundException)。
        //
        // 而 jar-obfuscator 早就把 plugin.yml 里的 main 改成了**正确的**
        // 混淆名。所以这里直接读它,一个字都不改。
        String declaredMain = newMainClass;
        final byte[] pluginYml = resources.get("plugin.yml");
        if (pluginYml != null) {
            for (String line : new String(pluginYml, StandardCharsets.UTF_8).split("\n")) {
                final String t = line.trim();
                if (t.startsWith("main:")) {
                    final String v = t.substring("main:".length()).trim();
                    if (!v.isEmpty()) {
                        declaredMain = v;
                    }
                    break;
                }
            }
            if (!declaredMain.equals(newMainClass)) {
                System.out.println("主类以 plugin.yml 为准 : " + declaredMain
                        + "(命令行给的是 " + newMainClass + ")");
            }
        }

        final Map<String, byte[]> pinned = new LinkedHashMap<String, byte[]>();
        final String mainInternal = declaredMain.replace('.', '/');

        // 额外的必须明文集合:Fabric 的 mixin 类。
        //
        // 为什么非要明文:Mixin 框架是**自己从 JAR 里按名字读字节**的,
        // 不走 Class.forName,也不经过我们那个解密加载器 —— 加密了它就看不到这个类,
        // mod 直接静默失效。
        //
        // 为什么连超类型也一起钉:JVM 校验 mixin 类时会解析它的父类/接口,
        // 缺一个就是 NoClassDefFoundError(和 GrimAC 那次同一个原因)。
        // 注意这里**不重定向父类**:mixin 类不是入口,没有插桥这一说。
        int keptPlainCount = 0;
        for (String raw : keepPlainArg.split(",")) {
            final String keep = raw.trim().replace('.', '/');
            if (keep.isEmpty()) {
                continue;
            }
            if (!allClasses.containsKey(keep)) {
                System.err.println("警告: --keep-plain 指定的类不在 JAR 里: " + keep);
                continue;
            }
            pinVerifierNeed(allClasses, keep, new java.util.HashSet<String>(), pinned);
            ++keptPlainCount;
        }
        if (keptPlainCount > 0) {
            System.out.println("mixin 明文   : " + keptPlainCount + " 个类(+它们引用的类型)");
        }

        // 资源覆盖:在算签名之前就得落好 —— 反篡改签名盖的是最终字节,
        // 改晚了会把已经被签过的内容又改一遍,产物直接校验不过。
        for (String spec : resourceOverrides) {
            final int eq = spec.indexOf('=');
            if (eq <= 0 || eq == spec.length() - 1) {
                System.err.println("错误: --set-resource 需要 entryPath=文件 形式,收到 " + spec);
                System.exit(2);
            }
            final String entryPath = spec.substring(0, eq);
            final byte[] content = java.nio.file.Files.readAllBytes(
                    java.nio.file.Paths.get(spec.substring(eq + 1)));
            resources.put(entryPath, content);
            System.out.println("资源覆盖     : " + entryPath + " (" + content.length + " 字节)");
        }
        if (!bridgeInternal.isEmpty()) {
            pinVerifierNeed(allClasses, mainInternal, new java.util.HashSet<String>(), pinned);
            if (!pinned.containsKey(mainInternal)) {
                System.err.println("错误: 在输入 JAR 里找不到主类 " + mainInternal);
                System.err.println("      plugin.yml 声明的是 " + declaredMain);
                System.err.println("      JAR 里前 20 个类:");
                int shown = 0;
                for (String n : allClasses.keySet()) {
                    System.err.println("        " + n);
                    if (++shown >= 20) {
                        break;
                    }
                }
                System.exit(3);
            }
        }

        // 主类:父类重定向后明文;链上其余类型:原样明文。
        final java.util.Set<String> plainNames = new java.util.HashSet<String>();
        for (Map.Entry<String, byte[]> e : pinned.entrySet()) {
            if (e.getKey().equals(mainInternal)) {
                injected.put(e.getKey() + ".class",
                             retargetSuperclass(e.getValue(), bridgeInternal));
                ++retargeted;
            } else {
                injected.put(e.getKey() + ".class", e.getValue());
            }
            plainNames.add(e.getKey());
        }

        // ---------- 4. 剩下的全部加密进载荷 ----------
        //
        // 除了上面钉住的那批,其余类一律加密。这份名单是**卡着 JVM 时序**算出来的,
        // 多钉一个就少保护一个,少钉一个就加载失败:
        //
        //   Paper: Class.forName(main, true, this)
        //     ├─ defineClass(main)   → 解析直接父类/接口     ⇒ 必须明文(②)
        //     ├─ link(main)          → 字节码校验,可赋值性检查会加载类型 ⇒ 必须明文(③④)
        //     └─ initialize(main)    → 桥类 <clinit> 定义载荷  ⇒ 从这里往后都无所谓了
        //
        // 也就是说:**在载荷被定义出来之前**,凡是 JVM 会主动去解析的东西都得明文;
        // 之后的一切(方法体里真正执行到的引用、字段读写、反射)都是懒解析的,
        // 加密没有任何问题。
        final Map<String, byte[]> classesToEncrypt = new LinkedHashMap<String, byte[]>();
        for (Map.Entry<String, byte[]> e : allClasses.entrySet()) {
            if (!plainNames.contains(e.getKey())) {
                classesToEncrypt.put(e.getKey(), e.getValue());
            }
        }


        // ---------- 3. 若存在 plugin.yml,把 main 指向加壳入口 ----------
        //    Bukkit/Paper 是用它来决定主类的;真正的主类已经加密进载荷,
        //    能对外暴露的只能是这个加载器入口。
        // plugin.yml 的 main 保持原样 —— 它已经是混淆后的正确名字,
        // 而且我们上面就是照着它去找主类的。这里不再做任何改写。
        if (resources.containsKey("plugin.yml")) {
            System.out.println("plugin.yml   : main = " + declaredMain + " (未改动)");
        }

        // ---------- 4. 写输出 JAR ----------
        //
        // 反篡改:对每个「非 .class、非 META-INF/」条目记下 SHA-256,最后把整表签名。
        // 为什么是这两类,见 AhxSignature 的长注释 —— 说白了是 Paper 会重写
        // 每个类文件的字节,而且会重写 MANIFEST.MF,拿它们做基准必然误报。
        final Map<String, byte[]> signedEntries = new java.util.TreeMap<String, byte[]>();
        FileOutputStream fos = new FileOutputStream(outJar);
        ZipOutputStream out = new ZipOutputStream(fos);
        try {
            final Manifest manifest = new Manifest();
            manifest.getMainAttributes().put(Attributes.Name.MANIFEST_VERSION, "1.0");
            manifest.getMainAttributes().put(Attributes.Name.MAIN_CLASS, declaredMain);
            out.putNextEntry(new ZipEntry("META-INF/MANIFEST.MF"));
            manifest.write(out);
            out.closeEntry();

            for (Map.Entry<String, byte[]> e : injected.entrySet()) {
                out.putNextEntry(new ZipEntry(e.getKey()));
                out.write(e.getValue());
                out.closeEntry();
                if (isSignedEntry(e.getKey())) {
                    signedEntries.put(e.getKey(), e.getValue());
                }
            }
            for (Map.Entry<String, byte[]> e : resources.entrySet()) {
                out.putNextEntry(new ZipEntry(e.getKey()));
                out.write(e.getValue());
                out.closeEntry();
                if (isSignedEntry(e.getKey())) {
                    signedEntries.put(e.getKey(), e.getValue());
                }
            }

            final byte[] payload = buildPayload(classesToEncrypt, key, random);
            out.putNextEntry(new ZipEntry(PAYLOAD_ENTRY));
            out.write(payload);
            out.closeEntry();
            signedEntries.put(PAYLOAD_ENTRY, payload);

            if (!signKeyPath.isEmpty()) {
                final PrivateKey signKey = loadPrivateKey(new File(signKeyPath));
                final byte[] sigBlob = buildSignature(signedEntries, signKey);
                out.putNextEntry(new ZipEntry(SIGN_ENTRY));
                out.write(sigBlob);
                out.closeEntry();
                System.out.println("反篡改签名   : " + signedEntries.size()
                                   + " 个文件 -> " + SIGN_ENTRY + " (" + sigBlob.length + " 字节)");
                System.out.println("签名公钥指纹 : " + fingerprint(signKey));
            }
        } finally {
            out.close();
            fos.close();
        }

        System.out.println("注入明文类   : " + injected.size());
        System.out.println("加密类       : " + classesToEncrypt.size());
        if (retargeted > 0) {
            System.out.println("主类父类重定向: " + retargeted + " 个 -> " + bridgeClass);
        }
        System.out.println("跳过(同名注入): " + skipped);
        System.out.println("丢弃(多版本) : " + multiRelease);
        System.out.println("保留资源     : " + resources.size());
        System.out.println("Main-Class   : " + declaredMain);
        System.out.println("输出         : " + outJar);
    }

    // ------------------------------------------------------------------

    /** 读大端 u2 */
    private static int u2(byte[] b, int p) {
        return ((b[p] & 0xFF) << 8) | (b[p + 1] & 0xFF);
    }

    /** 扫一遍常量池,记下每项的 tag 与起始偏移,返回常量池结束偏移。 */
    private static int scanConstantPool(byte[] b, int[] off, int[] tag) throws IOException {
        final int cpCount = u2(b, 8);
        int p = 10;
        for (int i = 1; i < cpCount; i++) {
            off[i] = p;
            final int t = b[p++] & 0xFF;
            tag[i] = t;
            switch (t) {
                case 1: {
                    final int len = u2(b, p);
                    p += 2 + len;
                    break;
                }
                case 7: case 8: case 16: case 19: case 20:
                    p += 2;
                    break;
                case 15:
                    p += 3;
                    break;
                case 3: case 4: case 9: case 10: case 11: case 12: case 17: case 18:
                    p += 4;
                    break;
                case 5: case 6:                 // long/double 各占两个常量槽
                    p += 8;
                    i++;
                    break;
                default:
                    throw new IOException("常量池里出现不认识的 tag " + t + "(索引 " + i + ")");
            }
        }
        return p;
    }

    private static String cpUtf8(byte[] b, int[] off, int[] tag, int index) {
        if (index <= 0 || index >= off.length || tag[index] != 1) {
            return null;
        }
        final int len = u2(b, off[index] + 1);
        return new String(b, off[index] + 3, len, StandardCharsets.UTF_8);
    }

    private static String cpClassName(byte[] b, int[] off, int[] tag, int index) {
        if (index <= 0 || index >= off.length || tag[index] != 7) {
            return null;
        }
        return cpUtf8(b, off, tag, u2(b, off[index] + 1));
    }

    /** 读出直接父类与直接超接口的内部名。 */
    private static List<String> directSupertypes(byte[] b) {
        final List<String> out = new ArrayList<String>();
        try {
            final int cpCount = u2(b, 8);
            final int[] off = new int[cpCount];
            final int[] tag = new int[cpCount];
            int q = scanConstantPool(b, off, tag) + 4;   // 跳过 access_flags + this_class
            final String sup = cpClassName(b, off, tag, u2(b, q));
            q += 2;
            if (sup != null) {
                out.add(sup);
            }
            final int ifCount = u2(b, q);
            q += 2;
            for (int i = 0; i < ifCount; i++) {
                final String itf = cpClassName(b, off, tag, u2(b, q));
                q += 2;
                if (itf != null) {
                    out.add(itf);
                }
            }
        } catch (Throwable malformed) {
            // 畸形就当作“没有超类型”—— 少钉几个类不会把东西弄坏。
        }
        return out;
    }

    /**
     * 把 {@code internal} 以及它在本 JAR 内的全部超类型收进 {@code pinned}。
     *
     * <p>不在本 JAR 里的(如 {@code java/lang/Object}、{@code JavaPlugin})由服务端
     * 提供,不用管 —— 递归到这里自然就停了。</p>
     */
    private static void pinSupertypes(Map<String, byte[]> allClasses, String internal,
                                      java.util.Set<String> seen, Map<String, byte[]> pinned) {
        if (internal == null || !seen.add(internal)) {
            return;
        }
        final byte[] data = allClasses.get(internal);
        if (data == null) {
            return;
        }
        pinned.put(internal, data);
        for (String sup : directSupertypes(data)) {
            pinSupertypes(allClasses, sup, seen, pinned);
        }
    }

    /**
     * 钉住主类、主类**直接引用到的全部类型**,以及它们的超类型链。
     *
     * <p>为什么不止钉超类型链:字节码校验器在需要做“可赋值性检查”的地方
     * (返回值 / 字段读写 / 方法调用)<b>会真的把类型加载起来</b>。实测 GrimAC:
     * 只钉超类型链时加载主类就炸在</p>
     *
     * <pre>NoClassDefFoundError: ac/grim/grimac/platform/api/player/xxx</pre>
     *
     * <p>把主类引用到的类型一起钉成明文之后,主类能正常 link + initialize。</p>
     *
     * <p>只做一层:校验器拉进来的类只是被 defineClass,不会被校验,
     * 所以不需要递归它们的引用。递归会把整个插件拉成明文。</p>
     */
    private static void pinVerifierNeed(Map<String, byte[]> allClasses, String mainInternal,
                                        java.util.Set<String> seen, Map<String, byte[]> pinned) {
        final byte[] main = allClasses.get(mainInternal);
        if (main == null) {
            return;
        }
        seen.add(mainInternal);
        pinned.put(mainInternal, main);
        final java.util.Set<String> refs = new java.util.HashSet<String>();
        try {
            refs.addAll(allTypeReferences(main));
        } catch (Throwable malformed) {
            // 畸形就退回“只钉超类型链”—— 少钉几个类不会把东西弄坏,
            // 但这里要保证主类自己一定在 pinned 里(下面 pinSupertypes 会补)。
        }
        refs.addAll(directSupertypes(main));
        for (String ref : refs) {
            if (ref != null && allClasses.containsKey(ref)) {
                pinSupertypes(allClasses, ref, seen, pinned);
            }
        }
    }

    /**
     * 收集 class 字节里出现的<b>全部</b>类型引用。
     *
     * <p>两个来源都要:</p>
     * <ol>
     *   <li>常量池里的 {@code CONSTANT_Class} —— 覆盖 new / checkcast / instanceof /
     *       异常处理器 / 字段与方法引用所属的类 / 类字面量;</li>
     *   <li>所有描述符常量(Utf8)里的 {@code Lxxx;} —— 字段类型、方法参数与返回值。
     *       它们不在 {@code CONSTANT_Class} 里,但校验器做可赋值性检查时同样
     *       要把类加载起来,漏了就是 VerifyError/NoClassDefFoundError。</li>
     * </ol>
     *
     * <p>泛型签名(带 {@code <})一律跳过 —— 它们只出现在 Signature 属性里,
     * 对 JVM 的类型检查无意义,而对应的描述符常量会另外被扫到。</p>
     */
    private static java.util.Set<String> allTypeReferences(byte[] b) throws IOException {
        final java.util.Set<String> out = new java.util.HashSet<String>();
        final int cpCount = u2(b, 8);
        final int[] off = new int[cpCount];
        final int[] tag = new int[cpCount];
        scanConstantPool(b, off, tag);
        for (int i = 1; i < cpCount; i++) {
            if (tag[i] == 7) {
                final String n = cpClassName(b, off, tag, i);
                if (n != null && !n.isEmpty() && n.charAt(0) != '[') {
                    out.add(n);
                }
            } else if (tag[i] == 1) {
                final String s = cpUtf8(b, off, tag, i);
                if (s != null && s.indexOf('<') < 0) {
                    addTypesInDescriptor(s, out);
                }
            }
        }
        return out;
    }

    /** 从描述符里挑出 {@code Lxxx;} 形式的类型(转成内部名)。 */
    private static void addTypesInDescriptor(String s, java.util.Set<String> out) {
        final int n = s.length();
        for (int i = 0; i < n; i++) {
            if (s.charAt(i) != 'L') {
                continue;
            }
            int j = i + 1;
            while (j < n && s.charAt(j) != ';' && s.charAt(j) != '(' && s.charAt(j) != ')') {
                ++j;
            }
            if (j >= n || s.charAt(j) != ';') {
                i = j;
                continue;
            }
            final String t = s.substring(i + 1, j);
            if (!t.isEmpty() && isPlausibleInternalName(t)) {
                out.add(t);
            }
            i = j;
        }
    }

    /** 粗筛:像不像一个内部名(避免把描述符碎片当成类名钉进去)。 */
    private static boolean isPlausibleInternalName(String t) {
        for (int i = 0; i < t.length(); i++) {
            final char c = t.charAt(i);
            if (!(Character.isJavaIdentifierPart(c) || c == '/' || c == '$' || c == '-')) {
                return false;
            }
        }
        return true;
    }

    /**
     * 把 class 字节里的父类换成 {@code newSuper},并把构造器里对<b>原父类</b>
     * 的 {@code invokespecial <init>} 一并改指过去。
     *
     * <p>为什么必须同时改构造器:JVM 要求 {@code <init>} 里的
     * {@code invokespecial} 只能调当前类的<b>直接父类</b>。只改 super_class
     * 而不改这个调用,一加载就是</p>
     *
     * <pre>VerifyError: Bad &lt;init&gt; method call</pre>
     *
     * <p>实现上只动常量池:追加一个 CONSTANT_Class 指向新父类,然后把
     * super_class 和那几个 Methodref 的 class_index 都改成它 ——
     * 方法体里的指令一个字节都不用碰。</p>
     */
    private static byte[] retargetSuperclass(byte[] b, String newSuper) throws IOException {
        final int cpCount = u2(b, 8);
        final int[] off = new int[cpCount];
        final int[] tag = new int[cpCount];
        final int p = scanConstantPool(b, off, tag);
        final int superOff = p + 4;                     // 跳过 access_flags 与 this_class
        final int oldSuperIdx = u2(b, superOff);
        if (tag[oldSuperIdx] != 7) {
            throw new IOException("super_class 不是 CONSTANT_Class");
        }
        final int oldSuperNameIdx = u2(b, off[oldSuperIdx] + 1);
        final int oldLen = u2(b, off[oldSuperNameIdx] + 1);
        final String oldSuper = new String(b, off[oldSuperNameIdx] + 3, oldLen,
                                           StandardCharsets.UTF_8);

        // 找出所有 class=<旧父类> 且名字是 <init> 的 Methodref
        final List<Integer> initRefs = new ArrayList<Integer>();
        for (int i = 1; i < cpCount; i++) {
            if (tag[i] != 10) {                         // 只看 CONSTANT_Methodref
                continue;
            }
            final int ci = u2(b, off[i] + 1);
            final int nti = u2(b, off[i] + 3);
            if (tag[ci] != 7 || tag[nti] != 12) {
                continue;
            }
            final int cnIdx = u2(b, off[ci] + 1);
            final int cnLen = u2(b, off[cnIdx] + 1);
            if (!new String(b, off[cnIdx] + 3, cnLen, StandardCharsets.UTF_8).equals(oldSuper)) {
                continue;
            }
            final int mnIdx = u2(b, off[nti] + 1);
            final int mnLen = u2(b, off[mnIdx] + 1);
            if (new String(b, off[mnIdx] + 3, mnLen, StandardCharsets.UTF_8).equals("<init>")) {
                initRefs.add(Integer.valueOf(i));
            }
        }

        // 追加 CONSTANT_Utf8(newSuper) + CONSTANT_Class(上述 Utf8)
        final byte[] nb = newSuper.getBytes(StandardCharsets.UTF_8);
        final ByteArrayOutputStream add = new ByteArrayOutputStream();
        add.write(1);
        add.write((nb.length >>> 8) & 0xFF);
        add.write(nb.length & 0xFF);
        add.write(nb, 0, nb.length);
        add.write(7);
        add.write((cpCount >>> 8) & 0xFF);
        add.write(cpCount & 0xFF);
        final byte[] extra = add.toByteArray();
        final int newClassIdx = cpCount + 1;

        final byte[] out = new byte[b.length + extra.length];
        System.arraycopy(b, 0, out, 0, 10);
        out[8] = (byte) (((cpCount + 2) >>> 8) & 0xFF);
        out[9] = (byte) ((cpCount + 2) & 0xFF);
        System.arraycopy(b, 10, out, 10, p - 10);
        System.arraycopy(extra, 0, out, p, extra.length);
        System.arraycopy(b, p, out, p + extra.length, b.length - p);

        // super_class 在常量池**之后**,要加上追加长度;而 Methodref 在常量池
        // **内部**,位置不变。之前就是在这里把位移加错了地方,导致改到了字符串内容上,
        // 表现为 ClassFormatError: Illegal UTF8 string.
        final int ns = superOff + extra.length;
        out[ns] = (byte) ((newClassIdx >>> 8) & 0xFF);
        out[ns + 1] = (byte) (newClassIdx & 0xFF);
        for (Integer ref : initRefs) {
            final int at = off[ref.intValue()] + 1;
            out[at] = (byte) ((newClassIdx >>> 8) & 0xFF);
            out[at + 1] = (byte) (newClassIdx & 0xFF);
        }
        System.out.println("父类重定向   : " + oldSuper + " -> " + newSuper
                + "(构造器调用改了 " + initRefs.size() + " 处)");
        return out;
    }

    /** 按 AhxPayload 约定的格式构造载荷 */
    private static byte[] buildPayload(Map<String, byte[]> classes,
                                       SecretKeySpec key,
                                       SecureRandom random) throws Exception {
        final List<String> names = new ArrayList<String>(classes.keySet());
        final List<byte[]> blobs = new ArrayList<byte[]>(names.size());

        final ByteArrayOutputStream header = new ByteArrayOutputStream();
        header.write(MAGIC.getBytes(StandardCharsets.US_ASCII));
        writeU32(header, names.size());

        for (String internalName : names) {
            final byte[] plain = classes.get(internalName);

            final byte[] nonce = new byte[NONCE_LENGTH];
            random.nextBytes(nonce);

            final Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, key, new GCMParameterSpec(TAG_BITS, nonce));
            // 与解密端一致:把类名作为附加认证数据
            cipher.updateAAD(internalName.getBytes(StandardCharsets.UTF_8));
            final byte[] sealed = cipher.doFinal(plain);

            final byte[] blob = new byte[NONCE_LENGTH + sealed.length];
            System.arraycopy(nonce, 0, blob, 0, NONCE_LENGTH);
            System.arraycopy(sealed, 0, blob, NONCE_LENGTH, sealed.length);
            blobs.add(blob);

            final byte[] nameBytes = internalName.getBytes(StandardCharsets.UTF_8);
            writeU16(header, nameBytes.length);
            header.write(nameBytes);
            writeU32(header, blob.length);
        }

        final ByteArrayOutputStream payload = new ByteArrayOutputStream();
        payload.write(header.toByteArray());
        for (byte[] blob : blobs) {
            payload.write(blob);
        }
        return payload.toByteArray();
    }

    /** 递归收集目录下所有文件(相对路径作为 key),含非 class 资源 */
    private static void collectFiles(File root, File dir, Map<String, byte[]> out)
            throws IOException {
        final File[] children = dir.listFiles();
        if (children == null) {
            return;
        }
        for (File child : children) {
            if (child.isDirectory()) {
                collectFiles(root, child, out);
            } else {
                final String relative = root.toURI().relativize(child.toURI()).getPath();
                // MANIFEST 由本类重写;注入源(如 NOBF 产物)自带的清单不能带进来,
                // 否则 ZipOutputStream 会抛 "duplicate entry: META-INF/MANIFEST.MF"。
                // 同理丢掉旧签名文件,它们对新内容无效。
                if (isMetadataToRewrite(relative)) {
                    continue;
                }
                out.put(relative, readAll(new FileInputStream(child)));
            }
        }
    }

    /** 由本打包器重建、因而必须从注入源里剔除的元数据 */
    private static boolean isMetadataToRewrite(String name) {
        final String upper = name.toUpperCase(java.util.Locale.ROOT);
        if (!upper.startsWith("META-INF/")) {
            return false;
        }
        if (upper.equals("META-INF/MANIFEST.MF")) {
            return true;
        }
        return upper.endsWith(".SF") || upper.endsWith(".RSA")
                || upper.endsWith(".DSA") || upper.endsWith(".EC");
    }

    /* ------------------------------------------------------------------ */
    /* 反篡改签名                                                          */
    /* ------------------------------------------------------------------ */

    /**
     * 这个条目要不要进签名表。
     *
     * <p>类文件排除在外是因为 Paper 加载插件前会用 ASM 重写每一个类(重映射到
     * 服务端的混淆映射),实测 504 个类里 492 个字节都变了 —— 拿打包时的字节去比
     * 必然误报。{@code META-INF/} 排除是因为 {@code MANIFEST.MF} 会被重写,而且
     * 有些插件加载器对 {@code META-INF} 下的资源读取有限制。</p>
     *
     * <p>剩下的恰好是最要紧的那几样:载荷 {@code p.dat}、原生库 {@code *.so}、
     * {@code plugin.yml} 与全部配置文件。</p>
     */
    /**
     * 哪些条目进签名表。
     *
     * <p><b>只收我们自己产出的两样东西:</b></p>
     * <ol>
     *   <li>{@link #PAYLOAD_ENTRY} —— 加密载荷,整个防护的核心;</li>
     *   <li>原生库 {@code .so / .dll / .dylib} —— 桥与载荷真正执行的代码。</li>
     * </ol>
     *
     * <p>其余一概不收。这个范围是用真实产物换来的:Paper 会重写每个 class 与
     * MANIFEST;而 Fabric 这边**启动器**会重新序列化资源与元数据 —— 实测。
     * {@code fabric.mod.json} 与 {@code icon.png} 都会变字节。拿它们当“未被篡改”
     * 的基准一启动就误报,而且签名表按字母序、只报第一个不符,
     * 排查起来像打地鼠。</p>
     */
    private static boolean isSignedEntry(String name) {
        if (SIGN_ENTRY.equals(name)) {
            return false;
        }
        return PAYLOAD_ENTRY.equals(name)
                || name.endsWith(".so") || name.endsWith(".dll") || name.endsWith(".dylib");
    }

    /**
     * 生成签名块。布局(必须与 {@code top.h3k4.AhxSignature} 严格一致):
     *
     * <pre>
     *   "AHXSIG01" | u16 version | u32 count
     *              | count × { u16 nameLen | name(UTF-8) | 32B sha256 }
     *              | u16 sigLen | signature
     * </pre>
     *
     * <p>被签名的是「从魔数到条目表结束」这一整段 —— 签名段自己不在其中,
     * 否则就成了自指。</p>
     */
    private static byte[] buildSignature(Map<String, byte[]> entries, PrivateKey key)
            throws Exception {
        final MessageDigest sha256 = MessageDigest.getInstance("SHA-256");
        final ByteArrayOutputStream body = new ByteArrayOutputStream();
        body.write(SIG_MAGIC.getBytes(StandardCharsets.US_ASCII));
        writeU16(body, SIG_VERSION);
        writeU32(body, entries.size());
        for (Map.Entry<String, byte[]> e : entries.entrySet()) {
            final byte[] name = e.getKey().getBytes(StandardCharsets.UTF_8);
            writeU16(body, name.length);
            body.write(name);
            body.write(sha256.digest(e.getValue()));
        }
        final byte[] signed = body.toByteArray();

        final Signature signer = Signature.getInstance("SHA256withECDSA");
        signer.initSign(key);
        signer.update(signed);
        final byte[] signature = signer.sign();

        final ByteArrayOutputStream blob = new ByteArrayOutputStream();
        blob.write(signed);
        writeU16(blob, signature.length);
        blob.write(signature);
        return blob.toByteArray();
    }

    /** 读 {@code AhxSign keygen} 写出来的私钥文件(PKCS#8,hex,允许 {@code #} 注释行)。 */
    private static PrivateKey loadPrivateKey(File file) throws Exception {
        if (!file.isFile()) {
            throw new IOException("签名私钥不存在: " + file);
        }
        final StringBuilder hex = new StringBuilder();
        for (String line : Files.readAllLines(file.toPath(), StandardCharsets.UTF_8)) {
            final String trimmed = line.trim();
            if (trimmed.isEmpty() || trimmed.startsWith("#")) {
                continue;
            }
            hex.append(trimmed);
        }
        return KeyFactory.getInstance("EC")
                .generatePrivate(new PKCS8EncodedKeySpec(hexToBytes(hex.toString())));
    }

    /** 公钥指纹:方便肉眼核对"产物里烧的是哪把钥匙"。 */
    private static String fingerprint(PrivateKey key) {
        try {
            // 从私钥推公钥不划算,直接报私钥的指纹没意义;这里报曲线 + 长度即可。
            return key.getAlgorithm() + "/" + key.getFormat() + " "
                   + key.getEncoded().length + "B";
        } catch (Throwable ignored) {
            return "(算不出来)";
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        try {
            final ByteArrayOutputStream out = new ByteArrayOutputStream(1 << 16);
            final byte[] buffer = new byte[8192];
            int n;
            while ((n = in.read(buffer)) != -1) {
                out.write(buffer, 0, n);
            }
            return out.toByteArray();
        } finally {
            in.close();
        }
    }

    private static void writeU16(ByteArrayOutputStream out, int value) {
        out.write((value >>> 8) & 0xFF);
        out.write(value & 0xFF);
    }

    private static void writeU32(ByteArrayOutputStream out, int value) {
        out.write((value >>> 24) & 0xFF);
        out.write((value >>> 16) & 0xFF);
        out.write((value >>> 8) & 0xFF);
        out.write(value & 0xFF);
    }

    private static byte[] hexToBytes(String hex) {
        if (hex == null || hex.length() % 2 != 0) {
            throw new IllegalArgumentException("密钥长度非法");
        }
        final byte[] out = new byte[hex.length() / 2];
        for (int i = 0; i < out.length; i++) {
            out[i] = (byte) Integer.parseInt(hex.substring(i * 2, i * 2 + 2), 16);
        }
        return out;
    }
}
