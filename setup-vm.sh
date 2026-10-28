#!/bin/bash
set -euo pipefail

echo "=== Learn Wayland - VM 环境配置 ==="
echo "适用于 Ubuntu 24.04 LTS"
echo

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
fi

echo "[1/5] 更新软件源..."
$SUDO apt update -q

echo "[2/5] 安装基础编译工具链..."
$SUDO apt install -y -q \
    build-essential \
    pkg-config \
    cmake \
    meson \
    ninja-build \
    git \
    wget

echo "[3/5] 安装 Wayland 开发依赖（客户端）..."
$SUDO apt install -y -q \
    libwayland-dev \
    wayland-protocols \
    libxkbcommon-dev \
    libcairo2-dev \
    libpixman-1-dev

echo "[4/5] 安装 Wayland 开发依赖（服务端）..."
$SUDO apt install -y -q \
    libdrm-dev \
    libinput-dev \
    libudev-dev \
    libgbm-dev \
    libegl1-mesa-dev \
    libgles2-mesa-dev

echo "[5/5] 安装系统上下文依赖（音频/会话, 模块 13-14）..."
$SUDO apt install -y -q \
    libasound2-dev \
    libpipewire-0.3-dev \
    libsystemd-dev

# 运行时观察工具 (非编译必需, 但强烈建议: docs 里的动手观察清单会用到)
$SUDO apt install -y -q \
    alsa-utils \
    pulseaudio-utils \
    wireplumber \
    acl \
    psmisc \
    weston 2>/dev/null || \
    echo "  (部分观察工具安装失败, 不影响编译; weston 提供 weston-simple-shm 演示客户端)"

echo
echo "=== 环境配置完成 ==="
echo
echo "验证安装:"
pkg-config --modversion wayland-client wayland-server wayland-protocols 2>/dev/null || true
echo
echo "下一步: cd module-01-connection && make && ./list-globals"
