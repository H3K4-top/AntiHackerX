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

import me.n1ar4.jar.obfuscator.asm.IntToXorVisitor;
import me.n1ar4.jar.obfuscator.core.ObfEnv;
import me.n1ar4.jar.obfuscator.loader.CustomClassLoader;
import me.n1ar4.log.LogManager;
import me.n1ar4.log.Logger;
import org.objectweb.asm.MethodTooLargeException;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Map;

@SuppressWarnings("all")
public class XORTransformer {
    private static final Logger logger = LogManager.getLogger();

    public static void transform(CustomClassLoader loader) {
        for (Map.Entry<String, String> entry : ObfEnv.classNameObfMapping.entrySet()) {
            String newName = entry.getValue();
            Path newClassPath = TransformerUtil.classPath(newName);

            logger.debug("整数异或混淆进行中 {} -> {}", newClassPath.toAbsolutePath());

            if (!Files.exists(newClassPath)) {
                logger.debug("class not exist: {}", newClassPath.toString());
                continue;
            }
            try {
                TransformerUtil.transformClass(newClassPath, loader, IntToXorVisitor::new);
            } catch (MethodTooLargeException ex) {
                // JVM 硬限制:单个方法不能超过 65535 字节。把每个 int 常量展开成
                // `ldc key; ldc key^x; ixor` 会让巨型 <clinit>(典型是查表初始化的
                // 巨型枚举/常量类,如 packetevents 的 StateTypes)超限。
                //
                // 这不是错误,只是这个类吃不下这项混淆 —— 跳过它,别把整个任务带崩。
                // 安全性:toByteArray() 抛异常时 writeAtomically() 还没执行,
                // 类文件仍保持原样,不会有半成品落盘。
                logger.warn("跳过整数异或(方法超 64KB 上限):{} - {}",
                        newName, ex.getMessage());
            } catch (Exception ex) {
                logger.error("transform error: {}", ex.toString());
                throw new IllegalStateException("xor transform failed: " + newName, ex);
            }
        }
        logger.info("xor transform finish");
    }
}
