/*
 * AntiHackerX 签名密钥工具 —— 单文件源码启动,不需要编译产物。
 *
 * SPDX-License-Identifier: MIT
 * 许可证全文见仓库中的 LICENSE。
 *
 * 用法:
 *   java AhxSign.java keygen <输出目录>
 *
 * 会在 <输出目录> 下写两个文件:
 *   ahx-sign.key  私钥(PKCS#8,hex)。**绝不要随产物分发**。
 *   ahx-sign.pub  公钥(X.509 SubjectPublicKeyInfo,hex)。打包器读它,并把它
 *                 以字面量形式塞进入口类,最终落进原生代码。
 *
 * 为什么每份产物单独一对密钥,而不是全工具共用一把:
 *   共用就意味着这把私钥要出现在用户机器的安装目录里(甚至打进 AntiHackerX
 *   本体),谁都能拿去伪造签名 —— 等于没有签名。每份产物现生成现用、私钥用完
 *   即删,攻击者手上永远只有公钥。
 */
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.spec.ECGenParameterSpec;

public final class AhxSign {

    /** 曲线固定 P-256:够安全,而且 JDK 8+ 全都自带,运行时不用带任何依赖。 */
    private static final String CURVE = "secp256r1";

    public static final String KEY_FILE = "ahx-sign.key";
    public static final String PUB_FILE = "ahx-sign.pub";

    private AhxSign() {
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 2 || !"keygen".equals(args[0])) {
            System.err.println("用法: java AhxSign.java keygen <输出目录>");
            System.exit(1);
        }
        final File dir = new File(args[1]);
        if (!dir.isDirectory() && !dir.mkdirs()) {
            System.err.println("无法创建目录: " + dir);
            System.exit(2);
        }

        final KeyPairGenerator generator = KeyPairGenerator.getInstance("EC");
        generator.initialize(new ECGenParameterSpec(CURVE));
        final KeyPair pair = generator.generateKeyPair();

        final File keyFile = new File(dir, KEY_FILE);
        final File pubFile = new File(dir, PUB_FILE);
        write(keyFile, "# AntiHackerX 签名私钥(PKCS#8, hex)\n"
                       + "# 打包完就删,绝不要随产物分发 —— 拿到它就能伪造签名。\n"
                       + toHex(pair.getPrivate().getEncoded()) + "\n");
        write(pubFile, toHex(pair.getPublic().getEncoded()) + "\n");

        // 私钥文件只有属主可读
        try {
            keyFile.setReadable(false, false);
            keyFile.setReadable(true, true);
            keyFile.setWritable(false, false);
            keyFile.setWritable(true, true);
        } catch (SecurityException ignored) {
            // 尽力而为
        }

        System.out.println("签名私钥(用完即删) : " + keyFile.getAbsolutePath());
        System.out.println("签名公钥(烧进产物) : " + pubFile.getAbsolutePath());
        System.out.println("公钥指纹(SHA-256)  : " + fingerprint(pair.getPublic().getEncoded()));
    }

    private static void write(File file, String text) throws Exception {
        final FileOutputStream out = new FileOutputStream(file);
        try {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        } finally {
            out.close();
        }
    }

    /** 公钥指纹,方便肉眼核对"产物里烧的是哪把钥匙"。 */
    public static String fingerprint(byte[] spki) {
        try {
            final byte[] digest =
                    java.security.MessageDigest.getInstance("SHA-256").digest(spki);
            final StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 8; i++) {
                if (i > 0) {
                    sb.append(':');
                }
                sb.append(String.format("%02x", digest[i] & 0xFF));
            }
            return sb.toString();
        } catch (Throwable ignored) {
            return "(算不出来)";
        }
    }

    public static String toHex(byte[] bytes) {
        final StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (int i = 0; i < bytes.length; i++) {
            sb.append(Character.forDigit((bytes[i] >> 4) & 0xF, 16));
            sb.append(Character.forDigit(bytes[i] & 0xF, 16));
        }
        return sb.toString();
    }
}
