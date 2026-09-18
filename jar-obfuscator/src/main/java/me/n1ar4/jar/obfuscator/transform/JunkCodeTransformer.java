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

import me.n1ar4.jar.obfuscator.asm.JunkCodeVisitor;
import me.n1ar4.jar.obfuscator.config.BaseConfig;
import me.n1ar4.jar.obfuscator.core.ObfEnv;
import me.n1ar4.log.LogManager;
import me.n1ar4.log.Logger;
import org.objectweb.asm.MethodTooLargeException;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Map;

@SuppressWarnings("all")
public class JunkCodeTransformer {
    private static final Logger logger = LogManager.getLogger();

    public static void transform(BaseConfig config) {
        for (Map.Entry<String, String> entry : ObfEnv.classNameObfMapping.entrySet()) {
            String newName = entry.getValue();
            Path newClassPath = TransformerUtil.classPath(newName);

            logger.debug("花指令混淆进行中 {} -> {}", newClassPath.toAbsolutePath());

            if (!Files.exists(newClassPath)) {
                logger.debug("class not exist: {}", newClassPath.toString());
                continue;
            }
            try {
                TransformerUtil.transformClassPreservingFrames(newClassPath,
                        classWriter -> new JunkCodeVisitor(classWriter, config));
            } catch (MethodTooLargeException ex) {
                // 加了花指令后方法超出 JVM 的 64KB 单方法上限。
                // 跳过这个类的花指令即可 —— 其他混淆(改名/字符串加密)仍然生效,
                // 没有理由为此中止整包。
                logger.warn("跳过花指令(方法超 64KB 上限):{} - {}",
                        newName, ex.getMessage());
            } catch (Exception ex) {
                logger.error("transform error: {}", ex.toString());
                throw new IllegalStateException("junk code transform failed: " + newName, ex);
            }
        }
        logger.info("junk code transform finish");
    }
}
