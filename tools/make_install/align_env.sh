#!/bin/bash
# align_env.sh: 将用户态和固件升级到 575.64.03
# 必须以 root 运行

RUN_FILE="../../../NVIDIA-Linux-x86_64-575.64.03.run"

if [ ! -f "$RUN_FILE" ]; then
    echo "错误：请先下载 $RUN_FILE 并放在当前目录"
    exit 1
fi

echo "[1] 停止显示服务..."
systemctl stop gdm
systemctl stop display-manager
systemctl isolate multi-user.target

echo "[2] 卸载旧驱动 (575.57.08)..."
# 注意：如果之前是用 apt 安装的，这里可能需要 apt remove nvidia-*
# 如果是用 .run 安装的，可以用 nvidia-uninstall
if command -v nvidia-uninstall &> /dev/null; then
    nvidia-uninstall -s
else
    echo "警告：未找到 nvidia-uninstall，尝试强制卸载模块..."
    rmmod nvidia_drm nvidia_modeset nvidia_uvm nvidia
fi

echo "[3] 安装 575.64.03 (仅用户态和固件，不安装内核模块)..."
# 关键参数：--no-kernel-modules
# 我们只想要 libs 和 firmware，内核模块我们要自己编译改过的
sh "$RUN_FILE" --silent --no-kernel-modules --install-libglvnd

echo "[4] 准备完毕"
test -d /lib/firmware/nvidia/575.64.03 || { echo "固件目录不存在"; exit 1; }
echo "现在的状态是：用户态=575.64.03，内核态=无(或待加载)"