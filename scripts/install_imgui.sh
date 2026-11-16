#!/usr/bin/env bash
# ============================================================
# install_imgui.sh —— 下载 Dear ImGui 源码到 third_party/imgui
# ============================================================
# Windower 的 UI 用 Dear ImGui（GLFW + WebGPU/Dawn 后端），
# 源码约 5 MB，不入版本库。
#
# 版本固定为 IMGUI_VERSION，因为 imgui_impl_wgpu 的初始化结构体
# （ImGui_ImplWGPU_InitInfo 的字段）在小版本间会变，升级需同步改
# apps/windower.cxx。
#
# 用法：
#   bash scripts/install_imgui.sh
#   bash scripts/install_imgui.sh --github-mirror "https://xxx/https://github.com/"
#   IMGUI_GH_MIRROR=URL bash scripts/install_imgui.sh
#   bash scripts/install_imgui.sh --force    # 已存在时强制重新安装
# ============================================================

set -euo pipefail

IMGUI_VERSION="1.92.4"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST_DIR="${ROOT_DIR}/third_party"
IMGUI_DIR="${DEST_DIR}/imgui"

OPT_MIRROR=""
FORCE=0

log() { printf '\033[1;34m[imgui]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[imgui]\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --github-mirror)
      OPT_MIRROR="${2:-}"
      [ -n "$OPT_MIRROR" ] || die "--github-mirror 需要一个参数"
      shift 2
      ;;
    --force) FORCE=1; shift ;;
    -h | --help)
      sed -n '2,18p' "$0"
      exit 0
      ;;
    *) die "未知参数：$1" ;;
  esac
done

# 检测 GitHub 镜像，语义与 install_dawn.sh 一致：
#   GH_BASE 直到 `https://github.com/` 结束。
detect_gh_base() {
  if [ -n "$OPT_MIRROR" ]; then
    printf '%s' "${OPT_MIRROR%/}/"
    return
  fi
  if [ -n "${IMGUI_GH_MIRROR:-}" ]; then
    printf '%s' "${IMGUI_GH_MIRROR%/}/"
    return
  fi
  if [ -n "${GITHUB_MIRROR_URL:-}" ]; then
    printf '%s' "${GITHUB_MIRROR_URL%/}/https://github.com/"
    return
  fi
  local line key val base=""
  while IFS= read -r line; do
    key="${line%% *}"
    val="${line#* }"
    if [ "$val" = "https://github.com/" ]; then
      key="${key#url.}"
      key="${key%.insteadof}"
      key="${key%.insteadOf}"
      base="$key"
      break
    fi
  done < <(git config --get-regexp '^url\..*\.insteadof$' 2>/dev/null || true)
  if [ -n "$base" ]; then printf '%s' "$base"; else printf '%s' "https://github.com/"; fi
}

if [ -f "${IMGUI_DIR}/imgui.h" ] && [ "$FORCE" -ne 1 ]; then
  log "Dear ImGui 已存在：${IMGUI_DIR}（加 --force 可重新安装）"
  exit 0
fi

command -v git >/dev/null || die "需要 git。"

GH_BASE="$(detect_gh_base)"
REMOTE="${GH_BASE}ocornut/imgui"

log "克隆 Dear ImGui v${IMGUI_VERSION}"
log "  URL: ${REMOTE}"
mkdir -p "$DEST_DIR"
rm -rf "$IMGUI_DIR"
if ! git clone -q --depth 1 --branch "v${IMGUI_VERSION}" "$REMOTE" "$IMGUI_DIR"; then
  printf '\033[1;33m[imgui] 警告:\033[0m 镜像获取失败，回退到 github.com 直连\n'
  rm -rf "$IMGUI_DIR"
  git clone -q --depth 1 --branch "v${IMGUI_VERSION}" \
    "https://github.com/ocornut/imgui" "$IMGUI_DIR"
fi
# 仅保留源码，删除 .git（减小体积，也避免被上层视为子模块）。
rm -rf "${IMGUI_DIR}/.git"

[ -f "${IMGUI_DIR}/imgui.h" ] || die "克隆结果缺少 imgui.h。"
[ -f "${IMGUI_DIR}/backends/imgui_impl_wgpu.cpp" ] || die "克隆结果缺少 WebGPU 后端。"
log "完成：${IMGUI_DIR}（$(grep -m1 'define IMGUI_VERSION ' "${IMGUI_DIR}/imgui.h" | cut -d'"' -f2)）"
