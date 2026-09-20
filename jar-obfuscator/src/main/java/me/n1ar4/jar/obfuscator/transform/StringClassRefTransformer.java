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

package me.n1ar4.jar.obfuscator.transform;

import me.n1ar4.jar.obfuscator.Const;
import me.n1ar4.jar.obfuscator.core.ObfEnv;
import me.n1ar4.log.LogManager;
import me.n1ar4.log.Logger;
import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassVisitor;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.FieldVisitor;
import org.objectweb.asm.MethodVisitor;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * 改写"字符串常量里的类名",等价于 ProGuard 的 {@code -adaptclassstrings}。
 *
 * <h3>为什么必须有这一步</h3>
 *
 * <p>改名有个天然的漏洞:类名不止出现在字节码的符号引用里,还会以<b>字符串
 * 常量</b>的形式出现 —— 只要它被用于反射。典型写法:</p>
 *
 * <pre>
 *   Class.forName("com.example.MyImpl").newInstance();
 * </pre>
 *
 * <p>符号引用由 {@link ClassNameTransformer} 改掉了,字符串常量却没人动,
 * 于是运行期按<b>老名字</b>去找一个已经改名的类 → {@code ClassNotFoundException}。</p>
 *
 * <p>这不是理论问题,真实踩到过:Simple Voice Chat 的
 * {@code de.maxhenkel.voicechat.intercompatibility.CrossSideManager} 用
 * {@code Class.forName("...ClientCrossSideManager")} 挑客户端实现,加壳后在
 * {@code VoicechatClient.initializeClient} 里直接崩,栈顶就是那句 forName。
 * 该类全 jar 只有这一处,但漏掉它就整包跑不起来。</p>
 *
 * <h3>做法与边界</h3>
 *
 * <p>只在字符串<b>完整等于</b>某个被改名的类名时才替换(点号形式与内部名形式
 * 都认)。不做"前缀匹配"式的宽松替换 —— {@code "com.example"} 这种包名字符串
 * 可能出现在日志文案、资源路径、权限名里,替换了就是另一个 bug。</p>
 *
 * <p>必须跑在 {@link StringTransformer}(字符串加密)与
 * {@link StringArrayTransformer}(字符串提取)<b>之前</b>:那两者会把字面量换成
 * 密文,之后再想读明文就来不及了。</p>
 */
public class StringClassRefTransformer {
    private static final Logger logger = LogManager.getLogger();

    private StringClassRefTransformer() {
    }

    public static void transform() {
        final Map<String, String> dotted = new HashMap<String, String>();
        final Map<String, String> slashed = new HashMap<String, String>();
        for (Map.Entry<String, String> entry : ObfEnv.classNameObfMapping.entrySet()) {
            final String from = entry.getKey();
            final String to = entry.getValue();
            // 被黑名单保住的类是 identity 映射(原名 -> 原名),不是真改名
            if (from == null || to == null || from.equals(to)) {
                continue;
            }
            slashed.put(from, to);
            dotted.put(from.replace('/', '.'), to.replace('/', '.'));
        }
        if (dotted.isEmpty()) {
            logger.debug("no renamed class, skip class-name string adapt");
            return;
        }

        int total = 0;
        final Set<String> visited = new HashSet<String>();
        // 注意遍历的是全部类(含被保住不改名的那些):它们里面同样可能写着
        // 别的被改名的类的名字。
        for (Map.Entry<String, String> entry : ObfEnv.classNameObfMapping.entrySet()) {
            final String name = entry.getValue();
            if (!visited.add(name)) {
                continue;
            }
            final Path classPath = TransformerUtil.classPath(name);
            if (!Files.exists(classPath)) {
                continue;
            }
            try {
                total += adapt(classPath, dotted, slashed);
            } catch (Exception ex) {
                throw new IllegalStateException("adapt class-name string failed: " + name, ex);
            }
        }
        logger.info("adapt class-name strings finish: {} change(s)", total);
    }

    private static int adapt(Path classPath, Map<String, String> dotted,
                             Map<String, String> slashed)
            throws IOException {
        final ClassReader classReader = new ClassReader(Files.readAllBytes(classPath));
        // 故意**不**把 classReader 传给 ClassWriter:那样 ASM 会整块复制原常量池,
        // 被替换掉的老类名字符串会以"无人引用的死条目"形式留在产物里 ——
        // 功能上无害,但等于给逆向留了条线索,而且会让"到底改没改"难以核实。
        // 不传则常量池按访问到的东西重建,干净。
        //
        // 只换常量池里的字面量,不动任何指令结构 —— 栈帧不会变,也不需要解析
        // 继承链,所以 COMPUTE_MAXS 足够(不用 COMPUTE_FRAMES)。
        final ClassWriter classWriter = new ClassWriter(ClassWriter.COMPUTE_MAXS);
        final int[] changed = new int[1];
        classReader.accept(new ClassVisitor(Const.ASMVersion, classWriter) {
            @Override
            public MethodVisitor visitMethod(int access, String name, String descriptor,
                                            String signature, String[] exceptions) {
                MethodVisitor mv = super.visitMethod(access, name, descriptor, signature, exceptions);
                if (mv == null) {
                    return null;
                }
                return new MethodVisitor(Const.ASMVersion, mv) {
                    @Override
                    public void visitLdcInsn(Object value) {
                        if (value instanceof String) {
                            String mapped = lookup((String) value, dotted, slashed);
                            if (mapped != null) {
                                value = mapped;
                                changed[0]++;
                            }
                        }
                        super.visitLdcInsn(value);
                    }
                };
            }

            @Override
            public FieldVisitor visitField(int access, String name, String descriptor,
                                           String signature, Object value) {
                // 静态常量的初始值也走常量池(`ConstantValue` 属性)
                if (value instanceof String) {
                    String mapped = lookup((String) value, dotted, slashed);
                    if (mapped != null) {
                        value = mapped;
                        changed[0]++;
                    }
                }
                return super.visitField(access, name, descriptor, signature, value);
            }
        }, 0);
        if (changed[0] > 0) {
            TransformerUtil.writeAtomically(classPath, classWriter.toByteArray());
        }
        return changed[0];
    }

    /** 精确匹配:点号形式(源码里怎么写)和内部名形式(内部名怎么写)都试一遍。 */
    private static String lookup(String value, Map<String, String> dotted,
                                 Map<String, String> slashed) {
        String mapped = dotted.get(value);
        if (mapped != null) {
            return mapped;
        }
        return slashed.get(value);
    }
}
