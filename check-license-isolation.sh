#!/bin/bash
# AntiHackerX - 许可证隔离检查脚本
# 
# 用途: 验证 AntiHackerX 与 native-obfuscator 之间无代码耦合(纯进程调用)
# 作者: H3K4
# 日期: 2024-01-15

set -e

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

BINARY_PATH="build/bin/AntiHackerX"
ERRORS=0

echo "=========================================="
echo "  AntiHackerX 许可证隔离检查"
echo "=========================================="
echo ""

# 检查二进制文件是否存在
if [ ! -f "$BINARY_PATH" ]; then
    echo -e "${RED}❌ 错误: 找不到可执行文件 $BINARY_PATH${NC}"
    echo "请先运行 ./build.sh 构建项目"
    exit 1
fi

echo "检查目标: $BINARY_PATH"
echo ""

# ===== 检查 1: 符号表检查 =====
echo "【检查 1】符号表分析"
echo "  目的: 确认二进制不包含 GPL 代码的符号引用"
echo ""

# 检查是否有 nm 命令
if ! command -v nm &> /dev/null; then
    echo -e "${YELLOW}⚠️  警告: nm 命令未找到,跳过符号表检查${NC}"
else
    # 搜索可疑的符号 (排除 Qt 元对象系统生成的符号)
    SUSPICIOUS_SYMBOLS=(
        "radioegor"
        "Transpil"
        "by_radioegor"
    )
    
    FOUND_SYMBOLS=0
    for symbol in "${SUSPICIOUS_SYMBOLS[@]}"; do
        if nm "$BINARY_PATH" 2>/dev/null | grep -i "$symbol" > /dev/null; then
            echo -e "${RED}  ❌ 发现可疑符号: $symbol${NC}"
            nm "$BINARY_PATH" | grep -i "$symbol" | head -3
            FOUND_SYMBOLS=1
            ERRORS=$((ERRORS + 1))
        fi
    done
    
    # ObfuscatorController 是我们自己的类,不是 GPL 代码
    # Qt 元对象系统会生成 qt_meta_stringdata_ObfuscatorController 等符号
    echo "说明: ObfuscatorController 是主项目自己的类,不含 upstream 代码"
    
    if [ $FOUND_SYMBOLS -eq 0 ]; then
        echo -e "${GREEN}  ✅ 通过: 未发现 GPL 上游项目符号${NC}"
    fi
fi
echo ""

# ===== 检查 2: 动态链接库检查 =====
echo "【检查 2】动态链接库分析"
echo "  目的: 确认未动态链接 GPL 库"
echo ""

if ! command -v ldd &> /dev/null; then
    echo -e "${YELLOW}⚠️  警告: ldd 命令未找到,跳过链接检查${NC}"
else
    if ldd "$BINARY_PATH" | grep -i "obfuscator\|native.*obf" > /dev/null; then
        echo -e "${RED}  ❌ 失败: 发现 GPL 库链接${NC}"
        ldd "$BINARY_PATH" | grep -i "obfuscator\|native.*obf"
        ERRORS=$((ERRORS + 1))
    else
        echo -e "${GREEN}  ✅ 通过: 未链接 GPL 库${NC}"
    fi
    
    # 显示实际链接的库 (仅 Qt 和系统库)
    echo "  已链接的库:"
    ldd "$BINARY_PATH" | grep -E "libQt|libc\.|libstdc" | head -5
fi
echo ""

# ===== 检查 3: 进程隔离机制检查 =====
echo "【检查 3】进程隔离机制"
echo "  目的: 确认使用 QProcess 进行进程间调用"
echo ""

CONTROLLER_FILE="src/obfuscator_controller.cpp"
if [ ! -f "$CONTROLLER_FILE" ]; then
    echo -e "${RED}  ❌ 失败: 找不到 $CONTROLLER_FILE${NC}"
    ERRORS=$((ERRORS + 1))
else
    # 检查 QProcess 使用
    if grep -q "QProcess" "$CONTROLLER_FILE"; then
        echo -e "${GREEN}  ✅ 通过: 使用 QProcess 进行进程隔离${NC}"
        
        # 显示关键代码行
        echo "  关键代码片段:"
        grep -n "process->start\|QProcess\s*\*\s*process" "$CONTROLLER_FILE" | head -3 | sed 's/^/    /'
    else
        echo -e "${RED}  ❌ 失败: 未找到 QProcess 调用${NC}"
        ERRORS=$((ERRORS + 1))
    fi
fi
echo ""

# ===== 检查 4: 静态编译检查 =====
echo "【检查 4】静态编译检查"
echo "  目的: 确认未将 GPL 代码静态编译进二进制"
echo ""

if ! command -v strings &> /dev/null; then
    echo -e "${YELLOW}⚠️  警告: strings 命令未找到,跳过字符串检查${NC}"
