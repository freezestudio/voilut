# AGENTS.md —— voilut 仓库的代理约定

> 记录对自动化代理（agent）的长期约定，防止跨会话遗忘。
> 人类可读，但主要服务于在本仓库工作的 AI 代理。

## 项目概述

- 项目名：`voilut`（DICOM 的 *VOI LUT* 就是窗宽窗位这条映射，故名）
- 应用名：`Windower`，可执行文件 `windower`
- 语言 / 标准：C++26（GCC，`-std=c++26`；MSVC 走 C++23 档映射 `/std:c++latest`）
- 图形：Dawn（WebGPU 原生实现）+ GLFW；着色器 WGSL（`shaders/`）
- 构建：CMake + Ninja
  - 配置：`cmake -B build -G Ninja`
  - 编译：`cmake --build build`
  - 运行：`./build/windower <图像或目录>`
- 注释：中文，Doxygen（`///`、`@brief` / `@param` / `@return` / `@note`）
- API 文档：`doxygen Doxyfile` 或 `cmake --build build --target docs`
  → `build/doxygen/html/index.html`

## 核心定位（重要）

- 本项目的**核心功能只有一项：16 位影像的窗宽窗位显示映射**。
  新增功能时应先判断：它是否服务于定窗、调窗或理解窗参数的映射行为？
  若无关且实现成本较高（标注、序列管理、测量工具、导出管线），不应纳入核心路径。
- 允许的周边能力：解码、视口缩放平移、方向变换、探针读数、直方图。
  这些都是为了让调窗更可靠，属于同一目标。

## 目录约定

- `core/`：与影像无关的基础设施（WebGPU 环境、协程、WGSL 读取、窗口常量）。
  目标 `voilut_core`。这里**不放**任何窗宽窗位业务逻辑。
- `imaging/`：核心库（解码 → 统计 → 定窗 → GPU 渲染）。目标 `voilut`。
  - `image_model.*`：纯计算（统计、直方图、自动窗、ROI 窗、预设窗、拖动调窗），
    必须可在无 GPU 环境下单独测试。
  - `image_renderer.*`：Dawn 管线；`WindowParams` 必须与 `shaders/image.wgsl`
    的 `WindowParams` 逐字段一致（32 字节，补齐 padding）。
  - `decode/`：格式分派 + TIFF（libtiff）+ DICOM（GDCM）。
- `apps/`：可执行入口。UI 只调库，不重复实现映射逻辑。
- `shaders/`：WGSL。**映射公式改动必须同时改 CPU 侧**（`display_ratio`），
  并保持注释里互相引用的说法一致。
- `third_party/`：不入库（见 `.gitignore`）。Dawn / GDCM 由 `scripts/*.sh` 获取。

## 命名与 API 约定

- 命名空间统一 `voilut`（不再有 `app::viewer` 前缀）。
- 窗宽窗位的术语固定为：`level`（窗位）、`width`（窗宽）、
  `WindowSetting{level, width}`；着色器里是 `WindowParams{level, width, …}`。
- 纯计算函数保持 `[[nodiscard]]` + `noexcept`（尽量全部添加），
  错误用 `std::expected<T, std::string>`，不抛异常。
- 跨平台差异必须保留现有处理，请勿移除：
  - `= delete("原因")` → 用 `APP_DELETE_REASON`（`core/include/coroutine.h`）
  - `path::display_string()` → 用 `voilut::path_display()`
  - 目录选择：Windows 走 COM `IFileOpenDialog`，Linux 走 `zenity`
  - Windows 需要 `d3dcompiler_47.dll`（CMake 已自动拷贝）

## 验证要求

- 代码改动后必须 `cmake --build build` 通过，无新增警告。
- 涉及窗宽窗位数值语义时，至少验证：自动窗、ROI 窗、拖动调窗、预设窗
  四条路径都能改变显示，且 VOI 两种公式的边界值正确。前四项与 VOI 已有 CTest 覆盖，
  先跑 `ctest --test-dir build --output-on-failure`（window_model_test + decode_test）；
  涉及渲染时再用真实 TIFF/DICOM 图像跑一次 `./build/windower <图像>`。
- 无依赖环境（CI）用 `cmake -B build -DVOILUT_MODEL_ONLY=ON`，只构建 
  `voilut_model` + `window_model_test`，秒级完成。
- 纯计算逻辑一律放进 `voilut_model`（`imaging/src/image_model.cxx`），
  不要依赖 Dawn / 窗口系统，以便无 GPU 环境下测试。
- 高 DPI（4K/200%）下 UI 尺寸按系统 content scale 缩放：窗口创建后调用
  `window_ui_scale()` 放大窗口，ImGui 样式用 `ScaleAllSizes()` + `FontScaleDpi`，
  所有 96 DPI 设计尺寸（面板宽、边距、固定列宽、直方图高度、线宽等）必须经
  `ui_px()` 换算。新加布局常量时不要直接写物理像素，否则高 DPI 下只有一半大小。
  Surface 初始配置必须用 `glfwGetFramebufferSize()`，不能用 `kWidth` / `kHeight`。
- 头文件注释改动后跑 `doxygen Doxyfile`，确认无 `WARN_IF_DOC_ERROR` 报警。
- 依赖不全时一律使用 `bash scripts/install_deps.sh`（或单独运行其中某个脚本）：
  它会安装 jinja2/markupsafe，并在网络较慢时逐个重新获取被中断的子依赖。
- 已知工具链问题（脚本已应用补丁，请勿删除）：
  - GCC 17 对 `uint64_t{X}` 形式的 constexpr 花括号初始化会 ICE，abseil 里那处
    必须保持 `static_cast<uint64_t>(X)` 写法。
  - Dawn 的 `tint/utils/rtti/traits.h` 必须补 `#include <sstream>`。
