# ============================================================
# install_dawn.ps1 —— 下载并准备 Dawn 及其全部第三方依赖（Windows）
# ============================================================
# 适用平台：Windows 10/11（Windows PowerShell 5.1 或 PowerShell 7+）
#
# 与 scripts/install_dawn.sh 做同样的事：
#   1) 克隆 Dawn（固定 revision，浅克隆）
#   2) 打一个必需的编译补丁（Tint 缺 <sstream>）
#   3) 获取 19 个第三方依赖（把 chromium.googlesource.com 重写到镜像）
#   4) 用上游等价 commit 替换 Chromium 专有的 abseil
#   5) jinja2/markupsafe 的固定 commit 只存在于 Chromium 仓库，GitHub 镜像
#      无法获取，改用 pip 安装（默认官方 PyPI）
#   6) 校验关键依赖
#
# 用法（在仓库根目录）：
#   powershell -ExecutionPolicy Bypass -File scripts\install_dawn.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\install_dawn.ps1 -GithubMirror "https://xxx/https://github.com/"
#   powershell -ExecutionPolicy Bypass -File scripts\install_dawn.ps1 -Force
#
# 前置要求：Git、Python 3 已加入 PATH。
# ============================================================

[CmdletBinding()]
param(
    [string]$GithubMirror = "",
    [string]$DawnDir = "",
    [string]$DawnRev = "857098c36c748264cf7dbbaca6decba23700202f",
    [string]$AbseilRev = "0661f83775442ad76cd7089b93fd0fd9c91ff694",
    [string]$ExpectedAbseilTree = "2758cf9cfcab3610173ddc4483764467d6454739",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# ---- 路径 ----
$ScriptDir = $PSScriptRoot
$RepoRoot = Split-Path -Parent $ScriptDir
if ([string]::IsNullOrEmpty($DawnDir)) { $DawnDir = Join-Path $RepoRoot "third_party/dawn" }

function Write-Log  { param([string]$m) Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Warn { param([string]$m) Write-Host "警告: $m" -ForegroundColor Yellow }
function Write-Die  { param([string]$m) Write-Host "错误: $m" -ForegroundColor Red; exit 1 }

# ---- 工具检测 ----
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Write-Die "找不到 git，请先安装 Git 并加入 PATH。" }

function Resolve-Python {
    foreach ($c in @("python", "python3", "py")) {
        if (Get-Command $c -ErrorAction SilentlyContinue) {
            try {
                & $c --version *> $null
                if ($LASTEXITCODE -eq 0) { return $c }
            } catch { }
        }
    }
    Write-Die "找不到 Python 3（Dawn 的依赖获取脚本需要它）。"
}
$Python = Resolve-Python

# jinja2 / markupsafe 用 pip 安装时的 PyPI 索引（默认官方 PyPI）
if ([string]::IsNullOrEmpty($env:DAWN_PIP_INDEX)) {
    $PipIndex = "https://pypi.org/simple"
} else {
    $PipIndex = $env:DAWN_PIP_INDEX
}

# ---- 检测 GitHub 镜像 ----
function Get-GhBase {
    if (-not [string]::IsNullOrEmpty($GithubMirror)) {
        return ($GithubMirror.TrimEnd('/')) + "/"
    }
    if (-not [string]::IsNullOrEmpty($env:DAWN_GH_MIRROR)) {
        return ($env:DAWN_GH_MIRROR.TrimEnd('/')) + "/"
    }
    if (-not [string]::IsNullOrEmpty($env:GITHUB_MIRROR_URL)) {
        return ($env:GITHUB_MIRROR_URL.TrimEnd('/')) + "/https://github.com/"
    }
    # 复用 git 全局配置里对 github.com 的 insteadOf 镜像
    $lines = & git config --get-regexp '^url\..*\.insteadof$' 2>$null
    foreach ($line in $lines) {
        $idx = $line.IndexOf(' ')
        if ($idx -lt 0) { continue }
        $key = $line.Substring(0, $idx)
        $val = $line.Substring($idx + 1)
        if ($val -eq "https://github.com/") {
            $key = $key -replace '^url\.', ''
            $key = $key -replace '\.insteadof$', ''
            return $key
        }
    }
    return "https://github.com/"
}

# ---- 安装步骤（使用脚本级变量 $GhBase）----
function Install-CloneDawn {
    Write-Log "克隆 Dawn @ $($DawnRev.Substring(0,12)) ..."
    if (Test-Path $DawnDir) { Remove-Item -Recurse -Force $DawnDir }
    New-Item -ItemType Directory -Path $DawnDir | Out-Null
    & git -C $DawnDir init -q
    & git -C $DawnDir remote add origin "$($GhBase)google/dawn"
    & git -C $DawnDir fetch -q --depth 1 origin $DawnRev 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Warn "镜像获取 Dawn 失败，回退到 github.com 直连"
        & git -C $DawnDir remote set-url origin "https://github.com/google/dawn"
        & git -C $DawnDir fetch -q --depth 1 origin $DawnRev
        if ($LASTEXITCODE -ne 0) { Write-Die "克隆 Dawn 失败。" }
    }
    & git -C $DawnDir checkout -q FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { Write-Die "检出 Dawn 失败（revision 可能不存在）。" }
}

