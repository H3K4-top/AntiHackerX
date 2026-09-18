/*
 * AntiHackerX 运行时 —— 以 MIT 许可证提供,便于嵌入你自己的程序。
 *
 * SPDX-License-Identifier: MIT
 * 许可证全文见仓库中的 LICENSE。
 */
package top.h3k4;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.GeneralSecurityException;
import java.util.HashMap;
import java.util.Map;

import javax.crypto.Cipher;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * 加密载荷读取器。
 *
 * <p>打包时把类文件的字节用 AES-256-GCM 加密,按下面的格式写进 JAR 内的单一资源
 * {@value #PAYLOAD_RESOURCE}。运行时本类负责按类名取出并解密。</p>
 *
 * <p><b>blob 格式(全大端):</b></p>
 * <pre>
 *   magic        8 字节  "AHXPAY01"
 *   entryCount   u32
 *   --- 索引区(entryCount 条) ---
 *     nameLen    u16
 *     nameBytes  nameLen 字节(UTF-8,内部类名,如 com/example/Foo)
 *     dataLen    u32
 *   --- 数据区(entryCount 条,顺序与索引区一致) ---
 *     data       dataLen 字节 = nonce(12) || 密文 || tag(16)
 * </pre>
 *
 * <p>用索引区把条目偏移先算出来,取单个类时无需解密整包。</p>
 *
 * <p><b>安全边界:</b>密钥不明文存储 —— 由生成的入口类里的三个随机分片推导
 * (见 {@link AhxRuntime#install}),{@code strings} 扫不出密钥。但这仍属于
 * "提高提取成本"而非密码学强度：能跑代码就能还原密钥。所有纯软件加壳都有这个
 * 固有上限，真正的强度来自与 native 转换、反调试等叠加。</p>
 */
public final class AhxPayload {

    /** 载荷在 JAR 中的资源路径 */
    public static final String PAYLOAD_RESOURCE = "/top/h3k4/p.dat";

    private static final String MAGIC = "AHXPAY01";   // 恰好 8 字节,两端必须一致

    private static final int NONCE_LENGTH = 12;
    private static final int TAG_BITS = 128;

    /** 类名 -> 该条目在数据区中的 (偏移, 长度) */
    private static final class Entry {
        final int offset;
        final int length;

        Entry(int offset, int length) {
            this.offset = offset;
            this.length = length;
        }
    }

    private static byte[] payload;
    private static Map<String, Entry> index;
    private static SecretKeySpec key;

    private AhxPayload() {
    }

    /**
     * 初始化:载入载荷并建立索引。
     *
     * @param aesKey 打包时生成的 AES-256 密钥(32 字节)。
     *               由 {@link AhxRuntime#install} 从三个分片还原后传入。
     */
    public static synchronized void init(byte[] aesKey) {
        if (index != null) {
            return;
        }
        key = new SecretKeySpec(aesKey, "AES");
        payload = readResource();
        index = buildIndex(payload);
    }

    /** 该内部类名是否在加密载荷中 */
    public static boolean contains(String internalName) {
        return index != null && index.containsKey(internalName);
    }

    /** 载荷里所有类的内部名(顺序不保证)。 */
    public static String[] names() {
        if (index == null) {
            return new String[0];
        }
        return index.keySet().toArray(new String[index.size()]);
    }

    /**
     * 取出并解密一个类的字节码。
     *
     * @param internalName 内部类名,如 {@code com/example/Foo}
     * @return 明文 class 字节;不存在或解密失败返回 null
     */
    public static byte[] readClass(String internalName) {
        final Entry entry = (index == null) ? null : index.get(internalName);
        if (entry == null) {
            return null;
        }

        final byte[] nonce = new byte[NONCE_LENGTH];
        System.arraycopy(payload, entry.offset, nonce, 0, NONCE_LENGTH);

        final int cipherLength = entry.length - NONCE_LENGTH;
        if (cipherLength <= 0) {
            return null;
        }

        try {
            final Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(TAG_BITS, nonce));
            // 把类名作为附加认证数据:条目被搬到别的类名下会导致认证失败
            cipher.updateAAD(internalName.getBytes("UTF-8"));
            return cipher.doFinal(payload, entry.offset + NONCE_LENGTH, cipherLength);
        } catch (GeneralSecurityException e) {
            return null;
        } catch (IOException e) {
            return null;
        }
    }

    // ------------------------------------------------------------------
    // 内部实现
    // ------------------------------------------------------------------

    private static byte[] readResource() {
        InputStream in = AhxPayload.class.getResourceAsStream(PAYLOAD_RESOURCE);
        if (in == null) {
            throw new IllegalStateException("缺少加密载荷:" + PAYLOAD_RESOURCE);
        }
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream(1 << 16);
            byte[] buffer = new byte[8192];
            int n;
            while ((n = in.read(buffer)) != -1) {
                out.write(buffer, 0, n);
            }
            return out.toByteArray();
        } catch (IOException e) {
            throw new IllegalStateException("读取加密载荷失败", e);
        } finally {
            try {
                in.close();
            } catch (IOException ignored) {
                // 忽略
            }
        }
    }

    private static Map<String, Entry> buildIndex(byte[] data) {
        final Reader reader = new Reader(data);

        final byte[] magic = new byte[MAGIC.length()];
        reader.read(magic, 0, magic.length);
        if (!MAGIC.equals(new String(magic, java.nio.charset.StandardCharsets.US_ASCII))) {
            throw new IllegalStateException("加密载荷格式不匹配");
        }

        final int count = (int) reader.readU32();
        final Map<String, Entry> map = new HashMap<String, Entry>(Math.max(16, count * 2));

        final String[] names = new String[count];
        final int[] lengths = new int[count];

        for (int i = 0; i < count; i++) {
            final int nameLength = reader.readU16();
            final byte[] nameBytes = new byte[nameLength];
            reader.read(nameBytes, 0, nameLength);
            names[i] = new String(nameBytes, java.nio.charset.StandardCharsets.UTF_8);
            lengths[i] = (int) reader.readU32();
        }

        int offset = reader.position();
        for (int i = 0; i < count; i++) {
            map.put(names[i], new Entry(offset, lengths[i]));
            offset += lengths[i];
        }
        return map;
    }


    /** 极简大端读取器 */
    private static final class Reader {
        private final byte[] data;
        private int pos;

        Reader(byte[] data) {
            this.data = data;
        }

        int position() {
            return pos;
        }

        void read(byte[] target, int off, int len) {
            if (pos + len > data.length) {
                throw new IllegalStateException("加密载荷被截断");
            }
            System.arraycopy(data, pos, target, off, len);
            pos += len;
        }

        int readU16() {
            if (pos + 2 > data.length) {
                throw new IllegalStateException("加密载荷被截断");
            }
            final int v = ((data[pos] & 0xFF) << 8) | (data[pos + 1] & 0xFF);
            pos += 2;
            return v;
        }

        long readU32() {
            if (pos + 4 > data.length) {
                throw new IllegalStateException("加密载荷被截断");
            }
            final long v = ((long) (data[pos] & 0xFF) << 24)
                    | ((data[pos + 1] & 0xFF) << 16)
                    | ((data[pos + 2] & 0xFF) << 8)
                    | (data[pos + 3] & 0xFF);
            pos += 4;
            return v;
        }
    }
}
