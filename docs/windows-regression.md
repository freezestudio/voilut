# Windows 实机回归清单

> 适用范围：Dawn / D3D12 后端、单文件发行、虚拟切片（预览 + 细节缓存）。
> 对应代码：`main`（commit `9636020` 之后）。
> 目标：在真实 Windows 10/11 + D3D12 显卡上验证 Linux 侧已完成的功能。

---

## 0. 前置条件

| 项目 | 要求 |
|---|---|
| 系统 | Windows 10 1809+ / Windows 11，x64 |
| 显卡 | 支持 D3D12（项目未启用 D3D11/Vulkan 后端） |
| 工具 | Visual Studio 2022（C++ 工具）、CMake ≥ 3.28、Ninja、Python 3、vcpkg |
| 依赖 | `third_party/dawn`、`third_party/imgui`；vcpkg 静态 `tiff` / `gdcm` |
| 显卡驱动 | 已安装厂商驱动（不要用微软基本显示适配器） |

> MSVC 需支持 C++23 的 `std::expected` / `std::println`（VS2022 17.10+）。

---

## 1. 构建与静态检查

### 1.1 一键脚本（推荐）

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_windows.ps1 `
  -VcpkgRoot C:\vcpkg
```

脚本会：安装 `x64-windows-static` 的 TIFF/GDCM → 用 `/MT` + 静态 triplet 构建 →
运行 CTest → `dumpbin` 检查非系统依赖 → 复制到 `dist\windower.exe`。

### 1.2 手动构建（排查用）

```powershell
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DVOILUT_PORTABLE=ON -DVOILUT_EMBED_SHADERS=ON `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

CTest 必须 **4/4** 通过：

- `window_model_test`
- `decode_test`
- `region_source_test`
- `detail_cache_test`

### 1.3 单文件检查

```powershell
dir dist\windower.exe
dumpbin /nologo /dependents dist\windower.exe
```

**必须**