else
    # 搜索 GPL 上游作者相关字符串
    SUSPICIOUS_STRINGS=(
        "radioegor146"
        "by\.radioegor"
    )
    
    FOUND_STRINGS=0
    for str in "${SUSPICIOUS_STRINGS[@]}"; do
        if strings "$BINARY_PATH" | grep -E "$str" > /dev/null; then
            COUNT=$(strings "$BINARY_PATH" | grep -c -E "$str" || true)
            echo -e "${RED}  ❌ 发现 GPL 上游作者引用: $str (共 $COUNT 次)${NC}"
            FOUND_STRINGS=1
            ERRORS=$((ERRORS + 1))
        fi
    done
    
    # "native-obfuscator" 字符串是合理的 (用于日志、配置等)
    echo "  说明: 'native-obfuscator' 字符串用于日志和配置,属于正常引用"
    
    if [ $FOUND_STRINGS -eq 0 ]; then
        echo -e "${GREEN}  ✅ 通过: 无 GPL 上游代码嵌入${NC}"
    fi
fi
echo ""

# ===== 检查 5: 文件大小合理性检查 =====
echo "【检查 5】文件大小分析"
echo "  目的: 检测异常的文件大小 (可能包含嵌入代码)"
echo ""

BINARY_SIZE=$(stat -c%s "$BINARY_PATH" 2>/dev/null || stat -f%z "$BINARY_PATH" 2>/dev/null)
BINARY_SIZE_MB=$((BINARY_SIZE / 1024 / 1024))

echo "  二进制大小: ${BINARY_SIZE_MB} MB"

# Qt 应用一般在 1-10 MB 之间
if [ "$BINARY_SIZE_MB" -gt 20 ]; then
    echo -e "${YELLOW}  ⚠️  警告: 文件大小异常 (>20MB),可能包含额外代码${NC}"
elif [ "$BINARY_SIZE_MB" -lt 1 ]; then
    echo -e "${YELLOW}  ⚠️  警告: 文件大小异常 (<1MB),可能构建不完整${NC}"
else
    echo -e "${GREEN}  ✅ 通过: 文件大小正常${NC}"
fi
echo ""

# ===== 检查 6: CMakeLists.txt 检查 =====
echo "【检查 6】构建配置检查"
echo "  目的: 确认 CMake 未链接 GPL 库"
echo ""

CMAKE_FILE="CMakeLists.txt"
if [ ! -f "$CMAKE_FILE" ]; then
    echo -e "${RED}  ❌ 失败: 找不到 $CMAKE_FILE${NC}"
    ERRORS=$((ERRORS + 1))
else
    # 检查是否有可疑的库链接 (排除源文件引用)
    if grep "target_link_libraries" "$CMAKE_FILE" | grep -i "obfuscator\|native.*obf" > /dev/null; then
        echo -e "${RED}  ❌ 失败: 发现 GPL 库链接${NC}"
        grep "target_link_libraries" "$CMAKE_FILE" | grep -i "obfuscator\|native.*obf" | sed 's/^/    /'
        ERRORS=$((ERRORS + 1))
    else
        echo -e "${GREEN}  ✅ 通过: 构建配置未链接 GPL 库${NC}"
    fi
    
    echo "说明: obfuscator_controller.cpp 是主项目自己的源文件,不是 upstream 的库"
    # 显示实际链接的库
    echo "  实际链接的库:"
    grep "target_link_libraries" "$CMAKE_FILE" | sed 's/^/    /'
fi
echo ""

# ===== 检查 7: 许可证文件检查 =====
echo "【检查 7】许可证文件"
echo "  目的: 确认主项目 LICENSE 与预期一致"
echo ""

LICENSE_FILE="LICENSE"
if [ -f "$LICENSE_FILE" ]; then
    if grep -qi "GNU GENERAL PUBLIC LICENSE" "$LICENSE_FILE" \
       && grep -q "Version 3" "$LICENSE_FILE"; then
        echo -e "${GREEN}  ✅ 通过: 主项目使用 GNU GPL v3.0${NC}"
    elif grep -qi "MIT\|Apache" "$LICENSE_FILE"; then
        echo -e "${YELLOW}  ⚠️  警告: 主项目看起来是宽松许可证,与预期(GPL-3.0)不符${NC}"
    else
        echo -e "${YELLOW}  ⚠️  警告: 许可证类型不明确${NC}"
    fi
else
    echo -e "${YELLOW}  ⚠️  警告: 未找到 LICENSE 文件${NC}"
fi
echo ""

# ===== 检查 8: 独立进程验证 (运行时) =====
echo "【检查 8】运行时进程隔离 (可选)"
echo "  目的: 验证运行时确实使用独立进程"
echo ""
echo "  提示: 运行 AntiHackerX 并执行混淆任务时,使用以下命令验证:"
echo ""
echo "    ps aux | grep -E 'AntiHackerX|java.*obfuscator'"
echo ""
echo "  应该看到两个独立进程:"
echo "    - AntiHackerX (C++ GUI)"
echo "    - java -jar native-obfuscator-mod.jar (GPL 组件)"
echo ""

# ===== 总结 =====
echo "=========================================="
echo "  检查总结"
echo "=========================================="
echo ""

if [ $ERRORS -eq 0 ]; then
    echo -e "${GREEN}✅ 所有检查通过!${NC}"
    echo ""
    echo "代码隔离状态: 完全隔离"
    echo "AntiHackerX 与 native-obfuscator 之间只有进程调用,无代码耦合"
    echo "两者均为 GPL-3.0;隔离不是许可证要求,而是架构选择"
    echo ""
    exit 0
else
    echo -e "${RED}❌ 检查失败: 发现 $ERRORS 个问题${NC}"
    echo ""
    echo "请修复上述问题以确保许可证隔离"
    echo ""
    exit 1
fi
