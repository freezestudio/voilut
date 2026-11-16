#!/usr/bin/env bash
# ============================================================
# install_dawn.sh —— 下载并准备 Dawn 及其全部第三方依赖
# ============================================================
# 适用平台：Linux / macOS（bash 3.2+，兼容 macOS 自带旧版 bash）
#
# 【为什么需要这个脚本】
#   Dawn 是第三方依赖（约 176 MB / 7 万个文件），不纳入本仓库。
#   Dawn 的依赖托管在 chromium.googlesource.com，该域名在部分网络环境下
#   不可达，使用官方 gclient / fetch_dawn_dependencies 会长时间无响应。
#
# 【做法（已验证）】
#   1) 克隆 Dawn 到本仓库的 third_party/dawn/（固定 revision，浅克隆，约 11 秒）
#   2) 打一个必需的编译补丁（Tint 缺 <sstream> 头文件）
#   3) 用 Dawn 官方脚本获取 19 个第三方依赖（约 48 秒）
#   4) 把 Chromium 专有的 abseil 换成内容完全一致的上游 commit
#   5) jinja2/markupsafe 的固定 commit 只存在于 Chromium 仓库，GitHub 镜像
#      无法获取，改用 pip 安装（默认官方 PyPI）
#   6) 校验关键依赖是否齐全
#
# 【加速原理】
#   用 git 的 url.<镜像>.insteadOf 把 chromium.googlesource.com 的
#   依赖地址直接重写到可访问的 GitHub 镜像。
#   注意：git 的 insteadOf 不会"链式"重写，因此必须直接重写到镜像，
#   不能先重写为 github.com 再依赖全局配置二次转发（否则会使用较慢的直连）。
#
# 【用法】
#   scripts/install_dawn.sh                      # 自动检测镜像
#   scripts/install_dawn.sh --github-mirror URL  # 手动指定镜像
#   scripts/install_dawn.sh --force              # 强制重新安装
#   DAWN_GH_MIRROR=URL scripts/install_dawn.sh   # 环境变量指定镜像
#
# 镜像 URL 格式（与 GITHUB_MIRROR_URL 约定一致）：
#   <镜像>/https://github.com/       例如 https://<镜像域名>/https://github.com/
# 若 git 全局配置中已有 "url.<X>.insteadOf = https://github.com/"，
# 脚本会自动识别并复用 <X>。
# ============================================================

set -euo pipefail

# ------------------------------------------------------------
# 固定版本（与仓库中 third_party/dawn/ 验证过的版本一致，请勿随意修改）
# ------------------------------------------------------------
DEFAULT_DAWN_REV="857098c36c748264cf7dbbaca6decba23700202f"   # Dawn 2026-09-06
DEFAULT_ABSEIL_REV="0661f83775442ad76cd7089b93fd0fd9c91ff694" # abseil 上游等价 commit
EXPECTED_ABSEIL_TREE="2758cf9cfcab3610173ddc4483764467d6454739" # abseil/absl 子树哈希

# jinja2 / markupsafe 用 pip 安装时的 PyPI 索引（默认官方 PyPI，可用环境变量覆盖）
PIP_INDEX="${DAWN_PIP_INDEX:-https://pypi.org/simple}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ------------------------------------------------------------
# 参数
# ------------------------------------------------------------
OPT_MIRROR=""
OPT_FORCE=0
DAWN_DIR="$REPO_ROOT/third_party/dawn"
DAWN_REV="$DEFAULT_DAWN_REV"
ABSEIL_REV="$DEFAULT_ABSEIL_REV"