- [ ] `dist\` 中只有 `windower.exe`（没有 `shaders\`、`.dll`、`.pdb` 需求）
- [ ] `dumpbin` 不出现：
  - `vcruntime140.dll` / `msvcp140.dll` → `/MT` 未生效
  - `tiff.dll` / `gdcmMSFF*.dll` / `zlib1.dll` → 动态依赖残留
  - `dxcompiler.dll` / `dxil.dll` → `DAWN_USE_BUILT_DXC` 未保持 OFF
- [ ] 允许出现系统 DLL：`KERNEL32`、`USER32`、`GDI32`、`SHELL32`、`D3D12`、`DXGI`、
      `ole32`、`d3dcompiler_47`（System32 自带）

---

## 2. 启动与基础路径

| # | 步骤 | 预期 |
|---|---|---|
| 2.1 | `dist\windower.exe --version` | `Windower 0.1.0`（或当前版本） |
| 2.2 | `dist\windower.exe --help` | 打印用法，退出码 0 |
| 2.3 | 双击 `windower.exe`（无参数） | 窗口出现、UI 可交互，提示未加载图像 |
| 2.4 | 从任意目录（无 `shaders\`）启动并打开图像 | 正常渲染（着色器已嵌入） |
| 2.5 | 把 .tif 拖入窗口 | 开始加载；显示「正在加载…」；完成后显示图像 |
| 2.6 | 打开不存在的文件/损坏文件 | 弹出错误模态框（不是静默空白） |
| 2.7 | 关闭窗口 | 正常退出，无崩溃；后台解码线程不阻塞退出 |

---

## 3. 窗宽窗位核心（TIFF 16 位）

准备：16 位 stripped / tiled TIFF，512×512 与 2000×2000 各一份。

| # | 操作 | 预期 |
|---|---|---|
| 3.1 | 打开 TIFF | 自动窗（p1/p99）生效；直方图有数据 |
| 3.2 | 左键拖 ROI | 框选期间窗值实时变化（ROI 统计） |
| 3.3 | Shift+左键拖动 | 水平调窗宽、垂直调窗位；窄窗步进更小 |
| 3.4 | `1`…`6` / 面板预设 | 六种预设窗切换，窗宽按比例变化 |
| 3.5 | `0` | 重置窗（DICOM 默认窗优先，否则 p1/p99） |
| 3.6 | `I` / `L` | 反色 / 双线性切换立即生效 |
| 3.7 | `R` / `H` / `V` / `F` | 旋转、翻转、适应窗口；ROI 叠加仍对齐 |
| 3.8 | 滚轮 / Ctrl+滚轮 / Shift+滚轮 | 缩放 / 窗宽 / 窗位；光标锚点不漂移 |
| 3.9 | 鼠标悬停 | 探针显示原始像素与映射灰度；浮点/大图标注「预览值」 |
| 3.10 | 直方图拖动 | 窗位跟随；黄线 p1/p99 正确 |
| 3.11 | 菜单「图像 → DICOM 精确 VOI」 | 切换简化 / DICOM 公式；边界灰度变化可见 |
| 3.12 | 多页 TIFF（2 页以上） | 帧滑块/`,` `.` 切换；标题帧号、直方图更新；窗值保持 |

---

## 4. 浮点 TIFF

准备：32 位 IEEE float TIFF（显微/科学相机样本）。

| # | 检查 | 预期 |
|---|---|---|
| 4.1 | 打开 float TIFF | 正常显示；日志「32 位浮点」 |
| 4.2 | 窗位/窗宽显示 | 小数（`%.6g`），不是整数截断 |
| 4.3 | 自动窗 | 宽度按值域比例（不会出现固定 100 的异常宽窗） |
| 4.4 | 探针 | 浮点值与显示灰度正确 |
| 4.5 | 「DICOM 精确 VOI」 | 对浮点图像禁用（灰显） |
| 4.6 | 缩放/平移/旋转 | 正常，无纹理格式错误 |

---

## 5. 虚拟切片：大 TIFF

准备：≥ 20000×2000 的 tiled 16 位 TIFF（可用 ImageMagick 生成后 `tiffcp -t`）。
判定：宽/高 > 16384，或总像素超过解码上限。

| # | 步骤 | 预期 |
|---|---|---|
| 5.1 | 打开大 TIFF | 日志「大图预览：按 1/N 显示」；标题含「预览 1/N」；面板显示细节状态 |
| 5.2 | 观察内存/启动时间 | 不加载整幅像素；启动后 1~2 秒内可交互 |
| 5.3 | 放大到 100%（缩放 ≥ 75%） | 约 120ms 后日志「细节纹理已启用：W×H（层 0）」，画面细节变清晰 |
| 5.4 | 小范围平移 | 复用 25% 预取边距，不频繁出现「细节读取失败」 |
| 5.5 | 大幅平移 / 缩放 | 细节更新；无闪烁/错位；旧层仍按矩形正确显示 |
| 5.6 | 来回缩放/平移 | 已缓存区域不重复读取（观感：更快、无加载抖动） |
| 5.7 | 缩放到 < 75% | 细节层关闭，回到预览 + mipmap；无接缝 |
| 5.8 | 旋转/翻转后放大 | 细节矩形仍对应当前原图区域，无错位 |
| 5.9 | 菜单「视图 → 高分辨率细节」取消勾选 | 细节关闭；重新勾选后缓存区域恢复 |
| 5.10 | 切换到另一张大图再回来 | 旧缓存清空；新图重新请求细节 |

> 细节纹理数组为 4×2048²；若 D3D12 驱动对 `texture_2d_array` 或 2048² 有异常，
> 日志会出现 Dawn validation error。记录完整日志。

---

## 6. DICOM

准备样本：
- 8/16 位 MONOCHROME2；
- 有符号 CT（Rescale Slope/Intercept、HU）；
- 多组 WindowCenter/WindowWidth；
- 带 PixelPaddingValue；
- 多帧（NumberOfFrames > 1）；
- 压缩传输语法（JPEG Lossless / JPEG2000 / RLE，视样本而定）。

| # | 检查 | 预期 |
|---|---|---|
| 6.1 | 打开 CT（有符号） | 面板 L/W 显示 HU（不是 32768 偏移值）；探针显示「像素（HU xx）」 |
| 6.2 | 多组 WC/WW | 面板出现「DICOM 窗值（共 N 组）」按钮，逐组切换正确 |
| 6.3 | 带 padding 的 CT/MR | 面板提示「已排除填充值 …」；自动窗不被背景拉低 |
| 6.4 | 多帧 DICOM | 帧滑块/`,` `.` 可切换；帧统计/直方图更新；标题帧号正确 |
| 6.5 | 压缩 DICOM | 全量解码路径正常；虚拟路径下细节读取失败时只记录日志，预览仍可用 |
| 6.6 | 大 DICOM（≥20000×2000） | 流式分块预览（不一次性加载）；放大后细节纹理上传 |
| 6.7 | 像素间距/方向 | 当前版本不处理 DICOM IOP；确认只影响显示方向，不影响调窗 |

---

## 7. 渲染与错误处理

| # | 检查 | 预期 |
|---|---|---|
| 7.1 | 日志中 Dawn 启动 Warning | 允许 `maxDynamicUniformBuffers...` 两条；不应有 `Error:` |
| 7.2 | 超纹理上限且无预览能力的格式 | 明确错误模态框，不静默空白 |
| 7.3 | 内存不足/损坏文件 | 错误模态框；程序不退出 |
| 7.4 | 快速连续切换大图 | 加载期间翻页排队；完成后只显示最后一张；无崩溃 |
| 7.5 | 加载中关闭窗口 | 正常退出（等待后台 future，不访问已销毁窗口） |
| 7.6 | 高 DPI 150% / 200% | 窗口、面板、字体、线宽同比例；文字不裁切；无模糊到不可读 |
| 7.7 | 多显示器不同缩放 | 移动窗口后布局仍正确；Surface 尺寸跟随 framebuffer |
| 7.8 | 4K 全屏 | 面板宽度足够容纳预设按钮；直方图/探针可读 |

---

## 8. 单文件发行终局

在有 VS / vcpkg 的机器构建后，把 `dist\windower.exe` 复制到**另一台干净机器**
（未安装 VS、未装 vcpkg、无项目源码）：

- [ ] 双击可启动
- [ ] 打开 16 位 TIFF / DICOM 正常
- [ ] 自动窗 / ROI / 拖动 / 预设 / 反色 / 旋转正常
- [ ] 大图预览 + 放大细节正常（需要 D3D12 驱动）
- [ ] 任务管理器中没有异常高内存（大图预览不应等于整幅像素）
- [ ] 关闭后目录中不产生 `imgui.ini` 或其他文件

---

## 9. 失败时收集的信息

请附上：

1. Windows 版本 + 显卡型号 + 驱动版本；
2. `dumpbin /dependents dist\windower.exe` 输出；
3. `windower.exe <样本> > log.txt 2>&1` 的完整日志；
4. 触发步骤（缩放比例、图像尺寸/格式、是否启用 DICOM VOI/细节）；
5. 如为渲染问题，附截图与是否可稳定复现。

---

## 10. 通过标准

- 第 1 节全部复选框通过；
- 第 2–7 节无 `FAIL`，无未解释的 Dawn `Error:`；
- 第 8 节干净机器验证通过。

---

## 附：合成 DICOM 生成

仓库带有纯 Python 脚本（无第三方依赖）：

```powershell
python scripts\make_test_dicom.py big.dcm 20000 2000
```

生成未压缩、显式 VR 小端、16 位 MONOCHROME2、HU 标定（slope=1, intercept=-1024,
WC=40, WW=400）的测试文件，用于第 6 节的大 DICOM 流式预览与细节验证。
