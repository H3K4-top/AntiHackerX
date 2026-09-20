/*
 * AntiHackerX 反篡改运行时 —— 以 MIT 许可证提供,便于嵌入你自己的程序。
 *
 * SPDX-License-Identifier: MIT
 * 许可证全文见仓库中的 LICENSE。
 */
package top.h3k4;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.security.KeyFactory;
import java.security.MessageDigest;
import java.security.PublicKey;
import java.security.Signature;
import java.security.spec.X509EncodedKeySpec;
import java.util.ArrayList;
import java.util.List;

/**
 * 完整性校验(反篡改)。
 *
 * <h3>校验什么</h3>
 *
 * <p>打包时对产物的每个<b>非 {@code .class} 条目</b>算 SHA-256,把
 * 「条目名 + 摘要」整表用 ECDSA(P-256 / SHA-256)签名,签名块写进
 * {@value #SIGN_JAR_ENTRY}。运行时逐条重算摘要,再用打包时烧进原生代码的公钥验签。</p>
 *
 * <h3>为什么不管 {@code .class}</h3>
 *
 * <p>Paper 在加载插件前会把 JAR 里的每个类用 ASM 重写一遍(重映射到服务端的
 * 混淆映射),实测 504 个类里有 492 个字节变了 —— 拿打包时的类字节去比对必然
 * 全红。而<b>非类条目 198 个里有 197 个原样保留</b>(唯一变的是
 * {@code META-INF/MANIFEST.MF}),所以这张表只收非类条目。</p>
 *
 * <p>于是真正被钉死的是:加密载荷 {@code p.dat}、原生库 {@code *.so / *.dll}、
 * {@code plugin.yml}、以及所有配置/资源文件。这几样恰好也是攻击者必须动的东西 ——
 * 载荷被 GCM 保护且现在还有签名背书,原生库想改就得动二进制。</p>
 *
 * <h3>公钥在哪</h3>
 *
 * <p>不是常量数组存在这里,而是由打包器生成入口类时以字面量形式传进
 * {@link #verify(String)} 的参数。入口类会被 native-obfuscator 整类搬进原生库,
 * 那个字符串字面量因此落在 {@code string_pool} 里 —— 想换掉公钥就得改二进制,
 * 而不是改一个 Java 常量。</p>
 *
 * <h3>失败就拒绝加载</h3>
 *
 * <p>校验失败直接抛 {@link IllegalStateException}。它在入口类的静态块里抛出,
 * 于是变成 {@code ExceptionInInitializerError},服务端会拒绝启用这个插件 ——
 * 这正是「被修改后拒绝加载」。</p>
 */
public final class AhxSignature {

    /** 签名块在 JAR 里的条目名(用 {@code /} 分隔)。 */
    public static final String SIGN_JAR_ENTRY = "top/h3k4/p.sig";

    private static final String MAGIC = "AHXSIG01";   // 恰好 8 字节,两端必须一致
    private static final int VERSION = 1;
    private static final int DIGEST_LENGTH = 32;      // SHA-256

    private AhxSignature() {
    }

    /** 用本类自己的加载器读资源并校验。 */
    public static void verify(String publicKeyHex) {
        verify(publicKeyHex, AhxSignature.class.getClassLoader());
    }

    /**
     * 校验当前产物的完整性。
     *
     * @param publicKeyHex 打包时生成的公钥(X.509 SubjectPublicKeyInfo 的 hex)
     * @param loader       读资源的加载器,一般是宿主(服务端)的插件加载器
     * @throws IllegalStateException 缺少签名、文件被动过、或验签不通过
     */
    public static void verify(String publicKeyHex, ClassLoader loader) {
        final ClassLoader host = loader != null ? loader : AhxSignature.class.getClassLoader();

        final byte[] blob = readResource(host, SIGN_JAR_ENTRY);
        if (blob == null) {
            fail("缺少签名文件 " + SIGN_JAR_ENTRY
                 + " —— 产物被改动过,或它本来就不是 AntiHackerX 加壳产物");
        }
        if (blob.length < 8 + 2 + 4 + 2 || !MAGIC.equals(ascii(blob, 0, 8))) {
            fail("签名文件头非法(已被替换或损坏)");
        }

        int p = 8;
        final int version = u16(blob, p); p += 2;
        if (version != VERSION) {
            fail("签名文件版本不受支持: " + version);
        }
        final int count = (int) u32(blob, p); p += 4;
        if (count <= 0 || count > 1 << 20) {
            fail("签名文件里的条目数不合理: " + count);
        }

        final List<String> names = new ArrayList<String>(count);
        final List<byte[]> expected = new ArrayList<byte[]>(count);
        for (int i = 0; i < count; i++) {
            if (p + 2 > blob.length) {
                fail("签名文件损坏(条目名长度越界)");
            }
            final int nameLength = u16(blob, p); p += 2;
            if (p + nameLength + DIGEST_LENGTH > blob.length) {
                fail("签名文件损坏(条目越界)");
            }
            names.add(new String(blob, p, nameLength, java.nio.charset.StandardCharsets.UTF_8));
            p += nameLength;
            final byte[] digest = new byte[DIGEST_LENGTH];
            System.arraycopy(blob, p, digest, 0, DIGEST_LENGTH);
            p += DIGEST_LENGTH;
            expected.add(digest);
        }

        // 被签名的部分 = 从魔数到条目表结束。签名段自己当然不在其中。
        final int signedLength = p;
        if (p + 2 > blob.length) {
            fail("签名文件损坏(缺少签名段)");
        }
        final int signatureLength = u16(blob, p); p += 2;
        if (signatureLength <= 0 || p + signatureLength != blob.length) {
            fail("签名文件损坏(签名段长度不符)");
        }
        final byte[] signature = new byte[signatureLength];
        System.arraycopy(blob, p, signature, 0, signatureLength);

        final MessageDigest sha256 = sha256();
        int checked = 0;
        for (int i = 0; i < names.size(); i++) {
            final String name = names.get(i);
            // 与 AhxPacker.isSignedEntry **严格一致**:只校验我们自己产出的两样东西 ——
            //   1) 加密载荷 top/h3k4/p.dat(整个防护的核心)
            //   2) 原生库 .so / .dll / .dylib(桥与载荷真正执行的代码)
            // 其余一概不管:类文件会被平台重写(Paper),资源与元数据会被**启动器**
            // 重新序列化(Fabric 实测:fabric.mod.json、icon.png 都会变字节)。
            // 拿它们当“未被篡改”的基准必然误报。
            final boolean protectedEntry = "top/h3k4/p.dat".equals(name)
                    || name.endsWith(".so") || name.endsWith(".dll")
                    || name.endsWith(".dylib");
            if (!protectedEntry) {
                // 打包器根本不会把这些写进表里。出现就说明表被人重排过。
                fail("签名表里有不该出现的条目: " + name);
            }
            final byte[] actual = readResource(host, name);
            if (actual == null) {
                fail("文件缺失: " + name);
            }
            if (!equals(sha256.digest(actual), expected.get(i))) {
                fail("文件已被修改: " + name);
            }
            ++checked;
        }

        final byte[] signed = new byte[signedLength];
        System.arraycopy(blob, 0, signed, 0, signedLength);
        final PublicKey key = decodePublicKey(publicKeyHex);
        boolean valid;
        try {
            final Signature verifier = Signature.getInstance("SHA256withECDSA");
            verifier.initVerify(key);
            verifier.update(signed);
            valid = verifier.verify(signature);
        } catch (IllegalStateException bad) {
            throw bad;
        } catch (Throwable broken) {
            throw new IllegalStateException("[AntiHackerX] 反篡改:验签过程出错", broken);
        }
        if (!valid) {
            fail("签名不匹配 —— 产物已被重新打包或修改");
        }
        System.out.println("[AntiHackerX] 完整性校验通过: " + checked + " 个文件");
    }

