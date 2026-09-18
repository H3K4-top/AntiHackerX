# native-obfuscator 加载器分析报告

## 概述

native-obfuscator 在处理 JAR 文件时会自动生成一个 **Loader 类**来加载本地库,该加载器负责在运行时加载编译好的 .so/.dll/.dylib 文件并注册 JNI 本地方法。

## 加载器类型

### 1. LoaderUnpack (默认模式)

**源文件**: `obfuscator/src/main/java/by/radioegor146/compiletime/LoaderUnpack.java`

#### 特点
- **自动解压模式**: 从 JAR 内部资源中提取本地库到临时文件
- **跨平台检测**: 自动识别操作系统和 CPU 架构
- **临时文件管理**: 使用 `File.createTempFile()` + `deleteOnExit()`

#### 工作流程

```java
static {
    // 1. 检测操作系统
    String osName = System.getProperty("os.name").toLowerCase();
    // linux, windows, macos
    
    // 2. 检测 CPU 架构
    String platform = System.getProperty("os.arch").toLowerCase();
    // x86_64 → x64
    // aarch64 → arm64
    // arm → arm32
    // x86 → x86
    
    // 3. 构建资源路径
    String libFileName = String.format("/%s/%s-%s", 
        LoaderUnpack.class.getName().split("\\.")[0],  // native0
        platformTypeName,                                // x64
        osTypeName);                                     // linux.so
    // 例如: /native0/x64-linux.so
    
    // 4. 创建临时文件
    File libFile = File.createTempFile("lib", null);
    libFile.deleteOnExit();
    
    // 5. 从 JAR 资源提取到临时文件
    InputStream in = LoaderUnpack.class.getResourceAsStream(libFileName);
    FileOutputStream out = new FileOutputStream(libFile);
    // 复制 2048 字节缓冲区...
    
    // 6. 加载本地库
    System.load(libFile.getAbsolutePath());
}
```

#### 支持的平台

| 操作系统 | 扩展名 | CPU 架构 | 资源路径示例 |
|---------|--------|---------|-------------|
| Linux | `.so` | x64 | `/native0/x64-linux.so` |
| Linux | `.so` | arm64 | `/native0/arm64-linux.so` |
| Windows | `.dll` | x64 | `/native0/x64-windows.dll` |
| macOS | `.dylib` | x64 | `/native0/x64-macos.dylib` |
| macOS | `.dylib` | arm64 | `/native0/arm64-macos.dylib` |

### 2. LoaderPlain (简化模式)

**源文件**: `obfuscator/src/main/java/by/radioegor146/compiletime/LoaderPlain.java`

#### 特点
- **直接加载模式**: 使用 `System.loadLibrary()` 从系统路径加载
- **需要预安装**: 本地库必须在 `java.library.path` 中
- **简单但不便携**: 适合已部署环境

#### 工作流程

```java
static {
    System.loadLibrary("%LIB_NAME%");  // 占位符,会被替换
}
```

#### 激活方式

使用命令行参数 `--plain-lib-name`:
```bash
java -jar native-obfuscator.jar input.jar output/ --plain-lib-name=native_library
```

生成的 Loader 代码:
```java
static {
    System.loadLibrary("native_library");  // %LIB_NAME% 被替换
}
```

## 生成的类数量

### 核心类: 1 个

根据代码分析,**只生成 1 个 Loader 类**:

```java
String loaderClassName = nativeDir + "/Loader";
// 例如: native0/Loader.class

ClassNode loaderClass;
if (plainLibName == null) {
    // 使用 LoaderUnpack 模板
    loaderClass = loadTemplate("compiletime/LoaderUnpack.class");
} else {
    // 使用 LoaderPlain 模板
    loaderClass = loadTemplate("compiletime/LoaderPlain.class");
}

// 重命名类: by.radioegor146.compiletime.LoaderUnpack → native0/Loader
ClassRemapper remapper = new ClassRemapper(resultLoaderClass, new Remapper() {
    @Override
    public String map(String internalName) {
        return internalName.equals(originalLoaderClassName) 
            ? loaderClassName  // native0/Loader
            : internalName;
    }
});

// 写入 JAR
Util.writeEntry(out, loaderClassName + ".class", classWriter.toByteArray());
// 输出: native0/Loader.class
```

### 生成的文件结构

