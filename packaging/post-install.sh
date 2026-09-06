#!/bin/sh
set -eu

command -v update-desktop-database >/dev/null 2>&1 && \
  update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
  gtk-update-icon-cache -q -t /usr/share/icons/hicolor >/dev/null 2>&1 || true

cat <<'EOF'
WeTypeX 已安装。首次使用请准备原版运行时：
  fcitx5-wetypex-setup --archive /path/to/WeType_2.2.3_657.zip
EOF
