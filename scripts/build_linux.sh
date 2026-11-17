#!/usr/bin/env bash
# ============================================================
# build_linux.sh —— Linux 单文件发行构建
# ============================================================
# 与 build_windows.ps1 对应：
#   1) 检查 third_party 依赖（缺失时给出安装命令）
#   2) 配置 Release + VOILUT_PORTABLE + VOILUT_EMBED_SHADERS
#   3) 构建并运行 CTest
#   4) strip 后复制到 dist/windower，并检查是否残留第三方动态库
#
# 用法：
#   bash scripts/build_linux.sh
#   BUILD_DIR=build-release DIST_DIR=dist bash scripts/build_linux.sh
# ============================================================
set -euo pipefail

cd "$(dirname "$0")/.."
BUILD_DIR="${BUILD_DIR:-build-release}"
DIST_DIR="${DIST_DIR:-dist}"
CONFIG="${CONFIG:-Release}"

fail() {
    echo "错误: $*" >&2
    exit 1
}

info() {
    echo "==> $*"
}

for tool in cmake ninja; do
    command -v "$tool" >/dev/null 2>&1 || fail "找不到 $tool，请先安装并加入 PATH。"
done

# ---- 依赖检查 ----
missing=0
for path in third_party/dawn/CMakeLists.txt third_party/GDCM/CMakeLists.txt third_party/imgui/imgui.h; do
    if [[ ! -e "$path" ]]; then
        echo "缺少 $path"
        missing=1
    fi
done
if [[ "$missing" -ne 0 ]]; then
    fail "请先运行：bash scripts/install_deps.sh"
fi

if ! pkg-config --exists libtiff-4; then
    fail "缺少 libtiff 开发包（如 apt install libtiff-dev），VOILUT_PORTABLE 需要静态依赖链。"
fi

# ---- 配置 / 编译 / 测试 ----
info "配置（Release + 静态依赖 + 嵌入着色器）..."
cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DVOILUT_PORTABLE=ON \
    -DVOILUT_EMBED_SHADERS=ON

info "编译..."
cmake --build "$BUILD_DIR" --parallel

info "运行单元测试..."
ctest --test-dir "$BUILD_DIR" --output-on-failure

EXE="$BUILD_DIR/windower"
[[ -f "$EXE" ]] || fail "没有生成 $EXE"

# ---- 去除调试符号 ----
if command -v strip >/dev/null 2>&1; then
    info "strip 调试符号..."
    strip --strip-all "$EXE"
fi

# ---- 依赖检查（只允许 glibc / 系统库）----
if command -v readelf >/dev/null 2>&1; then
    deps="$(readelf -d "$EXE" | sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p' | tr '\n' ' ')"
    bad="$(printf '%s\n' "$deps" | tr ' ' '\n' |
        grep -E '^(libtiff|libz|libjpeg|libwebp|liblzma|libzstd|libLerc|libdeflate|libjbig|libstdc\+\+|libgcc_s)' || true)"
    if [[ -n "$bad" ]]; then
        fail "发现非系统动态依赖：$(printf '%s' "$bad" | tr '\n' ' ')"
    fi
    info "动态依赖: $deps"
else
    echo "提示: 未找到 readelf，跳过依赖检查。"
fi

# ---- 产物 ----
mkdir -p "$DIST_DIR"
cp "$EXE" "$DIST_DIR/windower"
chmod +x "$DIST_DIR/windower"

size="$(stat -c %s "$DIST_DIR/windower" | numfmt --to=iec-i --suffix=B)"
info "完成: $DIST_DIR/windower（$size）"
if command -v sha256sum >/dev/null 2>&1; then
    (cd "$DIST_DIR" && sha256sum windower)
fi
info "运行时要求：glibc ≥ 构建机版本；Vulkan 1.1+ 驱动；X11 或 Wayland 客户端库"
