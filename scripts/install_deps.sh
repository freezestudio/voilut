#!/usr/bin/env bash
# ============================================================
# install_deps.sh —— 一次获取全部第三方依赖
# ============================================================
# 依次执行：
#   install_dawn.sh   → third_party/dawn   （约 820 MB，Dawn + Tint + 依赖）
#   install_gdcm.sh   → third_party/GDCM   （约 15 MB，DICOM 解码）
#   install_imgui.sh  → third_party/imgui  （约 5 MB，UI）
#
# 用法：
#   bash scripts/install_deps.sh
#   bash scripts/install_deps.sh --force                  # 全部重新安装
#   bash scripts/install_deps.sh --github-mirror "https://xxx/https://github.com/"
#   GITHUB_MIRROR_URL=https://xxx/ bash scripts/install_deps.sh
#
# 提示：Dawn 的依赖获取（19 个仓库）耗时约 5~20 分钟，取决于网络。
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log() { printf '\033[1;35m[deps]\033[0m %s\n' "$*"; }

log "1/3 Dawn"
bash "${SCRIPT_DIR}/install_dawn.sh" "$@"

log "2/3 GDCM"
bash "${SCRIPT_DIR}/install_gdcm.sh" "$@"

log "3/3 Dear ImGui"
bash "${SCRIPT_DIR}/install_imgui.sh" "$@"

log "全部就绪，下一步：cmake -B build -G Ninja && cmake --build build && ./build/windower"
