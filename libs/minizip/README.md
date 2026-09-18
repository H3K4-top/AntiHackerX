# 内嵌的 minizip 库

本目录包含来自 zlib 官方仓库的 minizip 源码。

## 来源

- 官方仓库: https://github.com/madler/zlib/tree/master/contrib/minizip
- 版本: MiniZip 1.1 with ZIP64 support
- 许可证: zlib License (非常宽松，允许商业使用)

## 文件说明

- `unzip.c` / `unzip.h` - ZIP 文件解压缩功能
- `ioapi.c` / `ioapi.h` - 文件 I/O 抽象层
- `ints.h` - 整数类型定义 (为兼容性添加)
- `crypt.h` - 加密相关定义 (minizip 依赖)
- `LICENSE.txt` - 许可证信息

## 为什么内嵌？

1. **无依赖**: 用户不需要安装任何额外的库
2. **纯 C**: 比 C++ 封装（QuaZip）更轻量
3. **可移植**: 只依赖标准 C 库和 zlib（通常系统自带）
4. **小巧**: 总共不到 100KB 的源码
5. **许可友好**: zlib 许可证允许自由使用

## 许可证

minizip 使用 zlib 许可证，这是一个非常宽松的开源许可证：

```
Copyright (c) 1998-2026 Gilles Vollant
  http://www.winimage.com/zLibDll/minizip.html

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## 使用方法

这些文件会在 CMake 构建时自动编译，不需要任何手动操作。

编译系统会自动：
1. 检测 C 编译器
2. 编译 minizip 的 C 源文件
3. 链接到主程序

用户只需要运行：
```bash
cd build
cmake ..
make -j$(nproc)
```
