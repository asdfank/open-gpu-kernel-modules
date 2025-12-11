#!/bin/bash
# 列出 NVIDIA 驱动中所有可用的控制命令
# 用法: ./list_control_commands.sh [ClassName] [grep_pattern]
#
# 示例:
#   ./list_control_commands.sh Subdevice          # 列出所有 Subdevice 命令
#   ./list_control_commands.sh Subdevice GSP     # 列出 Subdevice 中与 GSP 相关的命令
#   ./list_control_commands.sh Device            # 列出所有 Device 命令

CLASS_NAME="${1:-Subdevice}"
PATTERN="${2:-}"

NVOC_FILE="/home/lwq/Desktop/open-gpu-kernel-modules/src/nvidia/generated/g_${CLASS_NAME,,}_nvoc.c"

if [ ! -f "$NVOC_FILE" ]; then
    echo "错误: 找不到文件 $NVOC_FILE"
    echo ""
    echo "可用的类名:"
    ls -1 /home/lwq/Desktop/open-gpu-kernel-modules/src/nvidia/generated/g_*_nvoc.c | sed 's|/home/lwq/Desktop/open-gpu-kernel-modules/src/nvidia/generated/g_||; s|_nvoc.c||' | sort
    exit 1
fi

echo "=========================================="
echo "类: $CLASS_NAME"
echo "文件: $NVOC_FILE"
echo "=========================================="
echo ""

# 提取命令信息
if [ -z "$PATTERN" ]; then
    # 列出所有命令
    grep -E "(methodId|func=)" "$NVOC_FILE" | \
        awk '
        /methodId/ {
            match($0, /0x[0-9a-fA-F]+/);
            methodId = substr($0, RSTART, RLENGTH);
            getline;
            if (/func=/) {
                match($0, /"[^"]+"/);
                funcName = substr($0, RSTART+1, RLENGTH-2);
                printf "0x%08s  %s\n", methodId, funcName;
            }
        }' | sort -k1
else
    # 按模式过滤
    grep -i "$PATTERN" "$NVOC_FILE" -A 5 -B 5 | \
        awk '
        /methodId/ {
            match($0, /0x[0-9a-fA-F]+/);
            methodId = substr($0, RSTART, RLENGTH);
            getline;
            if (/func=/) {
                match($0, /"[^"]+"/);
                funcName = substr($0, RSTART+1, RLENGTH-2);
                printf "0x%08s  %s\n", methodId, funcName;
            }
        }' | sort -k1 | uniq
fi

echo ""
echo "=========================================="
echo "提示: 使用 grep 在头文件中查找命令定义:"
echo "  grep -r '0x[命令ID]' /home/lwq/Desktop/open-gpu-kernel-modules/src/common/sdk/nvidia/inc/ctrl/"
echo "=========================================="