```
output.jar
├── native0/                       ← 本地库目录 (自动编号)
│   ├── Loader.class              ← 唯一的加载器类
│   ├── x64-linux.so              ← 编译的本地库 (LoaderUnpack 模式)
│   ├── x64-windows.dll
│   ├── arm64-macos.dylib
│   └── hidden/                   ← 隐藏方法类 (如果有)
│       └── Proxy$1.class
├── com/
│   └── example/
│       └── Main.class            ← 原始类 (已修改)
└── META-INF/
    └── MANIFEST.MF
```

### 目录命名规则

```java
// 自动查找未占用的 native{N} 目录
int nativeDirId = IntStream.iterate(0, i -> i + 1)
    .filter(i -> jar.stream().noneMatch(x -> 
        x.getName().equals("native" + i) ||
        x.getName().startsWith("native" + i + "/")))
    .findFirst().orElseThrow(RuntimeException::new);

nativeDir = "native" + nativeDirId;
// native0, native1, native2, ...
```

## 加载器的公共接口

两种加载器都提供相同的 JNI 方法:

```java
public static native void registerNativesForClass(int index, Class<?> clazz);
```

### 使用方式

原始 Java 类的静态初始化器会被修改为:

```java
// 原始代码
public class BusinessLogic {
    public String process(String input) {
        // 业务逻辑
    }
}

// 转换后
public class BusinessLogic {
    static {
        native0.Loader.registerNativesForClass(0, BusinessLogic.class);
        //                                     ↑         ↑
        //                                   类ID      类引用
    }
    
    public native String process(String input);
    //     ↑ 方法变成 native 声明
}
```

### 注册流程

1. **Java 端**: `Loader.registerNativesForClass(0, BusinessLogic.class)`
2. **JNI 端**: C++ 函数查找类 ID 对应的方法表
3. **绑定**: 使用 `env->RegisterNatives()` 绑定 Java 方法到 C++ 函数

```cpp
// native_jvm_output.cpp 生成的注册函数
JNIEXPORT void JNICALL Java_native0_Loader_registerNativesForClass
  (JNIEnv *env, jclass clazz, jint index, jclass target_class) {
    
    switch (index) {
        case 0: // BusinessLogic
            {
                JNINativeMethod methods[] = {
                    {"process", "(Ljava/lang/String;)Ljava/lang/String;", 
                     (void*)&__ngen_native_process1}
                };
                env->RegisterNatives(target_class, methods, 1);
            }
            break;
        // 更多类...
    }
}
```

## 混淆功能分析

### ❌ Loader 类本身 **无混淆**

根据代码分析:

```java
loaderClass.sourceFile = "synthetic";  // 只标记为 synthetic

// 只做类重命名 (Remapper)
ClassRemapper remapper = new ClassRemapper(...);
// LoaderUnpack → native0/Loader

// 无额外混淆处理
ClassWriter classWriter = new SafeClassWriter(...);
resultLoaderClass.accept(classWriter);
Util.writeEntry(out, loaderClassName + ".class", classWriter.toByteArray());
```

**Loader 类特点**:
- ✅ 类名重命名: `LoaderUnpack` → `native0.Loader`
- ❌ 无字符串加密
- ❌ 无控制流混淆
- ❌ 无垃圾代码插入
- ❌ 无成员名混淆

**反编译 Loader.class 可以看到**:
- 清晰的平台检测逻辑
- 明文的资源路径格式
- 可读的异常消息

### 为什么不混淆?

README 中明确说明:

> **This tool does not particularly obfuscate your code; it just transpiles it to native.**
> 
> Remember to use protectors like **VMProtect, Themida, or obfuscator-llvm** (in case of clang usage)

**设计理念**:
1. **native-obfuscator 只负责**: Java → C++ 转换
2. **混淆由专业工具完成**: 
   - C++ 代码: VMProtect, Themida, obfuscator-llvm
   - Java Loader: 需要额外的 Java 混淆器 (如 Proguard, Allatori)

### 可以添加的混淆功能

如果你想修改 native-obfuscator 增强 Loader 混淆:

```java
// 在 NativeObfuscator.java 的 Loader 生成部分添加

// 1. 字符串加密
loaderClass.methods.forEach(method -> {
    for (AbstractInsnNode insn : method.instructions) {
        if (insn instanceof LdcInsnNode) {
            LdcInsnNode ldc = (LdcInsnNode) insn;
            if (ldc.cst instanceof String) {
                String encrypted = StringEncryptor.encrypt((String) ldc.cst);
                // 替换为解密调用
            }
        }
    }
});

// 2. 控制流混淆
ControlFlowObfuscator.obfuscate(loaderClass);

// 3. 成员重命名
MemberRenamer.rename(loaderClass, "a", "b", "c", ...);
```

