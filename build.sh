#!/bin/bash
# AntiHackerX 构建脚本

set -e

echo "===== AntiHackerX 构建脚本 ====="
echo ""

# 检查 Qt6
if ! command -v qmake6 &> /dev/null && ! command -v qmake &> /dev/null; then
    echo "错误: 未找到 Qt6"
    echo "请安装 Qt6:"
    echo "  Ubuntu/Debian: sudo apt install qt6-base-dev"
    echo "  Arch: sudo pacman -S qt6-base"
    echo "  macOS: brew install qt@6"
    exit 1
fi

# 创建构建目录
mkdir -p build
cd build

# 运行 CMake
echo "正在配置项目..."
cmake ..

# 编译
echo ""
echo "正在编译..."
cmake --build . --config Release

echo ""
echo "✓ 构建完成!"
echo "可执行文件: $(pwd)/bin/AntiHackerX"
echo ""
echo "运行程序:"
echo "  ./bin/AntiHackerX"