usage() {
  sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m警告:\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m错误:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --github-mirror) OPT_MIRROR="${2:-}"; [ -n "$OPT_MIRROR" ] || die "--github-mirror 需要一个参数"; shift 2 ;;
    --dawn-dir)      DAWN_DIR="${2:-}";   [ -n "$DAWN_DIR" ]   || die "--dawn-dir 需要一个参数"; shift 2 ;;
    --dawn-rev)      DAWN_REV="${2:-}";   [ -n "$DAWN_REV" ]   || die "--dawn-rev 需要一个参数"; shift 2 ;;
    --force)         OPT_FORCE=1; shift ;;
    -h|--help)       usage; exit 0 ;;
    *) die "未知参数: $1（用 --help 查看用法）" ;;
  esac
done

# ------------------------------------------------------------
# 工具检测
# ------------------------------------------------------------
command -v git >/dev/null 2>&1 || die "找不到 git，请先安装 Git。"
PY=""
for c in python3 python; do
  if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
done
[ -n "$PY" ] || die "找不到 Python 3（Dawn 的依赖获取脚本需要它）。"

# ------------------------------------------------------------
# 检测 GitHub 镜像
# ------------------------------------------------------------
detect_gh_base() {
  if [ -n "$OPT_MIRROR" ]; then
    printf '%s' "${OPT_MIRROR%/}/"; return
  fi
  if [ -n "${DAWN_GH_MIRROR:-}" ]; then
    printf '%s' "${DAWN_GH_MIRROR%/}/"; return
  fi
  if [ -n "${GITHUB_MIRROR_URL:-}" ]; then
    printf '%s' "${GITHUB_MIRROR_URL%/}/https://github.com/"; return
  fi
  # 复用 git 全局配置里对 github.com 的 insteadOf 镜像
  local line key val base=""
  while IFS= read -r line; do
    key="${line%% *}"; val="${line#* }"
    if [ "$val" = "https://github.com/" ]; then
      key="${key#url.}"
      key="${key%.insteadof}"
      key="${key%.insteadOf}"
      base="$key"; break
    fi
  done < <(git config --get-regexp '^url\..*\.insteadof$' 2>/dev/null || true)
  if [ -n "$base" ]; then printf '%s' "$base"; else printf '%s' "https://github.com/"; fi
}

# ------------------------------------------------------------
# 三个安装步骤（都使用全局变量 GH_BASE）
# ------------------------------------------------------------

# 1) 克隆 Dawn（浅克隆指定 revision；镜像失败则回退直连）
clone_dawn() {
  log "克隆 Dawn @ ${DAWN_REV:0:12} ..."
  rm -rf "$DAWN_DIR"
  mkdir -p "$DAWN_DIR"
  git -C "$DAWN_DIR" init -q
  git -C "$DAWN_DIR" remote add origin "${GH_BASE}google/dawn"
  if ! git -C "$DAWN_DIR" fetch -q --depth 1 origin "$DAWN_REV"; then
    warn "镜像 ${GH_BASE} 获取 Dawn 失败，回退到 github.com 直连"
    git -C "$DAWN_DIR" remote set-url origin "https://github.com/google/dawn"
    git -C "$DAWN_DIR" fetch -q --depth 1 origin "$DAWN_REV"
  fi
  git -C "$DAWN_DIR" checkout -q FETCH_HEAD
}

# 2) 编译补丁：Tint 的 traits.h 少一个 <sstream> 头文件
#    （本仓库 third_party/dawn/ 中同样打了这个补丁，否则编译报
#     error: 'stringstream' is not a member of 'std'）
patch_dawn() {
  log "应用 Dawn 编译补丁 ..."
  "$PY" - "$DAWN_DIR/src/tint/utils/rtti/traits.h" <<'PYEOF'
import sys
path = sys.argv[1]
src = open(path, encoding="utf-8").read()
if "#include <sstream>" in src:
    print("  traits.h 补丁已存在，跳过")
else:
    if "#include <ostream>\n" not in src:
        sys.exit("  未找到插入点 #include <ostream>，Dawn 版本可能不匹配")
    src = src.replace("#include <ostream>\n",
                      "#include <ostream>\n#include <sstream>\n", 1)
    open(path, "w", encoding="utf-8").write(src)
    print("  已为 traits.h 补上 #include <sstream>")
PYEOF
}

