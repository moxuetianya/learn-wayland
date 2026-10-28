#!/usr/bin/env bash
# inspect.sh — 模块 14: 登录/会话/显示权力的系统性观察清单 (只读, 全部可安全执行)
# 对应文档: ../docs/04-login-and-session.md
#
# 用法: bash inspect.sh [章节号]
#   bash inspect.sh        # 跑全部
#   bash inspect.sh 3      # 只跑第 3 章

set -u

sec() { printf '\n\033[1;36m═══ %s ═══\033[0m\n' "$1"; }
run() { printf '\033[1;33m$\033[0m %s\n' "$*"; "$@" 2>&1 | head -30; }

R="${1:-}"

# ─────────────────────────────────────────────────────────
if [[ -z $R || $R == 1 ]]; then
sec "1. 身份与认证痕迹 (PAM, docs/04 §2)"
run loginctl list-sessions
echo
echo "→ 每个 session 都源于一次 login + pam_systemd.so 的注册"
[[ -f /etc/pam.d/login ]] && grep -n pam_systemd /etc/pam.d/login || true
echo "→ 上次登录记录:"
run last -a -n 5
fi

# ─────────────────────────────────────────────────────────
if [[ -z $R || $R == 2 ]]; then
sec "2. seat 与 VT (docs/04 §3.1/3.2)"
run loginctl list-seats
run loginctl show-session "${XDG_SESSION_ID:-self}" -p Type -p Class -p State -p Active -p TTY -p Display -p Remote -p Service
echo "→ TTY 设备们 (内核的虚拟终端):"
ls /dev/tty[0-9]* 2>/dev/null | head -8
echo "→ 当前前台 VT: "
run fgconsole
fi

# ─────────────────────────────────────────────────────────
if [[ -z $R || $R == 3 ]]; then
sec "3. 设备访问权 (docs/04 §3.3)"
ls -l /dev/dri/ 2>/dev/null
# 显卡不一定叫 card0 (virtio-gpu 常为 card1), 自动取第一个 card 节点
DRM_CARD_DEV=$(ls /dev/dri/ 2>/dev/null | grep -E '^card[0-9]+$' | sort -V | head -1)
DRM_CARD_DEV=${DRM_CARD_DEV:+/dev/dri/$DRM_CARD_DEV}
DRM_CARD_DEV=${DRM_CARD_DEV:-/dev/dri/card0}
echo "→ 检测到 DRM card 设备: $DRM_CARD_DEV"
echo "→ 注意 card 设备的 ACL 末尾若有 '+' 号, 用 getfacl 看:"
getfacl -p "$DRM_CARD_DEV" 2>/dev/null | head -10 || echo "  (getfacl 未安装: sudo apt install acl)"
echo
echo "→ 谁开着它 (≈ 当前 DRM master / compositor):"
run fuser -v "$DRM_CARD_DEV"
echo "→ 进程是谁:"
PID=$(fuser "$DRM_CARD_DEV" 2>/dev/null | tr -d ' ')
[[ -n ${PID} ]] && ps -o pid,comm,args -p "$PID" | head -3
fi

# ─────────────────────────────────────────────────────────
if [[ -z $R || $R == 4 ]]; then
sec "4. Wayland 服务发现三要素 (docs/04 §6.1)"
run env
echo "→ 运行时目录 (logind 会话的产物):"
ls -la "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" 2>/dev/null | head -15
echo "→ socket 权限决定谁能连接 compositor:"
ls -l "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/wayland-* 2>/dev/null
fi

# ─────────────────────────────────────────────────────────
if [[ -z $R || $R == 5 ]]; then
sec "5. 显示管理器与会话移交复盘 (docs/04 §4)"
run systemctl status display-manager --no-pager
echo "→ 本次启动的登录/移交时间线:"
run journalctl -b 0 -g 'New session|Session c|logind|gdm' --no-pager
fi

# ─────────────────────────────────────────────────────────
if [[ -z $R || $R == 6 ]]; then
sec "6. 用户级服务 (compositor 的同僚, docs/00 §2 第⑤层)"
run systemctl --user list-units --no-pager --state=running
echo "→ portal 与 PipeWire 在这里 —— 图形桌面的周边服务:"
systemctl --user status xdg-desktop-portal pipewire wireplumber --no-pager 2>/dev/null | grep -E '●|Loaded' | head -8
fi

# ─────────────────────────────────────────────────────────
sec "完成。把每节输出与 docs/04 的章节对照阅读。"
