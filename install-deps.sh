#!/bin/bash
# 安装 AntiHackerX 的依赖项

echo "正在安装依赖..."
echo ""

# Qt6 和构建工具
echo "1. 安装 Qt6 和构建工具..."
sudo apt install -y \
    qt6-base-dev \
    qt6-tools-dev \
    cmake \
    build-essential \
    pkg-config

# zlib (minizip 的依赖，但 minizip 源码已内嵌在项目中)
echo ""
echo "2. 确保 zlib 已安装..."
sudo apt install -y zlib1g-dev

echo ""
echo "✓ 依赖安装完成！"
echo ""
echo "注意: minizip 源码已内嵌在项目中 (libs/minizip/)，无需额外安装"
echo ""
echo "现在可以编译项目:"
echo "  cd build"
echo "  cmake .."
echo "  make -j\$(nproc)"
