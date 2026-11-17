# 虚拟切片（超大图像 1:1 查看）设计稿

> 状态：设计（Phase 0 实现中）
> 关联代码：`imaging/src/decode/`、`imaging/src/image_renderer.cxx`、`apps/image_io.cxx`
> 目标平台：Linux / Windows，Dawn（Vulkan / D3D12）

---

## 1. 背景与目标

当前实现把**整幅图像**解码进内存，再上传为单个 GPU 纹理：

- `kMaxDecodeBytes = 1.5 GB`，更大的文件直接拒绝；
- 纹理边长上限为 `min(adapter.maxTextureDimension2D, 16384)`；
- 超过上限时按整数倍降采样成**预览纹理**（`preview_scale()`），放大后无法恢复原始像素。

这与“科研大图”（全切片、显微拼图、工业 CT）的实际使用冲突：文件常为
20000×20000 ~ 100000×100000，既装不进内存，也超过单纹理上限。

**目标**

1. 打开任意尺寸图像时不加载整幅像素（内存有界）；
2. 缩小时显示全图预览，放大到 1:1 时显示**当前视口内的原始分辨率**细节；
3. 平移/缩放不冻结 UI，允许短时间使用预览降级；
4. 复用现有窗宽窗位、反色、方向、探针、ROI、直方图语义；
5. 支持 TIFF（stripped / tiled / 多页 / 大端 / 浮点）与 DICOM（能区域读取时）。

**非目标（本设计不引入）**

- 测量/标注/序列管理/导出（与 `AGENTS.md` 的核心定位冲突）；
- 完整虚拟纹理（sparse texture）级别的任意缩放 1:1；
- GPU 端解码（JPEG/JPEG2000 仍在 CPU，由 libtiff/GDCM 完成）。

---

## 2. 现状与约束（已实测）

| 项目 | 现状 |
|---|---|
| 默认纹理上限 | Dawn 默认 `maxTextureDimension2D = 8192` |
| 已请求上限 | `min(adapter 上限, 16384)`（本机 adapter 支持 32768） |
| 超过上限 | 整数倍盒式降采样为预览（`preview_scale()`） |
| 解码内存上限 | `kMaxDecodeBytes = 1.5 GB` |
| 解码线程 | `std::async` 后台解码，主线程只做 GPU 上传 |
| TIFF | stripped / tiled、多页、大端、MINISWHITE、32 位浮点 |
| 多帧 | 连续存储，`frame_pixels/frame_floats` 取帧，受内存上限 |

关键结论：**瓶颈不是纹理格式，而是“区域解码 + 按需上传”的缺失**。

---

## 3. 方案比较

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 全图解码 + 预览（现状） | 简单、窗值统计准确 | 内存/纹理受限，>16k 无细节 | 保留为小图路径 |
| B. 视口裁剪细节纹理 | 实现成本低；复用现有预览；1:1 只在可见区 | 快速平移需重解码，无可视区域外细节 | **Phase 1 采用** |
| C. 瓦片缓存（LRU tile） | 平移可命中缓存；可预取 | 需 tile 调度、缓存与多纹理绑定 | Phase 2 |
| D. 虚拟纹理 / 稀疏纹理 | 理论上最优 | WebGPU 无 sparse 纹理；atlas + indirection 复杂 | 远期可选 |

**分阶段策略**：先做 B（预览 + 单个细节纹理），验证区域解码与渲染映射；
再把细节纹理替换为 tile 缓存（C）。

---

## 4. 总体架构

```
┌────────────────────────────── app ──────────────────────────────┐
│ ImageIO: open -> RegionSource（不预解码全图）                    │
│          ├─ preview(max_dim) -> DecodedImage（预览纹理）         │
│          └─ read_region(rect, frame) -> PixelRegion              │
│                                                                  │
│ DetailController（新增）                                         │
│   视口变化 → 计算可见源区域 → 防抖 → 后台 read_region            │
│   完成后 -> renderer.upload_detail(rect, pixels)                 │
└──────────────────────────────────────────────────────────────────┘
                               │
┌──────────────────────── imaging ─────────────────────────────────┐
│ RegionSource 接口 + TIFF/GDCM 实现（区域读取、线程内串行）       │
│ ImageRenderer：preview texture + detail texture + 新的 uniform    │
│ image.wgsl：source UV → detail rect 判定 → detail / preview 采样  │
└──────────────────────────────────────────────────────────────────┘
```

### 4.1 数据流（打开与查看）

