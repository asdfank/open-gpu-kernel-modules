#!/bin/bash
# GSP Fuzz Hook 用户态测试工具编译脚本

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_FILE="$SCRIPT_DIR/gsp_fuzz_hook_user.c"
OUT_FILE="$SCRIPT_DIR/gsp_fuzz_hook_user"

echo "=== 编译 GSP Fuzz Hook 用户态测试工具 ==="
echo "源文件: $SRC_FILE"
echo "输出: $OUT_FILE"

# 编译
gcc -Wall -Wextra -O2 -o "$OUT_FILE" "$SRC_FILE"

if [ $? -eq 0 ]; then
    echo "✅ 编译成功!"
    echo ""
    echo "使用方法:"
    echo "  sudo $OUT_FILE --help     # 查看帮助"
    echo "  sudo $OUT_FILE -s         # 查看统计信息"
    echo "  sudo $OUT_FILE -e         # 启用Hook"
    echo "  sudo $OUT_FILE -m         # 持续监控"
    echo "  sudo $OUT_FILE -g 10      # 获取10个种子"
else
    echo "❌ 编译失败"
    exit 1
fi
