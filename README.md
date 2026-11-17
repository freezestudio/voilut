# voilut

**16 位影像的窗宽窗位（Window Level / Window Width）显示引擎**，附带可执行程序
**Windower**。

医学影像（CT / MR / CR）与科学成像（显微、工业 CT、冷冻电镜）的数据通常是
16 位甚至更高位深，而屏幕只能显示 8 位。**窗宽窗位**就是把这 16 位值域里的
一段区间（窗宽）以某个中心（窗位）线性拉伸到 0~255 的线性映射——
它是影像工作站的核心操作，也是本项目唯一的核心功能。

voilut 把这一流程拆成可复用的库：解码 → 统计 → 定窗 → GPU 映射，
界面（Windower）是这套能力的示例应用。

---

## 一、特性

- **GPU 窗宽窗位映射**：在 WGSL 片元着色器中完成
  `clamp((v - (L - W/2)) / W, 0, 1)`，16 位纹理（`r16uint`）直接显示，
  拖动调窗时无额外拷贝、无 CPU 回读
- **着色器内双线性插值**：`r16uint` 不能直接线性采样，着色器按 4 邻域加权，
  放大观察时保留细节（最近邻 / 双线性可切换）
- **大图与缩图质量**：向 adapter 请求更大的纹理边长（最高 16384，Dawn 默认仅 8192），
  超大 TIFF / DICOM 走区域解码（DICOM 用 `ImageRegionReader`），预览流式降采样；
  放大后按需加载当前视口的 1:1 细节纹理；缩小时按 mipmap LOD 采样，避免大图锯齿
- **浮点影像**：32 位浮点 TIFF（显微 / 科学相机）直接以浮点域参与窗宽窗位计算，
  渲染用 `R32Uint` 存位模式、着色器 `bitcast<f32>` 读取
- **DICOM 模态值**：Rescale Slope/Intercept 贯通窗位/窗宽、探针与 ROI 记录
  （CT 显示 HU）；多组 WindowCenter/WindowWidth 可直接切换
- **后台解码**：解码在后台线程完成，加载大图时界面不冻结，完成后自动显示
- **VOI 公式可选**：默认简化公式；菜单可切换 DICOM PS3.3 精确公式（`W-1` 分母）
- **自动定窗**：65536 桶直方图一次遍历得到 min/max 与 p1/p99，
  默认窗取 `(p1+p99)/2 ± (p99-p1)/2`
- **ROI（感兴趣区域）实时窗**：左键拖动框选区域，按框内统计重新定窗（实时跟随拖动）
- **手动调窗**：Shift+左键拖动，水平调窗宽、垂直调窗位；窄窗时步进自动减小
- **预设窗**：以值域比例定义（全范围 / 宽窗 / 中窗 / 窄窗 / 低值 / 高值），
  跨模态、跨设备通用，无需为每种设备单独配置
- **直方图联动**：柱状图 + 当前窗区间高亮 + p1/p99 参考线，拖动直方图可移动窗位
- **反色与方向**：MONOCHROME1 自动反色、手动反色、旋转 / 翻转（只影响显示）
- **重置图像**：一键清除窗值、反色、插值、方向、缩放平移和 ROI，恢复打开时的状态
- **ROI 记录**：把当前 ROI 以原图坐标保存，操作记录面板显示区域、尺寸与窗值
- **菜单栏**：文件 / 图像 / 视图，支持打开目录、重置、预设窗与面板显隐
- **探针**：悬停显示原始像素值及经窗映射后的显示灰度
- **解码**：TIFF（libtiff，8/16 位灰度、stripped / tiled、多页、大端、MINISWHITE、
  常见压缩）、DICOM（GDCM，含压缩传输语法、WindowCenter/WindowWidth 标签、
  Rescale Slope/Intercept、多组 WindowCenter/WindowWidth 预设、PixelPaddingValue 排除）；
  多页 / 多帧可用滑块或 `,` `.` 逐帧切换（受内存上限）；支持 32 位浮点 TIFF