function Install-PatchDawn {
    Write-Log "应用 Dawn 编译补丁 ..."
    $traits = Join-Path $DawnDir "src/tint/utils/rtti/traits.h"
    $py = @'
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
    open(path, "w", encoding="utf-8", newline="").write(src)
    print("  已为 traits.h 补上 #include <sstream>")
'@
    $py | & $Python - $traits
    if ($LASTEXITCODE -ne 0) { Write-Die "打补丁失败。" }
}

function Install-FetchDeps {
    Write-Log "获取 Dawn 第三方依赖（URL 重写到 $GhBase）..."
    $env:GIT_CONFIG_COUNT = "7"
    $env:GIT_CONFIG_KEY_0 = "url.${GhBase}.insteadOf"
    $env:GIT_CONFIG_VALUE_0 = "https://chromium.googlesource.com/external/github.com/"

    $env:GIT_CONFIG_KEY_1 = "url.${GhBase}abseil/abseil-cpp.insteadOf"
    $env:GIT_CONFIG_VALUE_1 = "https://chromium.googlesource.com/chromium/src/third_party/abseil-cpp"
    $env:GIT_CONFIG_KEY_2 = "url.${GhBase}protocolbuffers/protobuf.insteadOf"
    $env:GIT_CONFIG_VALUE_2 = "https://chromium.googlesource.com/chromium/src/third_party/protobuf"
    $env:GIT_CONFIG_KEY_3 = "url.${GhBase}pallets/jinja2.insteadOf"
    $env:GIT_CONFIG_VALUE_3 = "https://chromium.googlesource.com/chromium/src/third_party/jinja2"
    $env:GIT_CONFIG_KEY_4 = "url.${GhBase}pallets/markupsafe.insteadOf"
    $env:GIT_CONFIG_VALUE_4 = "https://chromium.googlesource.com/chromium/src/third_party/markupsafe"

    $env:GIT_CONFIG_KEY_5 = "http.lowSpeedLimit"
    $env:GIT_CONFIG_VALUE_5 = "1000"
    $env:GIT_CONFIG_KEY_6 = "http.lowSpeedTime"
    $env:GIT_CONFIG_VALUE_6 = "15"

    & $Python (Join-Path $DawnDir "tools/fetch_dawn_dependencies.py") -d $DawnDir
    if ($LASTEXITCODE -ne 0) { Write-Warn "依赖获取脚本返回非 0（将继续校验依赖是否齐全）。" }
}

function Install-FixAbseil {
    Write-Log "替换 abseil 为上游等价版本 ..."
    $dest = Join-Path $DawnDir "third_party/abseil-cpp"
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    New-Item -ItemType Directory -Path $dest | Out-Null
    & git -C $dest init -q
    & git -C $dest remote add origin "$($GhBase)abseil/abseil-cpp"
    & git -C $dest fetch -q --depth 1 origin $AbseilRev 2>$null
    if ($LASTEXITCODE -ne 0) {
        & git -C $dest remote set-url origin "https://github.com/abseil/abseil-cpp"
        & git -C $dest fetch -q --depth 1 origin $AbseilRev
        if ($LASTEXITCODE -ne 0) { Write-Die "获取 abseil 失败。" }
    }
    & git -C $dest checkout -q FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { Write-Die "检出 abseil 失败。" }
    $tree = (& git -C $dest rev-parse HEAD:absl).Trim()
    if ($tree -eq $ExpectedAbseilTree) {
        Write-Log "abseil 校验通过（absl 子树一致）"
    } else {
        Write-Warn "abseil absl 子树不一致：$tree != $ExpectedAbseilTree"
    }
}