- Windows / MSVC 的已知约束（已处理，修改时不要回退）：
  - 源码是 UTF-8 无 BOM，项目目标必须带 `/utf-8`（`CMakeLists.txt` 已加）；
    否则中文区域按 GBK 解析，会把中文注释当成语法错误。
  - `scripts/*.ps1` 保存为 UTF-8 with BOM：Windows PowerShell 5.1 会把无 BOM
    的 UTF-8 按 ANSI 解析，导致脚本解析失败。`.gitattributes` 已放行 BOM。
  - `std::setvbuf(stdout, nullptr, _IOLBF, 0)` 在 MSVC 上会触发 invalid parameter
    并直接终止进程，`size` 必须 > 0。
  - 中文路径：libtiff 的 `TIFFOpen` 按 ANSI 解释路径，Windows 下必须用
    `TIFFOpenW`（`tiff_decode.cxx` 已处理）；GDCM 当前可直接接受 UTF-8。
- 按需渲染与 ImGui 的两个已知问题（已修复，修改代码时不要回退）：
  - 面板尺寸不要使用自动适应（高度为 0）：首帧之后程序阻塞在 `glfwWaitEvents`，
    没有事件就不会重绘，窗口会停在只有标题栏、高度异常的状态。请使用固定尺寸。
  - 主循环前 3 帧使用 `glfwPollEvents` 预热，让字体图集与布局稳定。
- `std::println` 在 stdout 重定向时是全缓冲：`main` 中已设置行缓冲，
  否则日志文件里看不到「已加载 …」。

## 窗宽窗位映射与显示

- `WindowParams`（32 字节）与 `shaders/image.wgsl` 逐字段一致；`mode` 字段区分
  简化公式（默认）与 DICOM 精确公式（C.11.2.1.2.1）。改公式必须同时改
  `display_ratio` 与 WGSL，并在 `tests/window_model_test.cxx` 覆盖两种模式。
- 大图：`Context` 向 adapter 请求 `min(adapter.maxTextureDimension2D, 16384)`；
  `ImageRenderer::upload` 返回 `expected`，超限/内存不足必须走错误弹窗，不得忽略。
- mipmap 由 CPU 盒式滤波生成（`r16uint` 不能由采样器生成），着色器按 UV 导数选 LOD；
  缩图质量依赖这条链，不要回退成只读 level 0。
- TIFF：tiled、多页计数、大端字节交换、`PHOTOMETRIC_MINISWHITE` 均已处理；
  多帧 DICOM 必须按 `GetBufferLength()` 全量分配（目前只显示第 1 帧），
  按单帧分配会堆溢出。浮点 TIFF 明确拒绝。

## 发行构建（单文件）

- 单文件产物：`-DVOILUT_PORTABLE=ON -DVOILUT_EMBED_SHADERS=ON -DCMAKE_BUILD_TYPE=Release`，
  产物目录只有一个 `windower`（Linux strip 后约 19 MB）。
- shader 嵌入由 CMake 生成 `build/generated/embedded_shaders.h`（不用 `#embed`：
  MSVC 支持度不确定；GCC 17 已支持 `#embed`，二者等价）。新增着色器时要在
  `VOILUT_EMBEDDED_SHADERS`（文件名=数组名）里登记。开发时用环境变量
  `VOILUT_SHADERS_DIR` 覆盖，改 WGSL 免重编译。
- **glibc 不能静态化**（实测：Dawn / GLFW 用 `dlopen` 加载 libvulkan / libX11 /
  libwayland，全静态 `-static` 启动即崩溃）；`libm` / `libpthread` 也必须保持
  动态（`libm.a` 引用 glibc 内部符号 `_dl_x86_cpu_features`）。静态化范围只到
  libtiff 及其压缩依赖、zlib、libstdc++、libgcc。发行前请确认 `readelf -d` 的
  `NEEDED` 只剩 `libm.so.6` / `libc.so.6` / `ld-linux`。
- 单文件仍依赖系统显卡驱动（Vulkan ICD），无法随包分发；glibc 下限 = 构建机
  版本，要兼容旧发行版请在对应容器里构建。
- Windows：CMake 已固定 `DAWN_USE_BUILT_DXC=OFF`，D3D12 用系统 `d3dcompiler_47.dll`
  （FXC / SM 5.1）；不要打开它——会额外依赖 `dxcompiler.dll` 与 `dxil.dll`，
  单文件发行就破了。便携构建同时要求 `/MT`（CMake 已自动设置）与静态 vcpkg
  triplet（`x64-windows-static`）。
- Windows 便携构建的两个额外关键选项（CMake 在 `VOILUT_PORTABLE AND WIN32` 时自动设置）：
  `ABSL_MSVC_STATIC_RUNTIME=ON`（Dawn 内嵌 abseil 默认 `/MD`，与 `/MT` 混链会
  `LNK2038`）与 `DAWN_FORCE_SYSTEM_COMPONENT_LOAD=ON`（否则 Dawn 对裸文件名
  `d3dcompiler_47.dll` 使用 `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`，返回
  `ERROR_INVALID_PARAMETER`，单文件版启动失败）。
- Windows 一键发行：
  `powershell -ExecutionPolicy Bypass -File scripts\build_windows.ps1 -VcpkgRoot <vcpkg>`。
  脚本会安装静态依赖、构建、检查非系统 DLL 依赖并复制到 `dist/`。

## 文档约定

- `README.md` 面向使用者：特性、构建、操作、结构、公式。
  不要写成开发日志；不要列出未完成计划。
- 接口细节写在头文件注释里；`README.md` 只给阅读顺序，不重复 API。