```
open(file)
  → open_region_source()
  → source.preview(max_preview_dim)        // 有界内存的全图预览
  → renderer.upload_preview(source_w/h, preview_w/h)
  → 统计/直方图基于预览（见 §9）
  → fit_to_window

viewport 变化（缩放/平移/旋转/切帧）
  → 计算可见的源图像区域（AABB）
  → 若 zoom >= detail_threshold 且区域 <= detail_max：
      生成 detail 请求（带 generation）
  → 后台 read_region(rect, frame)
  → 完成后 upload_detail；失败则继续用预览
```

---

## 5. 新增接口（Phase 0）

放在 `imaging/include/region_source.h`：

```cpp
namespace voilut
{
    enum class SampleType { UInt16, Float32 };

    struct RegionInfo
    {
        uint32_t width = 0, height = 0, frame_count = 1;
        SampleType sample_type = SampleType::UInt16;
        uint32_t bits_per_sample = 0;
        bool initial_invert = false;      // MINISWHITE / MONOCHROME1
        ValueScale scale;                 // 存储域 ↔ 模态值
        ValueExclusion padding;           // PixelPaddingValue
        std::string name;
        std::string modality_unit;
    };

    struct PixelRegion
    {
        uint32_t x = 0, y = 0, width = 0, height = 0;
        std::vector<uint16_t> u16;        // sample_type == UInt16
        std::vector<float> f32;           // sample_type == Float32
    };

    class RegionSource
    {
    public:
        virtual ~RegionSource() = default;
        [[nodiscard]] virtual const RegionInfo &info() const noexcept = 0;

        /// 有界内存的全图预览（首帧）；最多 max_dimension 边长。
        [[nodiscard]] virtual std::expected<DecodedImage, std::string>
        preview(uint32_t max_dimension) = 0;

        /// 读取任意矩形（原图坐标，越界裁剪；frame 0-based）。
        [[nodiscard]] virtual std::expected<PixelRegion, std::string>
        read_region(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                    uint32_t frame) = 0;
    };

    [[nodiscard]] std::expected<std::unique_ptr<RegionSource>, std::string>
    open_region_source(const std::string &path);
}
```

设计约定：

- `read_region` 返回的像素坐标系与 `x/y` 对齐（左上角）；
- 调用方保证同一 `RegionSource` 上**串行**调用（Phase 1 在单解码线程内使用）；
- 预览返回的 `DecodedImage.meta.width/height` 是**预览尺寸**；
  Phase 1 会新增 `source_width/source_height`（原图尺寸）供视口/ROI/探针映射；
- 失败返回 `std::unexpected`，不抛异常。

---

## 6. 解码器实现

### 6.1 TIFF（Phase 0 首先实现）

- 每次 `read_region` 重新 `TIFFOpen` + `TIFFSetDirectory(frame)`：
  避免 libtiff 句柄跨线程共享，代价是几毫秒打开开销；
  若性能不足，Phase 2 改为“每解码线程一个句柄 + mutex”。
- **tiled**：读取与请求矩形相交的 tile（`TIFFReadTile`），按行拷贝；
- **stripped**：逐行 `TIFFReadScanline`（libtiff 内部按 strip 解压）；
- 处理 8/16 位、大端字节交换、`PHOTOMETRIC_MINISWHITE`；
- 32 位浮点走独立的读取路径（4 字节/像素）；
- `preview(max_dim)`：
  - Phase 0 用现成 `decode_tiff` 解码首帧后盒式降采样（小图/测试可用）；
  - Phase 1 改为流式降采样（逐 tile/行写输出，不分配全图）。

### 6.2 DICOM（Phase 2）

- 优先 `gdcm::ImageRegionReader`：`ReadInformation()` → `SetRegion()` → `Read()`；
  支持未压缩传输语法；压缩语法（JPEG/JPEG2000/RLE）若区域读取失败：
  - 文件总像素 ≤ `kMaxDecodeBytes`：退回全量 `decode_dicom` 后裁剪；
  - 否则返回“暂不支持该压缩 DICOM 的区域读取”，界面保留预览。
- 多帧：按 frame 设置 `ImageRegionReader` 的维度/帧偏移；
- 线程安全：与 TIFF 相同，一个解码线程内串行。

### 6.3 字体/资源

与虚拟切片无关；仍走单文件嵌入方案。

---

## 7. 渲染设计（Phase 1）

### 7.1 纹理