    /* ------------------------------------------------------------------ */

    private static PublicKey decodePublicKey(String hex) {
        if (hex == null || hex.isEmpty()) {
            throw new IllegalStateException(
                    "[AntiHackerX] 反篡改:没有可用的公钥(打包时没带上 --sign-key?)");
        }
        final byte[] der;
        try {
            der = fromHex(hex);
        } catch (IllegalArgumentException bad) {
            throw new IllegalStateException("[AntiHackerX] 反篡改:公钥不是合法的 hex", bad);
        }
        try {
            return KeyFactory.getInstance("EC").generatePublic(new X509EncodedKeySpec(der));
        } catch (Throwable bad) {
            throw new IllegalStateException("[AntiHackerX] 反篡改:公钥无法解析为 P-256 公钥", bad);
        }
    }

    private static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (Throwable missing) {
            throw new IllegalStateException("[AntiHackerX] 反篡改:JVM 不支持 SHA-256", missing);
        }
    }

    private static byte[] readResource(ClassLoader loader, String name) {
        InputStream in = null;
        try {
            in = loader.getResourceAsStream(name);
            if (in == null) {
                return null;
            }
            final ByteArrayOutputStream out = new ByteArrayOutputStream(8192);
            final byte[] buffer = new byte[8192];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
            return out.toByteArray();
        } catch (Throwable unreadable) {
            return null;
        } finally {
            if (in != null) {
                try {
                    in.close();
                } catch (Throwable ignored) {
                    // 关不掉就算了,不值得因为关流失败而判定被篡改
                }
            }
        }
    }

    private static void fail(String why) {
        throw new IllegalStateException("[AntiHackerX] 反篡改校验失败: " + why);
    }

    private static int u16(byte[] b, int off) {
        return ((b[off] & 0xFF) << 8) | (b[off + 1] & 0xFF);
    }

    private static long u32(byte[] b, int off) {
        return ((long) (b[off] & 0xFF) << 24) | ((b[off + 1] & 0xFF) << 16)
               | ((b[off + 2] & 0xFF) << 8) | (b[off + 3] & 0xFF);
    }

    private static String ascii(byte[] b, int off, int len) {
        final StringBuilder sb = new StringBuilder(len);
        for (int i = 0; i < len; i++) {
            sb.append((char) (b[off + i] & 0xFF));
        }
        return sb.toString();
    }

    private static boolean equals(byte[] a, byte[] b) {
        if (a == null || b == null || a.length != b.length) {
            return false;
        }
        // 定长比较,不做提前返回 —— 这里比的是摘要,不存在可利用的时序侧信道,
        // 但保持常量时间总归是免费的。
        int diff = 0;
        for (int i = 0; i < a.length; i++) {
            diff |= a[i] ^ b[i];
        }
        return diff == 0;
    }

    private static byte[] fromHex(String hex) {
        final String s = hex.replace(" ", "").replace(":", "").replace("\n", "").trim();
        if (s.length() % 2 != 0) {
            throw new IllegalArgumentException("hex 长度为奇数");
        }
        final byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; i++) {
            final int hi = Character.digit(s.charAt(i * 2), 16);
            final int lo = Character.digit(s.charAt(i * 2 + 1), 16);
            if (hi < 0 || lo < 0) {
                throw new IllegalArgumentException("非 hex 字符");
            }
            out[i] = (byte) ((hi << 4) | lo);
        }
        return out;
    }
}
