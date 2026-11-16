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
  四条路径都能改变显示（可用真实 TIFF/DICOM 图像验证一次）。
- 头文件注释改动后跑 `doxygen Doxyfile`，确认无 `WARN_IF_DOC_ERROR` 报警。
- 依赖不全时一律使用 `bash scripts/install_deps.sh`（或单独运行其中某个脚本）：
  它会安装 jinja2/markupsafe，并在网络较慢时逐个重新获取被中断的子依赖。
- 已知工具链问题（脚本已应用补丁，请勿删除）：
  - GCC 17 对 `uint64_t{X}` 形式的 constexpr 花括号初始化会 ICE，abseil 里那处
    必须保持 `static_cast<uint64_t>(X)` 写法。
  - Dawn 的 `tint/utils/rtti/traits.h` 必须补 `#include <sstream>`。
- 按需渲染与 ImGui 的两个已知问题（已修复，修改代码时不要回退）：
  - 面板尺寸不要使用自动适应（高度为 0）：首帧之后程序阻塞在 `glfwWaitEvents`，
    没有事件就不会重绘，窗口会停在只有标题栏、高度异常的状态。请使用固定尺寸。
  - 主循环前 3 帧使用 `glfwPollEvents` 预热，让字体图集与布局稳定。
- `std::println` 在 stdout 重定向时是全缓冲：`main` 中已设置行缓冲，
  否则日志文件里看不到「已加载 …」。

## 文档约定

- `README.md` 面向使用者：特性、构建、操作、结构、公式。
  不要写成开发日志；不要列出未完成计划。
- 接口细节写在头文件注释里；`README.md` 只给阅读顺序，不重复 API。