| 纹理 | 尺寸 | 用途 |
|---|---|---|
| `preview_texture_` | ≤ 8192（虚拟路径建议 8192，而非 16384） | 全图缩略，带 mipmap |
| `detail_texture_` | 2048 / 4096（方形，`R16Uint` 或 `R32Uint`） | 当前视口 1:1 细节 |

细节纹理尺寸与显存：4096²×2 = 32 MB（uint16）/ 64 MB（float）。

### 7.2 Uniform

`WindowParams` 扩展为 64 字节（或新增 `ViewParams` uniform）：

```wgsl
struct WindowParams {
  level: f32,
  width: f32,
  invert: f32,
  linear: f32,
  transform: f32,
  mode: f32,
  value_type: f32,
  _pad: f32,

  source_size: vec2<f32>,     // 原图宽高
  preview_size: vec2<f32>,    // 预览纹理宽高
  detail_rect: vec4<f32>,     // 细节区域（原图像素坐标 x,y,w,h）
  detail_enabled: f32,
  _pad2: vec3<f32>,
}
```

### 7.3 采样顺序

```wgsl
let image_uv = orient_uv(display_uv);         // 已有的方向变换
let source_px = image_uv * params.source_size;

var value: f32;
if (params.detail_enabled > 0.5 && inside(detail_rect, source_px)) {
    let duv = (source_px - detail_rect.xy) / detail_rect.zw;
    value = sample_detail(duv);
} else {
    value = sample_preview(image_uv);          // 现有 mipmap LOD 采样
}
// 之后照旧走窗宽窗位 / 反色
```

- 细节区域边界做 1~2 px 的 `mix`，避免分辨率突变的接缝；
- 方向变换在**采样前**完成，细节矩形始终用原图坐标；
- 旋转/翻转不改变细节区域的计算（可见 AABB 可能更大）。

### 7.4 视口 → 源区域

由屏幕四角反变换：

```
screen corner → viewport inverse → display UV → orient inverse → source px
取四角 AABB，clamp 到 [0, source_size]
```

- 细节区域上限 `detail_max`（2048/4096）；
- 若缩放比例 < `detail_threshold`（例如 0.75），关闭细节；
- 细节区域按 `detail_max` 裁剪，并**向外取整**到偶数/2 的倍数，减少重解码频率。

---

## 8. 应用调度（Phase 1）

新增 `apps/detail_view.cxx`（或并入 `image_io`）：

```cpp
struct DetailRequest {
    uint64_t generation;
    uint32_t x, y, w, h, frame;
};

struct DetailState {
    std::future<std::expected<PixelRegion, std::string>> future;
    DetailRequest request;
    bool inflight = false;
    uint64_t generation = 0;
    std::chrono::steady_clock::time_point last_change;
};
```

规则：

1. 视口/方向/帧变化时记录 `last_change`，**防抖 80~120 ms**；
2. 防抖结束后若满足条件，启动一次后台 `read_region`；
3. 完成时比较 `generation`，过期结果直接丢弃（仍可写入缓存）；
4. 平移过程中可继续显示旧细节（坐标已变，会短暂错位）或直接关闭细节；
   推荐：平移开始时关闭细节，完成后重新请求，避免“错位细节”；
5. 退出时等待 inflight future，避免后台线程访问已销毁的 `RegionSource`；
6. 失败（不支持/IO 错误）只关闭细节，不打断预览。

Phase 2 在此基础上加：

- 2~4 个 tile/region 缓存（LRU）；
- 预取视口外 10~20%；
- 按 tile 粒度的请求合并。

---

## 9. 统计 / 探针 / ROI

- **自动窗 / 直方图**：Phase 1 使用预览（最多 8192²）统计；
  对科研大图，这已经是数千万像素的稳定估计。
  可选优化：后台用 `read_region` 分块抽样做全图统计，完成后替换。
- **探针**：优先用当前细节区域内的像素；
  不在细节区域时回退到预览像素值，并在 UI 标注“预览值”。
- **ROI 窗**：对 ROI 覆盖的区域按 `read_region` 分块采样（Phase 2），
  Phase 1 先用预览上对应区域（ROI 坐标映射到预览）。
- **模态值/填充值**：`ValueScale`、`ValueExclusion` 在 `RegionSource::info()`
  中暴露；所有显示路径继续走现有换算。

---

## 10. 内存与性能预算

