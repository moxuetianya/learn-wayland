#!/bin/bash
set -euo pipefail

echo "=== Learn Wayland - VM 环境配置 ==="
echo "适用于 Ubuntu 24.04 LTS"
echo

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    SUDO="sudo"
fi

echo "[1/4] 更新软件源..."
$SUDO apt update -qq

echo "[2/4] 安装基础编译工具链..."
$SUDO apt install -y -qq \
    build-essential \
    pkg-config \
    cmake \
    meson \
    ninja-build \
    git \
    wget

echo "[3/4] 安装 Wayland 开发依赖（客户端）..."
$SUDO apt install -y -qq \
    libwayland-dev \
    wayland-protocols \
    libxkbcommon-dev \
    libcairo2-dev \
    libpixman-1-dev

echo "[4/4] 安装 Wayland 开发依赖（服务端）..."
$SUDO apt install -y -qq \
    libdrm-dev \
    libinput-dev \
    libudev-dev \
    libgbm-dev \
    libegl1-mesa-dev \
    libgles2-mesa-dev

echo
echo "=== 环境配置完成 ==="
echo
echo "验证安装:"
pkg-config --modversion wayland-client wayland-server wayland-protocols 2>/dev/null || true
echo
echo "下一步: cd module-01-connection && make && ./list-globals"
