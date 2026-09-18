/*
 * MIT License
 *
 * Project URL: https://github.com/jar-analyzer/jar-obfuscator
 *
 * Copyright (c) 2024-2026 4ra1n (https://github.com/4ra1n)
 *
 * This project is distributed under the MIT license.
 *
 * https://opensource.org/license/mit
 */

package me.n1ar4.jar.obfuscator.utils;

import me.n1ar4.jar.obfuscator.base.ClassReference;
import me.n1ar4.jar.obfuscator.base.MethodReference;
import me.n1ar4.jar.obfuscator.core.ObfEnv;
import org.objectweb.asm.Handle;
import org.objectweb.asm.Type;
import org.objectweb.asm.commons.Remapper;
import org.objectweb.asm.commons.SignatureRemapper;
import org.objectweb.asm.signature.SignatureReader;
import org.objectweb.asm.signature.SignatureWriter;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Map;

public class BytecodeRemapUtil {
    private BytecodeRemapUtil() {
    }

    public static String remapClassName(String name) {
        if (name == null) {
            return null;
        }
        return ObfEnv.classNameObfMapping.getOrDefault(name, name);
    }

    /**
     * 按描述符结构重映射其中的类名。
     *
     * <p><b>绝不能用"逐个子串 replace"的写法。</b>那个写法有个很隐蔽的漏洞:
     * 如果一个类<b>不参与改名</b>(枚举、黑名单类等,它在映射表里是 identity),
     * 那么对它全名的 replace 等于什么都没做,紧接着外层类的 replace 又把这个
     * 全名的<b>前缀</b>改掉了。典型后果:</p>
     *
     * <pre>
     *   VectorData$VectorType   (内部枚举,identity)
     *   第1轮 apply "VectorData$VectorType" -> 命中且等于自己,无变化
     *   第2轮 apply "VectorData"            -> 把前缀也换了
     *   => lLiLililLl$VectorType   (新外部类 + 旧内部类,运行期 NoClassDefFoundError)
     * </pre>
     *
     * <p>按描述符结构从左往右扫描就天然没有这个问题:每个 {@code L...;} 只取
     * 一次全名、只查一次映射表。</p>
     */
    public static String remapDesc(String descriptor) {
        if (descriptor == null) {
            return null;
        }
        final int len = descriptor.length();
        StringBuilder sb = null;        // 只有真的需要改动时才分配
        int i = 0;                      // 扫描位置
        int copied = 0;                 // 已经写进 sb 的位置
        while (i < len) {
            if (descriptor.charAt(i) != 'L') {
                i++;                    // ⚠ 不能把这个 i 当成"已复制起点",
                continue;               //   否则尾部(如描述符的 )V)会被丢掉
            }
            final int end = descriptor.indexOf(';', i);
            if (end < 0) {
                break;                  // 畸形描述符:剩下的原样保留
            }
            final String name = descriptor.substring(i + 1, end);
            final String mapped = remapClassName(name);
            if (!mapped.equals(name)) {
                if (sb == null) {
                    sb = new StringBuilder(len);
                }
                sb.append(descriptor, copied, i);
                sb.append('L').append(mapped).append(';');
                copied = end + 1;
            }
            i = end + 1;
        }
        if (sb == null) {
            return descriptor;
        }
        sb.append(descriptor, copied, len);
        return sb.toString();
    }

    public static String remapSignature(String signature) {
        if (signature == null) {
            return null;
        }
        try {
            SignatureReader reader = new SignatureReader(signature);
            SignatureWriter writer = new SignatureWriter();
            reader.accept(new SignatureRemapper(writer, new Remapper() {
                @Override
                public String map(String internalName) {
                    return remapClassName(internalName);
                }
            }));
            return writer.toString();
        } catch (Exception ignored) {
            return remapDesc(signature);
        }
    }

    public static Type remapType(Type type) {
        if (type == null) {
            return null;
        }
        if (type.getSort() == Type.OBJECT) {
            return Type.getObjectType(remapClassName(type.getInternalName()));
        }
        if (type.getSort() == Type.ARRAY || type.getSort() == Type.METHOD) {
            return Type.getType(remapDesc(type.getDescriptor()));
        }
        return type;
    }

    public static Handle remapHandle(Handle handle) {
        if (handle == null) {
            return null;
        }
        return new Handle(
                handle.getTag(),
                remapClassName(handle.getOwner()),
                handle.getName(),
                remapDesc(handle.getDesc()),
                handle.isInterface());
    }

    public static String remapAnnotationElementName(String annotationOwner, String elementName) {
        if (annotationOwner == null || elementName == null) {
            return elementName;
        }
        for (Map.Entry<MethodReference.Handle, MethodReference.Handle> entry :
                ObfEnv.methodNameObfMapping.entrySet()) {
            MethodReference.Handle oldHandle = entry.getKey();
            if (oldHandle.getClassReference().equals(new ClassReference.Handle(annotationOwner)) &&
                    oldHandle.getName().equals(elementName)) {
                return entry.getValue().getName();
            }
        }
        return elementName;
    }
}
