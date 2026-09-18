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

import me.n1ar4.jar.obfuscator.Const;
import me.n1ar4.log.LogManager;
import me.n1ar4.log.Logger;

import java.io.*;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.jar.JarEntry;
import java.util.jar.JarFile;
import java.util.jar.JarInputStream;

public class CustomClassLoader extends ClassLoader {
    private static final Logger logger = LogManager.getLogger();
    // 缓冲处理
    private final Map<String, byte[]> classCache = new ConcurrentHashMap<>();

    private final File baseJar;

    // ------------------------------------------------------------------
    // 额外类路径(插件 JAR 之外的类型)
    // ------------------------------------------------------------------

    /**
     * 由打包器通过 {@code -Dahx.extra.classpath=<a:b:c>} 传入。
     *
     * <p>Bukkit/Paper 插件大量依赖 provided 作用域的服务端 API(以及服务端自带的
     * 第三方库,如 {@code com.google.gson.*}),这些东西不在插件 JAR 里。</p>
     *
     * <p>混淆器需要它们来算类型层次的公共父类。一旦算不出来,ASM 只能退回
     * {@code java/lang/Object} —— 对栈上的操作数这是不可接受的,会写出栈帧与
     * 指令不匹配的字节码,运行时直接
     * {@code VerifyError: Bad type on operand stack}。</p>
     */
    private static final List<File> EXTRA = parseExtraClasspath();

    /** 已打开、进程内复用的额外 JAR。反复 new JarFile 是这里最大的开销。 */
    private static final Map<File, JarFile> EXTRA_JARS = new ConcurrentHashMap<>();

    /** 打不开的额外 JAR,记下来免得每次重试。 */
    private static final Set<File> EXTRA_BAD = ConcurrentHashMap.newKeySet();

    /** 额外类路径里确实没有的类,避免每次 miss 都把几百个 JAR 重扫一遍。 */
    private static final Set<String> EXTRA_MISS = ConcurrentHashMap.newKeySet();

    private static List<File> parseExtraClasspath() {
        final List<File> out = new ArrayList<File>();
        final String raw = System.getProperty("ahx.extra.classpath", "");
        for (String part : raw.split(File.pathSeparator)) {
            final String trimmed = part.trim();
            if (trimmed.isEmpty()) {
                continue;
            }
            final File f = new File(trimmed);
            if (!f.exists()) {
                continue;
            }
            if (f.isFile()) {
                out.add(f);
                continue;
            }
            // 目录:除了目录本身(散装 .class 的情况),还要**递归**找出里面的 JAR。
            // Paper 的 libraries/ 是多层嵌套的
            // (libraries/com/google/code/gson/gson/2.11.0/gson-2.11.0.jar),
            // 只做单层查找必然漏掉 Gson 这类关键库。
            out.add(f);
            try (java.util.stream.Stream<java.nio.file.Path> walk = Files.walk(f.toPath())) {
                walk.filter(Files::isRegularFile)
                    .filter(p -> {
                        final String n = p.getFileName().toString().toLowerCase();
                        return n.endsWith(".jar") || n.endsWith(".zip");
                    })
                    .forEach(p -> out.add(p.toFile()));
            } catch (IOException e) {
                logger.debug("展开额外类路径目录失败: " + f + " " + e.getMessage());
            }
        }
        if (!out.isEmpty()) {
            logger.info("额外类路径: " + out.size() + " 项");
        }
        return out;
    }

    public CustomClassLoader(Path baseJar) {
        this.baseJar = baseJar.toFile();
    }

    @Override
    public Class<?> findClass(String name) throws ClassNotFoundException {
        byte[] classData = loadClassData(name);
        if (classData != null) {
            try {
                return defineClass(name, classData, 0, classData.length);
            } catch (LinkageError e) {
                // 字节码里的父类/接口在本 JAR 之外(provided 依赖)时,HotSpot 在
                // 定义阶段就会抛 NoClassDefFoundError。这不是“找到了类”,
                // 而是“定义不了”,对外应和“没找到”同样处理 ——
                // 否则这个 Error 会穿透调用方所有的 catch (Exception)。
                logger.debug("cannot define class {}: {}", name, e.toString());
                throw new ClassNotFoundException(name, e);
            }
        }
        logger.debug("not found class: " + name);
        throw new ClassNotFoundException(name);
    }

    private byte[] loadClassData(String className) {
        // 先检查缓存
        byte[] cached = classCache.get(className);
        if (cached != null) {
            return cached;
        }
        // 首先尝试从基础JAR加载
        byte[] classData = loadClassFromJar(this.baseJar, className);
        if (classData == null) {
            // 再试额外类路径 —— 服务端 API、服务端自带的第三方库都在那里。
            // 算类型层次时必须能看到它们,否则公共父类会退化成 Object。
            classData = loadFromExtra(className);
        }
        if (classData == null) {
            // 最后一招:混淆器的**工作目录**。
            //
            // 混淆器是原地改 class 文件的,而且对每个类依次跑完所有 transformer。
            // 所以处理到第 N 个类时,它引用的前面那些类**已经改名落盘**了
            // —— 拿新名字到 baseJar 里找必然扑空。这时只能去工作目录取。
            classData = loadFromWorkDir(className);
        }
        if (classData != null) {
            classCache.put(className, classData);
            return classData;
        }
        return null;
    }

    /** 从混淆器的工作目录取一个已经（或正在）被处理的类。 */
    private byte[] loadFromWorkDir(String className) {
        try {
            Path p = Paths.get(Const.TEMP_DIR).resolve(className.replace('.', '/') + ".class");
            if (Files.isRegularFile(p)) {
                return Files.readAllBytes(p);
            }
        } catch (Throwable t) {
            logger.debug("从工作目录读 " + className + " 失败: " + t);
        }
        return null;
    }