function Install-PythonDeps {
    $j = Join-Path $DawnDir "third_party/jinja2/__init__.py"
    $m = Join-Path $DawnDir "third_party/markupsafe/__init__.py"
    if ((Test-Path $j) -and (Test-Path $m)) { return }

    Write-Log "jinja2/markupsafe 缺失，改用 pip 安装（镜像：$PipIndex）..."
    & $Python -m pip --version *> $null
    if ($LASTEXITCODE -ne 0) { Write-Die "找不到 pip（$Python -m pip）。请先安装 pip。" }

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("dawn_pydeps_" + [System.Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        & $Python -m pip install --quiet --disable-pip-version-check --target $tmp -i $PipIndex jinja2 markupsafe
        if ($LASTEXITCODE -ne 0) {
            Write-Die "pip 安装 jinja2/markupsafe 失败。可用环境变量 DAWN_PIP_INDEX 指定其它镜像。"
        }

        $jd = Join-Path $DawnDir "third_party/jinja2"
        $md = Join-Path $DawnDir "third_party/markupsafe"
        if (Test-Path $jd) { Remove-Item -Recurse -Force $jd }
        if (Test-Path $md) { Remove-Item -Recurse -Force $md }
        New-Item -ItemType Directory -Path $jd | Out-Null
        New-Item -ItemType Directory -Path $md | Out-Null
        Copy-Item -Path (Join-Path $tmp "jinja2\*") -Destination $jd -Recurse -Force
        Copy-Item -Path (Join-Path $tmp "markupsafe\*") -Destination $md -Recurse -Force
        Get-ChildItem -Path $tmp -Directory -Filter "jinja2-*.dist-info" | ForEach-Object {
            Copy-Item $_.FullName -Destination (Join-Path $DawnDir "third_party") -Recurse -Force
        }
        Get-ChildItem -Path $tmp -Directory -Filter "markupsafe-*.dist-info" | ForEach-Object {
            Copy-Item $_.FullName -Destination (Join-Path $DawnDir "third_party") -Recurse -Force
        }
    } finally {
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    }
    Write-Log "jinja2/markupsafe 安装完成"
}

function Test-Deps {
    $files = @(
        "third_party/abseil-cpp/absl/base/config.h",
        "third_party/glfw3/src/CMakeLists.txt",
        "third_party/spirv-tools/src/CMakeLists.txt",
        "third_party/spirv-headers/src/include/spirv/unified1/spirv.h",
        "third_party/vulkan-headers/src/include/vulkan/vulkan.h",
        "third_party/vulkan-loader/src/CMakeLists.txt",
        "third_party/glslang/src/CMakeLists.txt",
        "third_party/jinja2/__init__.py",
        "third_party/markupsafe/__init__.py",
        "third_party/webgpu-headers/src/webgpu.h"
    )
    $missing = $false
    foreach ($f in $files) {
        if (-not (Test-Path (Join-Path $DawnDir $f))) {
            Write-Warn "缺少依赖文件: $f"
            $missing = $true
        }
    }
    return (-not $missing)
}

# ---- 主流程 ----
$marker = Join-Path $DawnDir ".dawn-install-revision"
if (-not $Force -and (Test-Path (Join-Path $DawnDir "CMakeLists.txt")) -and (Test-Path $marker)) {
    if ((Get-Content $marker -Raw).Trim() -eq $DawnRev) {
        # 即使 Dawn 已就绪，也补充安装早期版本脚本可能遗漏的 jinja2/markupsafe
        Install-PythonDeps
        Write-Log "Dawn 已就绪（$DawnRev），无需重新安装。需要重新安装请加 -Force。"
        exit 0
    }
}

$GhBase = Get-GhBase
Write-Log "使用镜像基址：$GhBase"

function Install-All {
    Install-CloneDawn
    Install-PatchDawn
    Install-FetchDeps
    Install-FixAbseil
    Install-PythonDeps
}

Install-All
if (-not (Test-Deps)) {
    if ($GhBase -ne "https://github.com/") {
        Write-Warn "镜像可能不完整，改用 github.com 直连重试 ..."
        $GhBase = "https://github.com/"
        Install-All
    }
}
if (-not (Test-Deps)) { Write-Die "依赖仍不完整，请检查网络后重试（可加 -GithubMirror 指定镜像）。" }

Set-Content -Path $marker -Value $DawnRev -NoNewline

Write-Log "完成！下一步："
Write-Host "  cmake -B build -G Ninja"
Write-Host "  cmake --build build"
Write-Host "  .\build\windower.exe"
