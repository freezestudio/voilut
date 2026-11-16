#!/usr/bin/env bash
# ============================================================
# install_gdcm.sh —— 下载 GDCM 源码到 third_party/GDCM
# ============================================================
# GDCM（Grassroots DICOM）用于 Windower 的 DICOM 解码，支持压缩传输语法
# （JPEG / JPEG-LS / JPEG2000 / RLE）。源码约 13 MB，不入版本库。
#
# GDCM 3.0.22 自带 openjpeg / CharLS / IJG JPEG 源码（Utilities/），
# 因此无需额外安装压缩库。
#
# 用法：
#   bash scripts/install_gdcm.sh
#   bash scripts/install_gdcm.sh --github-mirror "https://xxx/https://github.com/"
#   GDCM_GH_MIRROR=URL bash scripts/install_gdcm.sh
#   bash scripts/install_gdcm.sh --force    # 已存在时强制重新安装
# ============================================================

set -euo pipefail

GDCM_VERSION="3.0.22"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST_DIR="${ROOT_DIR}/third_party"
GDCM_DIR="${DEST_DIR}/GDCM"

OPT_MIRROR=""
FORCE=0

log() { printf '\033[1;34m[gdcm]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[gdcm]\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --github-mirror)
      OPT_MIRROR="${2:-}"
      [ -n "$OPT_MIRROR" ] || die "--github-mirror 需要一个参数"
      shift 2
      ;;
    --force) FORCE=1; shift ;;
    -h | --help)
      sed -n '2,16p' "$0"
      exit 0
      ;;
    *) die "未知参数：$1" ;;
  esac
done

# 检测 GitHub 镜像（与 install_dawn.sh 语义一致）
detect_gh_base() {
  if [ -n "$OPT_MIRROR" ]; then
    printf '%s' "${OPT_MIRROR%/}/"
    return
  fi
  if [ -n "${GDCM_GH_MIRROR:-}" ]; then
    printf '%s' "${GDCM_GH_MIRROR%/}/"
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

GH_BASE="$(detect_gh_base)"
ARCHIVE="GDCM-${GDCM_VERSION}.tar.gz"
# GH_BASE 的语义：直到 `https://github.com/` 结束的前缀（与 install_dawn.sh 一致）。
#   直连        ：https://github.com/
#   镜像        ：https://<镜像>/https://github.com/
URL="${GH_BASE}malaterre/GDCM/archive/refs/tags/v${GDCM_VERSION}.tar.gz"

if [ -f "${GDCM_DIR}/CMakeLists.txt" ] && [ "$FORCE" -ne 1 ]; then
  log "GDCM 已存在：${GDCM_DIR}（加 --force 可重新安装）"
  exit 0
fi

command -v curl >/dev/null || die "需要 curl。"
command -v tar >/dev/null || die "需要 tar。"

mkdir -p "$DEST_DIR"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

log "下载 GDCM v${GDCM_VERSION}"
log "  URL: ${URL}"
if ! curl -fL -A "Mozilla/5.0" --retry 3 -o "${TMP_DIR}/${ARCHIVE}" "$URL"; then
  die "下载失败。可指定镜像：bash scripts/install_gdcm.sh --github-mirror \"<镜像>/https://github.com/\""
fi

if head -c 200 "${TMP_DIR}/${ARCHIVE}" | grep -qi '<!doctype html\|<html'; then
  die "下载到的是 HTML 错误页，镜像可能失效，请更换镜像后重试。"
fi
[ -s "${TMP_DIR}/${ARCHIVE}" ] || die "下载文件为空。"

log "解压到 ${DEST_DIR} ..."
rm -rf "$GDCM_DIR"
tar -xzf "${TMP_DIR}/${ARCHIVE}" -C "$TMP_DIR"
mv "${TMP_DIR}/GDCM-${GDCM_VERSION}" "$GDCM_DIR"

[ -f "${GDCM_DIR}/CMakeLists.txt" ] || die "解压结果缺少 CMakeLists.txt。"
log "完成：${GDCM_DIR}"
log "接下来：cmake -B build -G Ninja && cmake --build build"