## 实际测试结果

### 测试命令

```bash
java -jar obfuscator.jar sample.jar output/ -b blacklist.txt -p hotspot
```

### 生成的文件

```
output/
├── sample.jar                     ← 输出 JAR
│   ├── native0/
│   │   ├── Loader.class          ← 1 个加载器类
│   │   └── x64-linux.so          ← 本地库
│   └── com/test/
│       ├── Main.class
│       └── BusinessLogic.class   ← 方法变 native
└── cpp/                          ← C++ 源码
    ├── native_jvm.cpp
    ├── string_pool.cpp
    └── output/
        └── com_test_BusinessLogic_0.cpp
```

### 反编译 Loader.class

```java
// 使用 jd-gui 反编译 native0/Loader.class

package native0;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public class Loader {
    public static native void registerNativesForClass(int var0, Class<?> var1);

    static {
        String var0 = System.getProperty("os.name").toLowerCase();
        String var1 = System.getProperty("os.arch").toLowerCase();
        String var2;
        // ... 明文的平台检测代码
        
        String var4 = String.format("/%s/%s-%s", 
            Loader.class.getName().split("\\.")[0], var2, var3);
        // 可见的资源路径格式
        
        // ... 清晰的文件操作逻辑
        System.load(var5.getAbsolutePath());
    }
}
```

**结论**: Loader 类容易被逆向分析,需要额外的 Java 混淆器处理。

## 改进建议

### 1. 增强 Loader 混淆

**优先级: 高**

在 `NativeObfuscator.java` 中添加:

```java
// 生成 Loader 后应用混淆
ClassNode loaderClass = generateLoader();
loaderClass = applyObfuscation(loaderClass);  // 新增

private ClassNode applyObfuscation(ClassNode loader) {
    // 字符串加密
    encryptStrings(loader);
    // 控制流混淆
    obfuscateControlFlow(loader);
    // 垃圾代码
    insertJunkCode(loader);
    return loader;
}
```

### 2. 随机化资源路径

**优先级: 中**

```java
// 当前: /native0/x64-linux.so
// 改进: /a1b2c3/x64-linux.so (随机化)

String randomPrefix = generateRandomString();
String libFileName = String.format("/%s/%s-%s", 
    randomPrefix, platformTypeName, osTypeName);
```

### 3. 加密本地库资源

**优先级: 中**

```java
// 提取时解密
InputStream encryptedStream = getResourceAsStream(libFileName);
InputStream decryptedStream = decrypt(encryptedStream);
// 写入临时文件...
```

### 4. 多 Loader 策略

**优先级: 低**

```java
// 为每个类生成独立的 Loader
// native0/Loader1.class
// native0/Loader2.class
// ...
```

## 总结

### 核心发现

| 项目 | 结果 |
|------|------|
| **生成类数量** | **1 个** (native{N}/Loader.class) |
| **Loader 类型** | LoaderUnpack (默认) 或 LoaderPlain (可选) |
| **混淆程度** | ❌ 无混淆,仅类重命名 |
| **功能** | 加载本地库 + 注册 JNI 方法 |
| **可逆向性** | ⚠️ 高,容易反编译 |

### 关键代码位置

- **Loader 生成**: `NativeObfuscator.java:370-410`
- **LoaderUnpack 模板**: `compiletime/LoaderUnpack.java`
- **LoaderPlain 模板**: `compiletime/LoaderPlain.java`
- **类重命名**: 使用 ASM ClassRemapper

### 安全建议

1. ✅ **C++ 代码混淆**: 使用 VMProtect/obfuscator-llvm (native-obfuscator 推荐)
2. ⚠️ **Loader 类混淆**: 需要额外的 Java 混淆器 (Proguard, Allatori, **或修改源码**)
3. ✅ **字符串加密**: 修改 native-obfuscator 源码添加
4. ✅ **资源加密**: 加密 JAR 内的 .so/.dll 文件

---

**文档版本**: 1.0  
**分析日期**: 2024-01-15  
**分析者**: H3K4  
**参考代码**: native-obfuscator v3.5.4r