- **Windows / Linux**：Dawn（Vulkan / D3D12）统一后端，两个平台共用同一套代码
- **高 DPI 布局**：按系统内容缩放同步放大窗口、ImGui 样式、字体与面板固定尺寸，
  4K/200% 下仍保持 1280×720 的逻辑布局和可读字号

---

## 二、快速开始

### 依赖

| 依赖 | 提供方式（三条命令） |
|---|---|
| Dawn | `bash scripts/install_dawn.sh`（Windows：`scripts/install_dawn.ps1`） |
| GDCM | `bash scripts/install_gdcm.sh`（Windows：`vcpkg install gdcm`） |
| Dear ImGui | `bash scripts/install_imgui.sh`（版本固定 1.92.4） |
| libtiff | 系统包（`apt install libtiff-dev`）或 vcpkg |

一次安装全部依赖（约 830 MB；Dawn 及其 19 个第三方依赖耗时 5~20 分钟）：

```bash
bash scripts/install_deps.sh        # = install_dawn.sh + install_gdcm.sh + install_imgui.sh
```

三个脚本都将源码放入 `third_party/`（不入库），均支持
`--github-mirror "<镜像>/https://github.com/"` 或 `GITHUB_MIRROR_URL` 环境变量。
首次配置若 `third_party/dawn` 缺失，CMake 也会自动调用 `install_dawn.sh`
（可用 `-DDAWN_AUTO_INSTALL=OFF` 关闭）。

安装脚本内置两个必需补丁（幂等，可重复执行）：

| 补丁 | 原因 |
|---|---|
| `tint/utils/rtti/traits.h` 补 `#include <sstream>` | 上游缺少头文件，否则编译报 `stringstream is not a member of std` |
| `absl/strings/internal/str_join_internal.h` 把 `uint64_t{X}` 改成 `static_cast<uint64_t>(X)` | GCC 17 在该 constexpr 花括号初始化处触发 ICE（`verify_ctor_sanity`）；语义等价 |

另外脚本会用 pip 安装 jinja2 / markupsafe（Chromium 仓库的私有 commit
无法从 GitHub 获取），并在结束后逐个重新获取网络较慢时被中断的子依赖
（经验证，`glslang`、`spirv-tools` 容易被 `lowSpeedTime` 中断）。

### 构建与运行

```bash
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build              # 纯计算单测 + TIFF 解码回归（无需 GPU）
./build/windower --version         # 版本
./build/windower 图像.tif          # 或 .dcm
./build/windower ./samples/一组图像/  # 目录会展开成图像列表
./build/windower                   # 无参数：窗口内「打开文件夹...」或拖放
```

无依赖快速构建（只编译纯计算库与测试，CI 用）：

```bash
cmake -B build-model -G Ninja -DVOILUT_MODEL_ONLY=ON
cmake --build build-model && ctest --test-dir build-model
```

Windows（MSVC + vcpkg）：

```powershell
vcpkg install tiff gdcm --triplet x64-windows
powershell -ExecutionPolicy Bypass -File scripts\install_dawn.ps1
cmd /c "call `"<VS>\VC\Auxiliary\Build\vcvars64.bat`" >nul && set `"VCPKG_ROOT=C:\vcpkg`" && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build"
```

### 单文件发行构建

默认构建需要把 `shaders/` 放在可执行文件旁。发行版可以只产出一个可执行文件，
运行时不再读取任何随附文件：

```bash
cmake -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DVOILUT_PORTABLE=ON \
  -DVOILUT_EMBED_SHADERS=ON
