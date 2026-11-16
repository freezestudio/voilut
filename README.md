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
- **自动定窗**：65536 桶直方图一次遍历得到 min/max 与 p1/p99，
  默认窗取 `(p1+p99)/2 ± (p99-p1)/2`
- **ROI（感兴趣区域）实时窗**：框选区域，按框内统计重新定窗（实时跟随拖动）
- **拖动调窗**：左键拖动，水平调窗宽、垂直调窗位；窄窗时步进自动减小
- **预设窗**：以值域比例定义（全范围 / 宽窗 / 中窗 / 窄窗 / 低值 / 高值），
  跨模态、跨设备通用，无需为每种设备单独配置
- **直方图联动**：柱状图 + 当前窗区间高亮 + p1/p99 参考线，拖动直方图可移动窗位
- **反色与方向**：MONOCHROME1 自动反色、手动反色、旋转 / 翻转（只影响显示）
- **探针**：悬停显示原始像素值及经窗映射后的显示灰度
- **解码**：TIFF（libtiff，含 16 位灰度与常见压缩）、DICOM（GDCM，
  含压缩传输语法、WindowCenter/WindowWidth 标签、Rescale Slope/Intercept）
- **Windows / Linux**：Dawn（Vulkan / D3D12）统一后端，两个平台共用同一套代码

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
./build/windower 图像.tif          # 或 .dcm
./build/windower ./samples/一组图像/  # 目录会展开成图像列表
./build/windower                   # 无参数：窗口内「打开文件夹...」或拖放
```

Windows（MSVC + vcpkg）：

```powershell
vcpkg install tiff gdcm --triplet x64-windows
powershell -ExecutionPolicy Bypass -File scripts\install_dawn.ps1
cmd /c "call `"<VS>\VC\Auxiliary\Build\vcvars64.bat`" >nul && set `"VCPKG_ROOT=C:\vcpkg`" && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build"
```

---

## 三、操作

| 操作 | 效果 |
|---|---|
| **左键拖动** | **调窗（水平 = 窗宽，垂直 = 窗位）—— 核心交互** |
| Shift + 左键拖动 | 框选，按框内 p1/p99 实时定窗（ROI 窗） |
| 右键拖动 | 平移 |
| 滚轮 | 以光标为中心缩放 |
| Ctrl + 滚轮 | 窗宽 |
| Shift + 滚轮 | 窗位 |
| ← / → | 窗位微调（按住 Ctrl 时步进更小） |
| ↑ / ↓ 或 `+` / `-` | 窗宽微调（按住 Ctrl 时步进更小） |
| `0` | 重置窗（DICOM 默认窗 → p1/p99 自动窗） |
| `1`…`6` | 预设窗 |
| `F` | 适应窗口 |
| `I` / `L` | 反色 / 双线性插值 |
| `R` / `H` / `V` | 旋转 90° / 水平翻转 / 垂直翻转 |
| `[` / `]` | 上一张 / 下一张 |
| `Q` / `Esc` | 退出 |

面板里还有滑块、数值输入和直方图拖动；勾选「记住窗值」后，
同一会话内每张图像会记住上次使用的窗。

---

## 四、结构

```
core/      通用基础设施（静态库 voilut_core）
           context       WebGPU 环境（协程式 Instance/Adapter/Device 初始化）
           coroutine     task<T> / async_value<T> / sync_wait
           shader_loader 运行时读取 shaders/*.wgsl
           window        默认窗口尺寸

imaging/   窗宽窗位核心（静态库 voilut）
           image_model    统计、直方图、自动窗、ROI 窗、预设窗、拖动调窗
           image_renderer Dawn 渲染管线（uniform 即 WindowParams）
           decode/        TIFF（libtiff）/ DICOM（GDCM）/ 后缀分派
           viewport       缩放平移；orientation 旋转翻转

apps/      windower.cxx —— 可执行程序 windower
shaders/   image.wgsl  —— 窗宽窗位映射的实现
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

映射公式（着色器与 CPU 侧 `display_ratio` 完全一致）：

```
lower  = level - width / 2
gray   = clamp((value - lower) / width, 0, 1)   // 反色时取 1 - gray
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
