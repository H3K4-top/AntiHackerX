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

package me.n1ar4.jar.obfuscator.loader;

import me.n1ar4.jar.obfuscator.core.ObfEnv;
import me.n1ar4.log.LogManager;
import me.n1ar4.log.Logger;
import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassWriter;

import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

public class CustomClassWriter extends ClassWriter {
    private static final String defaultRet = "java/lang/Object";
    private static final Logger logger = LogManager.getLogger();
    private final ClassLoader classLoader;

    /** 混淆名 -> 原始名。{@link #toOriginalInternalName} 原本是 O(n) 线性扫描,
     *  而 getCommonSuperClass 会被调用成千上万次,不缓存会拖死混淆。 */
    private static final Map<String, String> OBF2ORIG = new ConcurrentHashMap<>();
    private static final Set<String> OBF2ORIG_MISS = ConcurrentHashMap.newKeySet();

    /** 反向映射建缓存时的表大小。映射表还在增长的话,缓存整体作废重来。 */
    private static volatile int cachedMappingSize = -1;

    /** 退化警告只报一次 —— 不然一个类能刷几百行。 */
    private static volatile boolean warnedFallback = false;

    public CustomClassWriter(ClassReader classReader, int flags, ClassLoader classLoader) {
        super(classReader, flags);
        this.classLoader = classLoader;
    }

    @Override
    protected String getCommonSuperClass(final String type1, final String type2) {
        if (type1 == null || type2 == null) {
            return defaultRet;
        }
        if (type1.trim().isEmpty() || type2.trim().isEmpty()) {
            return defaultRet;
        }
        if (type1.equals(type2)) {
            return type1;
        }

        final Class<?> c;
        try {
            c = Class.forName(toOriginalInternalName(type1).replace('/', '.'),
                              false, classLoader);
        } catch (Throwable e) {
            warnFallback(type1, type2, e);
            return defaultRet;
        }

        // 只有 c 就是 Object 时才能跳过加载 d —— 这时答案必然是 Object。
        // (别拿 final 做文章:final 只说明 c 没有子类,但 d 完全可能是 c 的**父类**,
        //  比如 JsonNull(final) 和 JsonElement,公共父类是后者而不是 Object。
        //  这么跳过会写出错误栈帧,踩过。)
        if (c == Object.class) {
            return defaultRet;
        }

        final Class<?> d;
        try {
            d = Class.forName(toOriginalInternalName(type2).replace('/', '.'),
                              false, classLoader);
        } catch (Throwable e) {
            warnFallback(type1, type2, e);
            return defaultRet;
        }

        if (c.isAssignableFrom(d)) {
            return type1;
        }
        if (d.isAssignableFrom(c)) {
            return type2;
        }
        if (c.isInterface() || d.isInterface()) {
            return defaultRet;
        }
        Class<?> walk = c;
        while ((walk = walk.getSuperclass()) != null && !walk.isAssignableFrom(d)) {
            // 顺着 c 往上找第一个能容纳 d 的祖先
        }
        return walk == null
                ? defaultRet
                : toObfuscatedInternalName(walk.getName().replace('.', '/'));
    }

    /**
     * 退化警告。
     *
     * <p>只报一次 —— 不然一个类能刷几百行。注意**大多数退化是无害的**
     * (比如另一个类型是 final 的 JDK 类,公共父类本来就只能是 Object),
     * 真正危险的只有“两个类型都该精确解析却解析不出来”的那一小撮。</p>
     */
    private static void warnFallback(String type1, String type2, Throwable e) {
        if (!warnedFallback) {
            warnedFallback = true;
            logger.warn("解析公共父类失败({} / {}),退化为 Object。"
                            + "多数情况无害;但若产物运行时出现 "
                            + "VerifyError: Bad type on operand stack,"
                            + "请把服务端的 libraries 加入「额外类路径」: {}",
                    type1, type2, e.toString());
        } else {
            logger.debug("common super class fallback: {}", e.toString());
        }
    }

    private String toOriginalInternalName(String name) {
        final int size = ObfEnv.classNameObfMapping.size();
        if (size != cachedMappingSize) {
            // 映射表还在变(比如刚扫完类名),旧缓存不可信
            OBF2ORIG.clear();
            OBF2ORIG_MISS.clear();
            cachedMappingSize = size;
        }
        String hit = OBF2ORIG.get(name);
        if (hit != null) {
            return hit;
        }
        if (!OBF2ORIG_MISS.contains(name)) {
            for (Map.Entry<String, String> entry : ObfEnv.classNameObfMapping.entrySet()) {
                if (entry.getValue().equals(name)) {
                    OBF2ORIG.put(name, entry.getKey());
                    return entry.getKey();
                }
            }
            OBF2ORIG_MISS.add(name);
        }
        // 表里没有:可能是内部类。映射表对内部类的处理是
        // “外层类的映射 + $后缀”,用外层类递归一次就能拼回来。
        // 少了这一步,任何引用了嵌套类的栈帧合流都会脆生生地退化成 Object。
        final int dollar = name.indexOf('$');
        if (dollar > 0) {
            final String outer = name.substring(0, dollar);
            final String outerOriginal = toOriginalInternalName(outer);
            if (!outerOriginal.equals(outer)) {
                final String candidate = outerOriginal + name.substring(dollar);
                OBF2ORIG.put(name, candidate);
                return candidate;
            }
        }
        return name;
    }

    private String toObfuscatedInternalName(String name) {
        final String mapped = ObfEnv.classNameObfMapping.get(name);
        if (mapped != null) {
            return mapped;
        }
        // 同上:内部类不在表里,得用外层类的映射推导
        final int dollar = name.indexOf('$');
        if (dollar > 0) {
            final String outer = name.substring(0, dollar);
            final String outerMapped = toObfuscatedInternalName(outer);
            if (!outerMapped.equals(outer)) {
                return outerMapped + name.substring(dollar);
            }
        }
        return name;
    }
}