cmake --build build-release
strip --strip-all build-release/windower    # 可选：去掉调试符号
```

也可以直接用脚本（构建 + CTest + strip + 依赖检查）：

```bash
bash scripts/build_linux.sh        # 产物：dist/windower
```

- `VOILUT_EMBED_SHADERS=ON`：CMake 把 `shaders/*.wgsl` 转成 C++ 字节数组
  （`build-release/generated/embedded_shaders.h`）编进二进制。
  开发时可用环境变量 `VOILUT_SHADERS_DIR=/path/to/shaders` 覆盖，改 WGSL 免重编译。
- `VOILUT_PORTABLE=ON`：libtiff 及其压缩依赖（webp / Lerc / jbig / zstd / lzma /
  jpeg / deflate）、zlib、libstdc++、libgcc 全部静态链接，产物只依赖系统 glibc
  与显卡驱动。

Windows 单文件发行（静态 triplet + `/MT`，`d3dcompiler_47.dll` 由系统提供）：

```powershell
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DVOILUT_PORTABLE=ON -DVOILUT_EMBED_SHADERS=ON `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build-release
```

也可以用脚本完成同样的流程：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_windows.ps1 `
  -VcpkgRoot C:\vcpkg
```

脚本会安装 `x64-windows-static` 的 TIFF / GDCM、配置并构建
`build-release\windower.exe`，检查是否残留 vcpkg / MSVC 运行库依赖，最后复制到
`dist\windower.exe`。依赖已安装时可加 `-SkipVcpkgInstall`。

运行时仍然需要系统环境：

| 平台 | 系统要求 |
|---|---|
| Linux | glibc ≥ 构建机版本；Vulkan 1.1+ 驱动（`libvulkan.so.1`）；X11 或 Wayland 客户端库 |
| Windows | Windows 10+（系统自带 `d3dcompiler_47.dll`）；D3D12 驱动 |

> glibc 不能静态链接：Dawn / GLFW 运行时用 `dlopen` 加载 Vulkan 与窗口系统库，
> 静态 glibc 下会启动即崩溃（已实测）。要控制 glibc 下限，请在目标发行版对应的
> 容器中构建（例如 Ubuntu 22.04 → glibc 2.35）。
>
> Windows 端 CMake 已固定 `DAWN_USE_BUILT_DXC=OFF`：D3D12 走系统自带的
> `d3dcompiler_47.dll`（FXC / Shader Model 5.1），不需要 `dxcompiler.dll`、
> `dxil.dll`。若手动打开该选项，产物就需要额外随包这两个 DLL。
>
> 大图的纹理上限取决于 GPU：程序会请求 adapter 支持的上限（封顶 16384），
> 超过上限的图像会降采样为预览纹理（面板与标题显示 1/N），不会再静默空白。

---

## 三、操作

| 操作 | 效果 |
|---|---|
| **左键拖动** | **框选 ROI，按框内 p1/p99 实时定窗 —— 核心交互** |
| Shift + 左键拖动 | 手动调窗（水平 = 窗宽，垂直 = 窗位） |
| 右键拖动 | 平移 |
| 滚轮 | 以光标为中心缩放 |
| Ctrl + 滚轮 | 窗宽 |
| Shift + 滚轮 | 窗位 |
| ← / → | 窗位微调（按住 Ctrl 时步进更小） |
| ↑ / ↓ 或 `+` / `-` | 窗宽微调（按住 Ctrl 时步进更小） |
| `0` | 重置窗（DICOM 默认窗 → p1/p99 自动窗） |
| `1`…`6` | 预设窗 |
| `Ctrl + O` | 打开文件夹 |
| `Ctrl + R` | 重置图像 |
| `F` | 适应窗口 |
| `I` / `L` | 反色 / 双线性插值 |
| `R` / `H` / `V` | 旋转 90° / 水平翻转 / 垂直翻转 |
| `[` / `]` | 上一张 / 下一张 |
| `,` / `.` | 上一帧 / 下一帧（多页 TIFF、多帧 DICOM，也可拖面板滑块） |
| `Q` / `Esc` / `Ctrl + Q` | 退出 |

面板里还有滑块、数值输入和直方图拖动；勾选「记住窗值」后，
同一会话内每张图像会记住上次使用的窗。

「重置窗」只恢复窗宽窗位；「重置图像」会同时清除反色、插值、旋转翻转、
缩放平移与当前 ROI。当前 ROI 可通过「操作记录」面板保存，记录使用原图坐标，
因此缩放、平移或旋转后仍能准确对应。

菜单栏「视图」可以分别显示或隐藏「窗宽窗位」和「操作记录」面板；
「显示全部面板」/「隐藏全部面板」可一键切换。

菜单栏「图像 → DICOM 精确 VOI」可在两种映射公式之间切换：
默认的简化公式，或 DICOM PS3.3 C.11.2.1.2.1 的线性 VOI LUT 公式。

---

## 四、结构

```
core/      通用基础设施（静态库 voilut_core）
           context       WebGPU 环境（协程式 Instance/Adapter/Device 初始化）
           coroutine     task<T> / async_value<T> / sync_wait
           shader_loader 着色器读取（开发）或内嵌字节（发行）
           window        默认窗口尺寸

imaging/   窗宽窗位核心（静态库 voilut；纯计算部分为 voilut_model）
           image_model    统计、直方图、自动窗、ROI 窗、预设窗、拖动调窗
                          （独立静态库 voilut_model，不链接 Dawn，可无 GPU 测试）
           image_renderer Dawn 渲染管线（uniform 即 WindowParams）
           decode/        TIFF（libtiff）/ DICOM（GDCM）/ 后缀分派
                          region_source 区域读取（虚拟切片 Phase 0）
           viewport       缩放平移；orientation 旋转翻转

apps/      Windower 可执行程序（按职责拆分）
           windower.cxx   入口 / 窗口与 WebGPU 初始化 / 主循环
           app_state.h    共享应用状态
           app_ops.cxx    窗值、视口、ROI、帧切换
           image_io.cxx   播放列表、后台解码、目录选择
           input.cxx      GLFW 输入回调
           ui.cxx         菜单 / 调窗面板 / 直方图 / 叠层
shaders/   image.wgsl  —— 窗宽窗位映射的实现
tests/     window_model_test.cxx —— 调窗与 VOI 公式单测（CTest）
           decode_test.cxx       —— TIFF 解码回归（fixtures 由测试现场生成）
```

数据流：

```
文件 → decode/ → DecodedImage{pixels, meta, default_window}
                        │
                        ├─ compute_stats  → p1/p99 → auto_window
                        ├─ compute_histogram → 直方图界面
                        └─ upload(r16uint) → GPU 纹理
                                              │
                          WindowParams{L,W,invert,linear} uniform
                                              │
                              image.wgsl：值 → 窗映射 → 灰度
```

映射公式（着色器与 CPU 侧 `display_ratio` 双模式完全一致）：

```
简化（默认）：
  lower = level - width / 2
  gray  = clamp((value - lower) / width, 0, 1)

DICOM 精确（菜单「图像 → DICOM 精确 VOI」）：
  center = level - 0.5
  lower  = center - (width - 1) / 2
  upper  = center + (width - 1) / 2
  gray   = clamp((value - center) / (width - 1) + 0.5, 0, 1)
  // width == 1 时退化为阈值：value > level - 0.5 → 1

反色（两种模式一致）：gray = 1 - gray
```

---

## 五、文档

```bash
doxygen Doxyfile        # 或 cmake --build build --target docs
xdg-open build/doxygen/html/index.html
```

头文件注释采用 Doxygen 风格（中文），是查阅接口的主要入口。

---

## 六、许可

本项目代码以 MIT 许可发布（见 `LICENSE`）。第三方依赖遵循各自的许可证：
Dawn（BSD-3-Clause）、GDCM（BSD-3-Clause）、libtiff（libtiff）、Dear ImGui（MIT）。

---

## 七、隐私与数据

- 仓库不包含任何患者图像或 DICOM 数据。
- `.gitignore` 默认忽略 `.dcm` / `.dicom` / `.ima` / `.nii` / `.tif` / `.tiff`
  等医学影像文件，避免误提交。
- 如需加入测试样本，必须先完成去标识化（清除 `PatientName`、`PatientID`、
  `PatientBirthDate`、`InstitutionName` 等标签），再用 `git add -f` 显式加入。
- 提交日志、截图或问题报告前，请确认其中不含本机路径、患者信息或设备标识。