    /**
     * 从额外类路径里直接命中一个类。
     *
     * <p>这里刻意不做 {@link #loadClassFromJarFile} 那种“遍历全部条目找嵌套 JAR”
     * 的扫描:额外类路径动辄几百个 JAR,每个 miss 都全扫一遍会让混淆慢到不可用。
     * 只做一次直接命中,miss 结果记进 {@link #EXTRA_MISS}。</p>
     */
    private byte[] loadFromExtra(String className) {
        if (EXTRA.isEmpty() || EXTRA_MISS.contains(className)) {
            return null;
        }
        final String entryName = className.replace('.', '/') + ".class";
        for (File f : EXTRA) {
            try {
                if (f.isDirectory()) {
                    File cf = new File(f, entryName);
                    if (cf.isFile()) {
                        return Files.readAllBytes(cf.toPath());
                    }
                    continue;
                }
                if (EXTRA_BAD.contains(f)) {
                    continue;
                }
                JarFile jar = EXTRA_JARS.get(f);
                if (jar == null) {
                    try {
                        jar = new JarFile(f);
                    } catch (IOException e) {
                        logger.debug("打不开额外 JAR: " + f + " " + e.getMessage());
                        EXTRA_BAD.add(f);
                        continue;
                    }
                    EXTRA_JARS.put(f, jar);
                }
                JarEntry entry = jar.getJarEntry(entryName);
                if (entry != null) {
                    return readEntryBytes(jar, entry);
                }
            } catch (Throwable t) {
                logger.debug("在 " + f + " 里查 " + className + " 出错: " + t);
            }
        }
        EXTRA_MISS.add(className);
        return null;
    }

    private byte[] loadClassFromJar(File jarFile, String className) {
        try (JarFile jar = new JarFile(jarFile)) {
            return loadClassFromJarFile(jar, className);
        } catch (IOException e) {
            logger.error("无法读取 JAR 文件: " + jarFile.getName() + ", 错误: " + e.getMessage());
            return null;
        }
    }

    private byte[] loadClassFromJarFile(JarFile jar, String className) {
        String classPath = className.replace('.', '/') + ".class";
        try {
            JarEntry entry = jar.getJarEntry(classPath);
            if (entry == null) {
                entry = jar.getJarEntry("BOOT-INF/classes/" + classPath);
            }
            if (entry == null) {
                entry = jar.getJarEntry("WEB-INF/classes/" + classPath);
            }
            if (entry != null) {
                return readEntryBytes(jar, entry);
            }
            Enumeration<JarEntry> entries = jar.entries();
            while (entries.hasMoreElements()) {
                JarEntry jarEntry = entries.nextElement();
                if (jarEntry.getName().toLowerCase().endsWith(".jar")) {
                    byte[] classData = loadClassFromNestedJarInMemory(jar, jarEntry, className);
                    if (classData != null) {
                        return classData;
                    }
                }
            }
        } catch (IOException e) {
            logger.error("搜索类文件时出错: " + e.getMessage());
        }

        return null;
    }

    private byte[] loadClassFromNestedJarInMemory(JarFile parentJar, JarEntry nestedJarEntry, String className) {
        try (InputStream nestedJarStream = parentJar.getInputStream(nestedJarEntry);
             ByteArrayOutputStream baos = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int bytesRead;
            while ((bytesRead = nestedJarStream.read(buffer)) != -1) {
                baos.write(buffer, 0, bytesRead);
            }
            try (ByteArrayInputStream bais = new ByteArrayInputStream(baos.toByteArray());
                 JarInputStream jarInputStream = new JarInputStream(bais)) {
                String classPath = className.replace('.', '/') + ".class";
                JarEntry entry;
                while ((entry = jarInputStream.getNextJarEntry()) != null) {
                    if (entry.getName().equals(classPath)) {
                        return readStreamBytes(jarInputStream);
                    }
                    if (entry.getName().toLowerCase().endsWith(".jar")) {
                        byte[] nestedClassData = loadClassFromNestedJarStream(jarInputStream, className);
                        if (nestedClassData != null) {
                            return nestedClassData;
                        }
                    }
                }
            }
        } catch (IOException e) {
            logger.error("处理嵌套 JAR 时出错: " + nestedJarEntry.getName() + ", 错误: " + e.getMessage());
        }

        return null;
    }

    private byte[] loadClassFromNestedJarStream(JarInputStream parentStream, String className) {
        try (ByteArrayOutputStream baos = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int bytesRead;
            while ((bytesRead = parentStream.read(buffer)) != -1) {
                baos.write(buffer, 0, bytesRead);
            }
            try (ByteArrayInputStream bais = new ByteArrayInputStream(baos.toByteArray());
                 JarInputStream jarInputStream = new JarInputStream(bais)) {
                String classPath = className.replace('.', '/') + ".class";
                JarEntry entry;
                while ((entry = jarInputStream.getNextJarEntry()) != null) {
                    if (entry.getName().equals(classPath)) {
                        return readStreamBytes(jarInputStream);
                    }
                }
            }
        } catch (IOException e) {
            logger.error("处理深层嵌套 JAR 时出错: " + e.getMessage());
        }

        return null;
    }

    private byte[] readEntryBytes(JarFile jar, JarEntry entry) throws IOException {
        try (InputStream inputStream = jar.getInputStream(entry)) {
            return readStreamBytes(inputStream);
        }
    }

    private byte[] readStreamBytes(InputStream inputStream) throws IOException {
        try (ByteArrayOutputStream baos = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int bytesRead;
            while ((bytesRead = inputStream.read(buffer)) != -1) {
                baos.write(buffer, 0, bytesRead);
            }
            return baos.toByteArray();
        }
    }
}