| 项目 | 预算 |
|---|---|
| 预览纹理 | 8192²×2 B ≈ 128 MB（+ mip ≈ 170 MB） |
| 预览 CPU 缓冲 | 同量级（解码期间短暂存在） |
| 细节纹理 | 4096²×2 B = 32 MB（float 64 MB） |
| 细节 CPU 缓冲 | 同量级（单个/两个槽位） |
| 单次区域解码 | 4096² 像素；tiled TIFF 通常 50~300 ms（本地 SSD） |
| 目标交互 | 缩放/平移停止后 ≤ 300 ms 出现 1:1 细节 |

配置以常量集中在一个头文件，便于按显存调整。

---

## 11. 分阶段实施

### Phase 0：区域解码基础（已完成）

- [x] 设计稿；
- [x] `region_source.h` + TIFF 实现（stripped/tiled/多页/大端/浮点）；
- [x] `preview(max_dim)`（**当前仍用全量解码后降采样**，Phase 1 改流式）；
- [x] 测试：随机区域读取、边界裁剪、多页、浮点、8 位 MINISWHITE、大端、预览尺寸上限；
- [x] 修正 libtiff 字节序处理：`TIFFReadScanline/TIFFReadTile` 已返回本机字节序，
      不再手动交换（原实现会双重交换）。

### Phase 1：预览 + 细节纹理

- [x] `DecodedImage` 增加 `preview_width/height`（`meta` 保留原图逻辑尺寸）；
- [x] `ImageRenderer::upload(..., data_size)` 支持已降采样的预览纹理；
- [x] 打开流程：超大 TIFF 走 `open_region_source` + **流式预览**（4096 边长），
      小图仍走全量解码；
- [x] 探针 / ROI 按原图坐标映射到预览纹理；
- [x] `ImageRenderer` 第二纹理（detail）+ 64 字节 uniform + WGSL 细节采样；
- [x] `DetailController`（120 ms 防抖、generation、异步、按需唤醒）；
- [x] 已在 20000×2000 tiled TIFF 上验证：预览 1/5，放大后请求 256×256 细节并上传；
- [ ] 验收：40000² tiled TIFF 打开 RSS 有界、100% 可读、平移 300ms 内更新（待大样本）。

### Phase 2：缓存与 DICOM（进行中）

- [x] GDCM `ImageRegionReader` 区域读取（元数据用 `ReadInformation`，不加载 PixelData）；
      未压缩与 GDCM 能按区域解码的压缩语法可用；预览暂用全量解码（≤ `kMaxDecodeBytes`）；
- [x] 细节预取 25% 边距（小幅平移复用同一纹理）；
- [x] 已在 20000×2000 未压缩 DICOM 上验证预览 1/5 + 放大后 256×256 细节；
- [x] 多区域 LRU 缓存（4 层 2048² 纹理数组）+ 25% 预取边距；
- [ ] 超大 DICOM（> `kMaxDecodeBytes`）的流式预览；
- [x] 全图统计抽样：TIFF 流式预览时累加 65536 桶直方图；DICOM 全量解码时沿用首帧完整统计；
- [ ] 验收：DICOM WSI/多帧、连续平移命中缓存。

### Phase 3（可选）

- 多细节纹理 / tile atlas；
- 背景全图统计与直方图渐进更新；
- 超大图导出（明确不在核心路径）。

---

## 12. 风险与回退

| 风险 | 应对 |
|---|---|
| libtiff/GDCM 线程安全 | 一个解码线程内串行；未来每线程独立句柄 |
| 压缩 DICOM 无法区域读 | 回退全量（≤1.5GB）或仅预览 + 明确提示 |
| 细节/预览接缝 | 1~2 px 混合；细节区域向外扩展 |
| 旋转后区域计算复杂 | 用源图 UV 反变换，AABB 保守取大 |
| 区域解码慢 | 防抖、缓存、预取；先保留预览 |
| 预览统计偏差 | UI 标注；后续抽样全图统计 |

**回退策略**：任何一步失败都退回当前“全量解码 + 预览”路径，
不阻塞用户查看窗宽窗位效果。

---

## 13. 验收标准（Phase 1）

1. 40000×40000 tiled TIFF：打开后进程内存 < 1 GB（不含 GPU 驱动缓存）；
2. 缩放到 100%：视口内显示原始像素（探针值与源文件一致）；
3. 平移停止后 ≤ 300 ms 更新细节；期间窗口可交互；
4. 现有测试（`window_model_test` / `decode_test`）全部通过；
5. 小图（≤ 8192）行为与当前版本一致；
6. 浮点 / 多页 / MINISWHITE / 大端样本的区域读取与全量解码一致。
