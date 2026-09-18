/*
 * MIT License
 *
 * Project URL: https://github.com/jar-analyzer/jar-obfuscator
 *
 * 本文件为 AntiHackerX 的 fork 扩展部分,原项目以 MIT 许可发布。
 *
 * https://opensource.org/license/mit
 */

package me.n1ar4.jar.obfuscator.utils;

import me.n1ar4.jar.obfuscator.base.ClassField;
import me.n1ar4.jar.obfuscator.base.MethodReference;
import me.n1ar4.jar.obfuscator.core.ObfEnv;
import me.n1ar4.log.LogManager;
import me.n1ar4.log.Logger;

import java.io.BufferedWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.HashMap;
import java.util.Map;
import java.util.TreeMap;

/**
 * 导出混淆映射表(ProGuard mapping.txt 风格)。
 *
 * <p>上游 jar-obfuscator 内部维护了完整的「原名 -&gt; 新名」映射
 * ({@link ObfEnv#classNameObfMapping}、{@link ObfEnv#methodNameObfMapping}、
 * {@link ObfEnv#fieldNameObfMapping}),但从不落盘。混淆开启
 * {@code obfuscateChars} 后类名会变成 {@code illLIILL1Lii} 这类肉眼无法分辨的
 * 字符串,线上堆栈出现这类名字时无法定位到原始代码。</p>
 *
 * <p>本工具在流程结束时把映射导出到输出 JAR 同目录的
 * {@code <输出jar名>.mapping.txt},用于混淆后反查原始类 / 方法 / 字段。</p>
 *
 * <p><b>注意</b>:映射文件包含原始类名与方法签名,属于敏感产物,
 * 不要随加壳结果一起分发。</p>
 */
public final class MappingWriter {

    private static final Logger logger = LogManager.getLogger();

    /** 混淆后类名 -&gt; 原始类名(在 {@link #write} 开始时构建) */
    private static final Map<String, String> REVERSE_CLASS_MAP = new HashMap<>();

    private MappingWriter() {
    }

    /**
     * 把「混淆后的类名」还原为「原始类名」;找不到时原样返回。
     */
    private static String originalClassName(String obfuscatedName) {
        String original = REVERSE_CLASS_MAP.get(obfuscatedName);
        String name = (original == null) ? obfuscatedName : original;
        return name.replace('/', '.');
    }

    /**
     * 导出映射文件。
     *
     * @param outJar 输出 JAR 的路径(可为相对路径,与 {@code DirUtil.zip} 一致)
     */
    public static void write(String outJar) {
        if (outJar == null || outJar.isEmpty()) {
            return;
        }
        Path mappingPath = Paths.get(outJar + ".mapping.txt");
        int classCount = 0;
        int fieldCount = 0;
        int methodCount = 0;

        // 构建反查表:混淆后内部名 -&gt; 原始内部名(供字段/方法还原归属类)
        REVERSE_CLASS_MAP.clear();
        for (Map.Entry<String, String> e : ObfEnv.classNameObfMapping.entrySet()) {
            REVERSE_CLASS_MAP.put(e.getValue(), e.getKey());
        }

        try (BufferedWriter writer = Files.newBufferedWriter(
                mappingPath, StandardCharsets.UTF_8)) {

            writer.write("# jar-obfuscator mapping (AntiHackerX fork)\n");
            writer.write("# 格式: 原始名 -> 混淆后名\n");
            writer.write("# \u26a0 本文件含原始符号信息,请勿随加壳产物分发\n\n");

            writer.write("# ===== 类 =====\n");
            // TreeMap 保证输出稳定有序,便于人工查找与 diff
            Map<String, String> classes = new TreeMap<>(ObfEnv.classNameObfMapping);
            for (Map.Entry<String, String> entry : classes.entrySet()) {
                String from = entry.getKey().replace('/', '.');
                String to = entry.getValue().replace('/', '.');
                if (from.equals(to)) {
                    continue;
                }
                writer.write(from + " -> " + to + "\n");
                classCount++;
            }

            writer.write("\n# ===== 字段 =====\n");
            // 字段/方法的归属类在上游数据里已经是混淆后的名字,这里用反查表
            // 还原成原始类名,使输出与 ProGuard 风格一致、可直接按原类名查找。
            for (Map.Entry<ClassField, ClassField> entry
                    : ObfEnv.fieldNameObfMapping.entrySet()) {
                ClassField oldField = entry.getKey();
                ClassField newField = entry.getValue();
                if (oldField.getFieldName().equals(newField.getFieldName())) {
                    continue;
                }
                writer.write(originalClassName(oldField.getClassName())
                        + "." + oldField.getFieldName()
                        + " -> " + newField.getFieldName() + "\n");
                fieldCount++;
            }

            writer.write("\n# ===== 方法 =====\n");
            for (Map.Entry<MethodReference.Handle, MethodReference.Handle> entry
                    : ObfEnv.methodNameObfMapping.entrySet()) {
                MethodReference.Handle oldMethod = entry.getKey();
                MethodReference.Handle newMethod = entry.getValue();
                if (oldMethod.getName().equals(newMethod.getName())) {
                    continue;
                }
                writer.write(originalClassName(oldMethod.getClassReference().getName())
                        + " " + oldMethod.getName() + oldMethod.getDesc()
                        + " -> " + newMethod.getName() + "\n");
                methodCount++;
            }

            writer.write("\n# 统计: 类 " + classCount
                    + " / 字段 " + fieldCount
                    + " / 方法 " + methodCount + "\n");
            logger.info("mapping file generated: {}", mappingPath.toAbsolutePath());
        } catch (Exception ex) {
            // 映射导出失败不应中断加壳主流程
            logger.error("write mapping file failed: {}", ex.toString());
        }
    }
}
