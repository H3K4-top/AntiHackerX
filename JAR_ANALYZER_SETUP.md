# JAR 分析功能说明

## ✅ 无需安装任何依赖！

JAR 分析功能使用 **内嵌的 minizip** (纯 C 库) 来读取 ZIP/JAR 文件。

所有必要的源码已经包含在 `libs/minizip/` 目录中，编译时会自动构建。

**用户不需要安装任何额外的库或依赖包！**

## 编译

只需要 Qt6 和标准的构建工具：

```bash
cd build
cmake ..
make -j$(nproc)
```

## 内嵌的 minizip

项目包含以下 minizip 源码文件（来自 zlib 官方 contrib）：

- `libs/minizip/unzip.c` / `unzip.h` - ZIP 文件读取
- `libs/minizip/ioapi.c` / `ioapi.h` - 文件 I/O 抽象层
- `libs/minizip/ints.h` - 整数类型定义
- `libs/minizip/LICENSE.txt` - zlib 许可证

minizip 使用 **zlib 许可证**（非常宽松的开源许可），允许商业使用。

## 功能说明

JAR 分析器可以识别以下类型:

1. **普通 JAR**: 从 MANIFEST.MF 读取 Main-Class
2. **Minecraft Paper 插件**: 解析 plugin.yml (YAML 格式)
3. **Minecraft Fabric Mod**: 解析 fabric.mod.json (JSON 格式)
4. **Minecraft Forge Mod**: 解析 mods.toml (TOML 格式) 或 mcmod.info (JSON 格式)
5. **Spring Boot**: 检测 BOOT-INF/ 目录和 Spring-Boot-Classes

## 使用方法

1. 启动程序后，将 JAR 文件拖放到主窗口
2. 程序会自动分析并显示 JAR 类型、主类、插件名、版本等信息
3. 不同类型会用不同颜色标识:
   - 🔵 普通 JAR (蓝色)
   - 🟠 Paper 插件 (橙色)
   - 🟣 Fabric Mod (紫色)
   - 🟢 Forge Mod (绿色)
   - 🔷 Spring Boot (青色)

## 技术细节

- 使用 minizip C API (`unzOpen`, `unzLocateFile`, `unzReadCurrentFile` 等)
- 纯 C 实现，无需额外的 C++ 依赖
- 支持大小写不敏感的文件查找
- 简单的 YAML/TOML 解析器 (仅解析所需字段)
- JSON 解析使用 Qt 内置的 QJsonDocument