# 3) 获取第三方依赖：把 chromium.googlesource.com 重写到镜像
fetch_deps() {
  log "获取 Dawn 第三方依赖（URL 重写到 ${GH_BASE}）..."
  export GIT_CONFIG_COUNT=7
  export GIT_CONFIG_KEY_0="url.${GH_BASE}.insteadOf"
  export GIT_CONFIG_VALUE_0="https://chromium.googlesource.com/external/github.com/"

  # 下面 4 个在 Chromium 仓库里，需要逐个映射到对应的 GitHub 上游
  export GIT_CONFIG_KEY_1="url.${GH_BASE}abseil/abseil-cpp.insteadOf"
  export GIT_CONFIG_VALUE_1="https://chromium.googlesource.com/chromium/src/third_party/abseil-cpp"
  export GIT_CONFIG_KEY_2="url.${GH_BASE}protocolbuffers/protobuf.insteadOf"
  export GIT_CONFIG_VALUE_2="https://chromium.googlesource.com/chromium/src/third_party/protobuf"
  export GIT_CONFIG_KEY_3="url.${GH_BASE}pallets/jinja2.insteadOf"
  export GIT_CONFIG_VALUE_3="https://chromium.googlesource.com/chromium/src/third_party/jinja2"
  export GIT_CONFIG_KEY_4="url.${GH_BASE}pallets/markupsafe.insteadOf"
  export GIT_CONFIG_VALUE_4="https://chromium.googlesource.com/chromium/src/third_party/markupsafe"

  # 低速持续 15 秒即断开，避免访问受限域名时长时间挂起
  export GIT_CONFIG_KEY_5="http.lowSpeedLimit"
  export GIT_CONFIG_VALUE_5="1000"
  export GIT_CONFIG_KEY_6="http.lowSpeedTime"
  export GIT_CONFIG_VALUE_6="15"

  "$PY" "$DAWN_DIR/tools/fetch_dawn_dependencies.py" -d "$DAWN_DIR" || warn "依赖获取脚本返回非 0（将继续校验依赖是否齐全）"
}

# 3.5) 逐个重新获取缺失的子依赖。
#      fetch_dawn_dependencies.py 在网络较慢时会因 lowSpeedTime=15 断开，
#      导致个别仓库（经验证是 glslang / spirv-tools）仅留下空目录。
#      这里按 DEPS 里的 url@rev 重新获取，每个仓库最多尝试 3 次。
ensure_missing_deps() {
  log "检查并重新获取缺失的子依赖 ..."
  local tuples rel repo rev dest attempt
  tuples="$("$PY" - "$DAWN_DIR/DEPS" <<'PYEOF'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
# 只处理形如 'third_party/x/src': { 'url': '{chromium_git}/external/github.com/owner/repo@<sha>' }
pat = re.compile(
    r"'([^']+)'\s*:\s*\{[^{}]*?'url'\s*:\s*'\{chromium_git\}/external/github\.com/([^'@]+)@([0-9a-f]{40})'",
    re.S)
for rel, repo, rev in pat.findall(src):
    print(f"{rel}\t{repo}\t{rev}")
PYEOF
)"
  while IFS=$'\t' read -r rel repo rev; do
    [ -n "${rel:-}" ] || continue
    # 只获取编译必需的子依赖。DEPS 里还有 llvm / clang-format / emsdk 等
    # 可选大仓库，本项目不需要，全部获取会额外下载数百 MB。
    case "$rel" in
      third_party/glfw3/src | third_party/spirv-tools/src | third_party/spirv-headers/src | \
      third_party/vulkan-headers/src | third_party/vulkan-loader/src | \
      third_party/glslang/src | third_party/webgpu-headers/src) ;;
      *) continue ;;
    esac
    dest="$DAWN_DIR/$rel"
    # 目录里有内容（不只有 .git）就跳过
    if [ -d "$dest" ] && [ -n "$(ls -A "$dest" 2>/dev/null | grep -v '^\\.git$' || true)" ]; then
      continue
    fi
    log "  重新获取 $rel @ ${rev:0:12}"
    attempt=0
    while [ "$attempt" -lt 3 ]; do
      attempt=$((attempt + 1))
      rm -rf "$dest"
      mkdir -p "$dest"
      git -C "$dest" init -q
      git -C "$dest" remote add origin "${GH_BASE}${repo}"
      if git -C "$dest" fetch -q --depth 1 origin "$rev" \
         && git -C "$dest" checkout -q FETCH_HEAD; then
        break
      fi
      warn "  $rel 第 $attempt 次获取失败，重试 ..."
      sleep 2
    done
  done <<< "$tuples"
}

