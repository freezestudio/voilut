# ============================================================
# build_windows.ps1 —— Windows 单文件发行构建
# ============================================================
# 做四件事：
#   1) 用 vcpkg 安装静态 TIFF / GDCM（x64-windows-static）
#   2) 用 MSVC /MT + Ninja 构建 VOILUT_PORTABLE + 嵌入着色器
#   3) 检查 exe 是否残留 vcpkg / MSVC 运行库依赖
#   4) 把产物复制到 dist/
#
# 前置要求（均需在 PATH 中）：
#   CMake、Ninja、Python 3、Visual Studio C++ 工具、vcpkg
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts\build_windows.ps1 -VcpkgRoot C:\vcpkg
#   powershell -ExecutionPolicy Bypass -File scripts\build_windows.ps1 `
#       -VcpkgRoot C:\vcpkg -SkipVcpkgInstall
# ============================================================

[CmdletBinding()]
param(
    [string]$BuildDir = "build-release",
    [string]$DistDir = "dist",
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$Triplet = "x64-windows-static",
    [string]$Configuration = "Release",
    [switch]$SkipVcpkgInstall
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

# 临时 .bat 以系统 OEM 代码页写入（与 cmd 默认代码页一致），中文路径也能传进去；
# 若仓库路径含非 ASCII 字符且控制台改过代码页（如 chcp 65001），仍可能乱码。
if ($repoRoot -match '[^\x00-\x7F]') {
    Write-Host "提示: 仓库路径含非 ASCII 字符，若构建报路径错误请移到纯英文路径。" -ForegroundColor Yellow
}

function Fail([string]$message) {
    Write-Host "错误: $message" -ForegroundColor Red
    exit 1
}

function Info([string]$message) {
    Write-Host "==> $message" -ForegroundColor Cyan
}

# ---- 工具检查 ----
foreach ($tool in @("cmake", "ctest", "ninja", "python")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "找不到 $tool，请先安装并加入 PATH。"
    }
}
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    Fail "请设置 VCPKG_ROOT 环境变量，或传入 -VcpkgRoot <vcpkg 目录>。"
}
$vcpkg = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path $vcpkg)) {
    Fail "找不到 $vcpkg。"
}

# ---- Visual Studio ----
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Fail "找不到 vswhere.exe（需要安装 Visual Studio 的 C++ 工具）。"
}
$vsPath = [string](& $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
if ([string]::IsNullOrWhiteSpace($vsPath)) {
    Fail "找不到带 C++ 工具的 Visual Studio。"
}
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    Fail "找不到 $vcvars。"
}

# ---- 静态依赖 ----
if (-not $SkipVcpkgInstall) {
    Info "安装静态依赖（$Triplet）..."
    & $vcpkg install "tiff:$Triplet" "gdcm:$Triplet" "--triplet" $Triplet
    if ($LASTEXITCODE -ne 0) {
        Fail "vcpkg install 失败。"
    }
}

# ---- 配置 + 编译 ----
# 用临时 .bat 调 vcvars64；PowerShell 进程无法直接继承 call 设置的环境变量。
$tempBat = Join-Path $env:TEMP ("voilut-build-" + [Guid]::NewGuid().ToString("N") + ".bat")
$toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
$lines = @(
    "@echo off",
    "call `"$vcvars`" >nul",
    "if errorlevel 1 exit /b 1",
    "set `"VCPKG_ROOT=$VcpkgRoot`"",
    "set `"VSLANG=1033`"",
    "cmake -S `"$repoRoot`" -B `"$BuildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Configuration -DVOILUT_PORTABLE=ON -DVOILUT_EMBED_SHADERS=ON -DCMAKE_TOOLCHAIN_FILE=`"$toolchain`" -DVCPKG_TARGET_TRIPLET=$Triplet -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
    "if errorlevel 1 exit /b 1",
    "cmake --build `"$BuildDir`" --target windower window_model_test --parallel",
    "if errorlevel 1 exit /b 1",
    "ctest --test-dir `"$BuildDir`" -C $Configuration --output-on-failure",
    "if errorlevel 1 exit /b 1"
)
Set-Content -Path $tempBat -Value $lines -Encoding Oem
try {
    Info "配置、编译并运行单元测试..."
    & cmd.exe /c "`"$tempBat`""
    if ($LASTEXITCODE -ne 0) {
        Fail "编译失败。"
    }
} finally {
    Remove-Item $tempBat -Force -ErrorAction SilentlyContinue
}

$exe = Join-Path $BuildDir "windower.exe"
if (-not (Test-Path $exe)) {
    Fail "没有生成 $exe。"
}

# ---- 依赖检查 ----
$msvcRoot = Get-ChildItem (Join-Path $vsPath "VC\Tools\MSVC") -Directory |
    Sort-Object Name -Descending | Select-Object -First 1
$dumpbin = Join-Path $msvcRoot.FullName "bin\Hostx64\x64\dumpbin.exe"
if (Test-Path $dumpbin) {
    Info "检查依赖..."
    $deps = & $dumpbin /nologo /dependents $exe |
        Select-String -Pattern '^\s+([A-Za-z0-9_.-]+\.dll)\s*$' |
        ForEach-Object { $_.Matches[0].Groups[1].Value }
    $bad = $deps | Where-Object {
        $_ -match '^(tiff|gdcm|jpeg|openjp2|libz|zlib|liblzma|libexpat|libwebp|webp|zstd|lerc|jbig|MSVCP140|VCRUNTIME140)'
    }
    if ($bad) {
        Fail ("发现非系统依赖: " + ($bad -join ", "))
    }
    Write-Host ("依赖: " + ($deps -join ", ")) -ForegroundColor DarkGray
} else {
    Write-Host "提示: 未找到 dumpbin，跳过依赖检查。" -ForegroundColor Yellow
}

# ---- 产物 ----
New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
Copy-Item $exe (Join-Path $DistDir "windower.exe") -Force
Info "完成: $(Join-Path $DistDir 'windower.exe')"
