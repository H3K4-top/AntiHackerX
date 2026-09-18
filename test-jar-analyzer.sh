#!/bin/bash
# 测试 JAR 分析功能

echo "=========================================="
echo "JAR 分析功能测试"
echo "=========================================="
echo ""

# 确保编译完成
if [ ! -f "build/bin/AntiHackerX" ]; then
    echo "❌ 程序未编译，请先运行:"
    echo "   cd build && cmake .. && make -j\$(nproc)"
    exit 1
fi

echo "✓ 程序已编译"
echo ""

# 检查测试 JAR
if [ ! -f "/tmp/test-plain.jar" ]; then
    echo "创建测试 JAR 文件..."
    mkdir -p /tmp/test-jar/META-INF
    cat > /tmp/test-jar/META-INF/MANIFEST.MF << 'EOF'
Manifest-Version: 1.0
Main-Class: com.example.TestMain
Created-By: AntiHackerX Test

EOF
    cd /tmp/test-jar
    zip -q ../test-plain.jar META-INF/MANIFEST.MF
    cd - > /dev/null
    echo "✓ 测试 JAR 已创建: /tmp/test-plain.jar"
else
    echo "✓ 测试 JAR 已存在: /tmp/test-plain.jar"
fi

echo ""
echo "=========================================="
echo "测试说明:"
echo "=========================================="
echo ""
echo "1. 程序会自动启动"
echo "2. 将 /tmp/test-plain.jar 拖放到主窗口"
echo "3. 应该显示:"
echo "   - 类型: 普通 JAR (蓝色)"
echo "   - 主类: com.example.TestMain"
echo ""
echo "4. 你也可以测试其他 JAR 文件:"
echo "   - Paper 插件 (需要 plugin.yml)"
echo "   - Fabric Mod (需要 fabric.mod.json)"
echo "   - Forge Mod (需要 mods.toml 或 mcmod.info)"
echo "   - Spring Boot (需要 BOOT-INF/ 目录)"
echo ""
echo "=========================================="
echo "启动程序..."
echo "=========================================="
echo ""

cd build/bin
./AntiHackerX