# 4) abseil：Chromium 专有 commit 无法获取，换成内容一致的上游 commit
fix_abseil() {
  log "替换 abseil 为上游等价版本 ..."
  local dest="$DAWN_DIR/third_party/abseil-cpp"
  rm -rf "$dest"
  mkdir -p "$dest"
  git -C "$dest" init -q
  git -C "$dest" remote add origin "${GH_BASE}abseil/abseil-cpp"
  if ! git -C "$dest" fetch -q --depth 1 origin "$ABSEIL_REV"; then
    git -C "$dest" remote set-url origin "https://github.com/abseil/abseil-cpp"
    git -C "$dest" fetch -q --depth 1 origin "$ABSEIL_REV"
  fi
  git -C "$dest" checkout -q FETCH_HEAD
  local tree
  tree="$(git -C "$dest" rev-parse HEAD:absl)"
  if [ "$tree" = "$EXPECTED_ABSEIL_TREE" ]; then
    log "abseil 校验通过（absl 子树一致）"
  else
    warn "abseil absl 子树不一致：$tree != $EXPECTED_ABSEIL_TREE"
  fi
}

# 4.5) abseil 编译补丁：新版 abseil 在 constexpr 里用花括号初始化
#      `uint64_t{(std::numeric_limits<size_t>::max)()}`，GCC 17 会在这里
#      触发编译器内部错误（internal compiler error: in verify_ctor_sanity,
#      at cp/constexpr.cc）。改成等价的 static_cast 写法即可。
#      语义完全相同，其它编译器不受影响。
patch_abseil() {
  log "应用 abseil 编译补丁（GCC 17 ICE）..."
  local f="$DAWN_DIR/third_party/abseil-cpp/absl/strings/internal/str_join_internal.h"
  if [ ! -f "$f" ]; then
    warn "未找到 str_join_internal.h，跳过 abseil 补丁"
    return 0
  fi
  "$PY" - "$f" <<'PYEOF'
import sys
path = sys.argv[1]
src = open(path, encoding="utf-8").read()
old = "uint64_t{(std::numeric_limits<size_t>::max)()}"
new = "static_cast<uint64_t>((std::numeric_limits<size_t>::max)())"
if old not in src:
    print("  abseil 补丁无需应用（写法已变或已打过）")
else:
    open(path, "w", encoding="utf-8").write(src.replace(old, new))
    print("  已把 uint64_t{...} 改为 static_cast<uint64_t>(...)，避开 GCC 17 ICE")
PYEOF
}

