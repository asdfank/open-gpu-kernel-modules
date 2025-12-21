#!/bin/bash
# fuzz_deploy.sh: 编译修改后的驱动并热加载
# 参考自 Moneta: build-nvidia-driver.sh & copy-modules.sh
# 
# 使用方法: sudo ./fuzz_deploy.sh
# 注意: 此脚本需要 root 权限来安装模块

set -e # 遇到错误立即停止

DRIVER_SOURCE_DIR="/home/lyh/Desktop/open-gpu-kernel-modules" # <--- 修改这里为你的源码路径
JOBS=$(nproc)

# 检查是否以 root 权限运行
if [ "$EUID" -ne 0 ]; then
    echo "❌ 错误：此脚本需要 root 权限"
    echo "请使用: sudo $0"
    exit 1
fi

echo "=== [Step 1] 编译修改后的驱动 ==="
cd "$DRIVER_SOURCE_DIR"

make modules -j$JOBS CC=gcc
echo "✓ 编译成功"

echo ""
echo "编译好的模块位置:"
echo "  - $DRIVER_SOURCE_DIR/kernel-open/nvidia.ko"
echo "  - $DRIVER_SOURCE_DIR/kernel-open/nvidia-modeset.ko"
echo "  - $DRIVER_SOURCE_DIR/kernel-open/nvidia-uvm.ko"
echo "  - $DRIVER_SOURCE_DIR/kernel-open/nvidia-drm.ko"
echo "  - $DRIVER_SOURCE_DIR/kernel-open/nvidia-peermem.ko"
echo ""

echo "=== [Step 2] 卸载旧模块 (热卸载) ==="

# 1) 停止桌面/显示服务，切到命令行 target
echo "正在停止显示管理器..."
systemctl stop gdm gdm3 sddm lightdm display-manager 2>/dev/null || true
sleep 2
systemctl isolate multi-user.target 2>/dev/null || true
sleep 2

# 2) 杀掉占用 GPU/DRM 的进程 (使用 SIGTERM 而非 SIGKILL，更温和)
echo "正在终止占用 GPU 的进程..."
# 先列出占用者
fuser -v /dev/nvidia* 2>/dev/null || true
fuser -v /dev/dri/* 2>/dev/null || true

# 使用 SIGTERM (-15) 而非默认的 SIGKILL
for dev in /dev/nvidia* /dev/dri/*; do
    [ -e "$dev" ] && fuser -k -TERM "$dev" 2>/dev/null || true
done
sleep 3

# 如果还有残留，再用 SIGKILL
for dev in /dev/nvidia* /dev/dri/*; do
    [ -e "$dev" ] && fuser -k -KILL "$dev" 2>/dev/null || true
done
sleep 2

# 3) 递归卸载 - 按正确顺序逐个卸载
echo "正在卸载模块..."
for mod in nvidia_drm nvidia_modeset nvidia_uvm nvidia; do
    if lsmod | grep -q "^$mod "; then
        echo "  卸载 $mod ..."
        rmmod "$mod" 2>/dev/null || modprobe -r "$mod" 2>/dev/null || true
        sleep 1
    fi
done

# 4) 再检查
if lsmod | grep -q "^nvidia "; then
    echo "❌ 错误：仍无法卸载 nvidia 模块，下面是占用者："
    fuser -v /dev/nvidia* 2>/dev/null || true
    fuser -v /dev/dri/* 2>/dev/null || true
    lsmod | egrep 'nvidia|drm'
    exit 1
fi
echo "✓ 旧模块已卸载"


echo "=== [Step 3] 安装新模块 ==="
# 使用 make modules_install 会把 .ko 放到 /lib/modules/.../video/ 下
make modules_install -j$JOBS
depmod -a
echo "✓ 模块已安装到系统目录"

echo "=== [Step 4] 加载新驱动 ==="
modprobe nvidia

# 验证加载情况
echo "=== [Step 5] 验证状态 ==="
# 检查 dmesg 中是否有你的 Hook 初始化日志
if dmesg | tail -n 50 | grep -i "GSP"; then
    echo "✓ 发现 GSP 相关日志"
else
    echo "⚠️  警告：未发现近期 GSP 日志，请检查 dmesg"
fi

# 检查 nvidia-smi 是否工作 (这一步最关键，验证版本匹配)
if nvidia-smi > /dev/null; then
    echo "✅ 成功：nvidia-smi 运行正常，版本匹配！"
    nvidia-smi | grep "Driver Version"
else
    echo "❌ 失败：nvidia-smi 报错，可能是版本不匹配或 GSP 固件加载失败"
    exit 1
fi