# 5) jinja2 / markupsafe：DEPS 里锁定的是 Chromium 仓库的私有 commit，
#    GitHub 镜像（pallets/jinja2 等）并没有这些提交，git 获取会留下空仓库。
#    这里改用 pip 安装，再按 Dawn 期望的目录布局放置（Dawn 会把包的父目录加进
#    sys.path，所以 third_party/jinja2 本身就要是 jinja2 包）。
ensure_python_deps() {
  local j="$DAWN_DIR/third_party/jinja2"
  local m="$DAWN_DIR/third_party/markupsafe"
  if [ -f "$j/__init__.py" ] && [ -f "$m/__init__.py" ]; then
    return 0
  fi

  log "jinja2/markupsafe 缺失，改用 pip 安装（镜像：${PIP_INDEX}）..."
  "$PY" -m pip --version >/dev/null 2>&1 \
    || die "找不到 pip（$PY -m pip）。请先安装 pip，或手动放置 jinja2/markupsafe。"

  local tmp
  tmp="$(mktemp -d)"
  if ! "$PY" -m pip install --quiet --disable-pip-version-check --target "$tmp" \
        -i "$PIP_INDEX" jinja2 markupsafe; then
    rm -rf "$tmp"
    die "pip 安装 jinja2/markupsafe 失败。可用 DAWN_PIP_INDEX 指定其它 PyPI 镜像。"
  fi

  rm -rf "$j" "$m"
  mkdir -p "$j" "$m"
  cp -R "$tmp/jinja2/." "$j/"
  cp -R "$tmp/markupsafe/." "$m/"
  # 附带 dist-info，保证 importlib.metadata.version() 可用
  cp -R "$tmp"/jinja2-*.dist-info "$DAWN_DIR/third_party/" 2>/dev/null || true
  cp -R "$tmp"/markupsafe-*.dist-info "$DAWN_DIR/third_party/" 2>/dev/null || true
  rm -rf "$tmp"
  log "jinja2/markupsafe 安装完成"
}

# 6) 校验关键依赖
verify() {
  local f missing=0
  for f in \
    third_party/abseil-cpp/absl/base/config.h \
    third_party/glfw3/src/CMakeLists.txt \
    third_party/spirv-tools/src/CMakeLists.txt \
    third_party/spirv-headers/src/include/spirv/unified1/spirv.h \
    third_party/vulkan-headers/src/include/vulkan/vulkan.h \
    third_party/vulkan-loader/src/CMakeLists.txt \
    third_party/glslang/src/CMakeLists.txt \
    third_party/jinja2/__init__.py \
    third_party/markupsafe/__init__.py \
    third_party/webgpu-headers/src/webgpu.h ; do
    if [ ! -f "$DAWN_DIR/$f" ]; then
      warn "缺少依赖文件: $f"
      missing=1
    fi
  done
  return $missing
}

# ------------------------------------------------------------
# 主流程
# ------------------------------------------------------------
# 已安装则跳过（按 revision 判断）。即使 Dawn 已就绪，也要确保
# jinja2/markupsafe 存在——早期版本脚本可能未安装这两个依赖。
if [ "$OPT_FORCE" -eq 0 ] \
   && [ -f "$DAWN_DIR/CMakeLists.txt" ] \
   && [ "$(cat "$DAWN_DIR/.dawn-install-revision" 2>/dev/null || true)" = "$DAWN_REV" ]; then
  ensure_python_deps
  log "Dawn 已就绪（$DAWN_REV），无需重新安装。需要重新安装请加 --force。"
  exit 0
fi

GH_BASE="$(detect_gh_base)"
log "使用镜像基址：$GH_BASE"

install_all() {
  clone_dawn
  patch_dawn
  fetch_deps
  ensure_missing_deps
  fix_abseil
  patch_abseil
  ensure_python_deps
}

install_all
if ! verify; then
  if [ "$GH_BASE" != "https://github.com/" ]; then
    warn "镜像可能不完整，改用 github.com 直连重试 ..."
    GH_BASE="https://github.com/"
    install_all
  fi
fi
verify || die "依赖仍不完整，请检查网络后重试（可加 --github-mirror 指定镜像）。"

echo "$DAWN_REV" > "$DAWN_DIR/.dawn-install-revision"

log "完成！下一步："
echo "  cmake -B build -G Ninja"
echo "  cmake --build build"
echo "  ./build/windower"
