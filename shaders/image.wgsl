// ============================================================
// image.wgsl —— 16 位灰度图像显示（窗宽窗位 / 反色 / 插值）
// ============================================================
// voilut 的显示映射核心：像素值 → 窗宽窗位 → 灰度。
//
// 【两个入口】
//   @vertex   vs：把图像四边形（quad）的顶点变换到裁剪空间，并传递 UV。
//   @fragment fs：按 UV 采样 16 位纹理，做窗宽窗位映射与反色。
//
// 【绑定】
//   @group(0) @binding(0) WindowParams ：窗位 / 窗宽 / 反色 / 插值开关。
//   @group(1) @binding(0) texture_2d<u32>：16 位单通道图像（格式 r16uint）。
//
// 【坐标系】
//   UV 原点在图像左上角，与图像像素坐标一致（顶点缓冲由 C++ 侧提供）。
// ============================================================

// 窗宽窗位参数：32 字节，与 C++ 侧 voilut::WindowParams 逐字段对应。
struct WindowParams {
  level: f32,     // 窗位（Window Level）
  width: f32,     // 窗宽（Window Width）
  invert: f32,    // 是否反色：>0.5 为反色
  linear: f32,    // 是否线性插值：>0.5 为双线性
  transform: f32, // 显示方向编码：bit0-1 旋转，bit2 水平翻转，bit3 垂直翻转
  mode: f32,      // 映射公式：0 = 简化，1 = DICOM 精确（C.11.2.1.2.1）
  _pad1: f32,
  _pad2: f32,
}

@group(0) @binding(0) var<uniform> params: WindowParams;
@group(1) @binding(0) var image: texture_2d<u32>;

struct VSOut {
  @builtin(position) position: vec4<f32>,
  @location(0) uv: vec2<f32>,
}

// 显示方向变换：把「显示 UV」映射到「原图 UV」。
// 公式必须与 C++ 侧 voilut::display_uv_to_image_uv 保持一致。
fn orient_uv(uv: vec2<f32>) -> vec2<f32> {
  var p = uv;
  let code = i32(params.transform + 0.5);
  let rot = code & 3;
  if (rot == 1) {
    p = vec2<f32>(p.y, 1.0 - p.x);
  } else if (rot == 2) {
    p = vec2<f32>(1.0 - p.x, 1.0 - p.y);
  } else if (rot == 3) {
    p = vec2<f32>(1.0 - p.y, p.x);
  }
  if ((code & 4) != 0) {
    p.x = 1.0 - p.x;
  }
  if ((code & 8) != 0) {
    p.y = 1.0 - p.y;
  }
  return p;
}

@vertex
fn vs(@location(0) pos: vec2<f32>, @location(1) uv: vec2<f32>) -> VSOut {
  var out: VSOut;
  out.position = vec4<f32>(pos, 0.0, 1.0);
  out.uv = uv;
  return out;
}

// 选择 mipmap 层级：由 UV 在图像像素空间的导数为屏幕足迹，取其 log2。
// 缩小显示大图时用低分辨率层采样，避免最近邻跳过像素造成的锯齿。
fn select_lod(image_uv: vec2<f32>) -> i32 {
  let size = vec2<f32>(textureDimensions(image, 0));
  let footprint = max(length(dpdx(image_uv * size)), length(dpdy(image_uv * size)));
  let lod = log2(max(footprint, 1.0));
  let max_level = f32(textureNumLevels(image)) - 1.0;
  return i32(clamp(floor(lod + 0.5), 0.0, max(0.0, max_level)));
}

// 最近邻采样：按 UV 取最近像素，放大时保留锐利边缘。
fn sampleNearest(uv: vec2<f32>) -> f32 {
  let lod = select_lod(uv);
  let size = vec2<i32>(textureDimensions(image, lod));
  let coord = clamp(vec2<i32>(vec2<f32>(size) * uv), vec2<i32>(0), size - vec2<i32>(1));
  return f32(textureLoad(image, coord, lod).r);
}

// 双线性采样：手动对 4 个邻居按小数权重混合。
// （r16uint 纹理不能直接用采样器做插值，因此手动实现。）
fn sampleBilinear(uv: vec2<f32>) -> f32 {
  let lod = select_lod(uv);
  let size = vec2<i32>(textureDimensions(image, lod));
  let p = vec2<f32>(size) * uv - vec2<f32>(0.5);
  let base = vec2<i32>(floor(p));
  let frac = p - floor(p);
  let lo = clamp(base, vec2<i32>(0), size - vec2<i32>(1));
  let hi = clamp(base + vec2<i32>(1), vec2<i32>(0), size - vec2<i32>(1));
  let v00 = f32(textureLoad(image, vec2<i32>(lo.x, lo.y), lod).r);
  let v10 = f32(textureLoad(image, vec2<i32>(hi.x, lo.y), lod).r);
  let v01 = f32(textureLoad(image, vec2<i32>(lo.x, hi.y), lod).r);
  let v11 = f32(textureLoad(image, vec2<i32>(hi.x, hi.y), lod).r);
  return mix(mix(v00, v10, frac.x), mix(v01, v11, frac.x), frac.y);
}

@fragment
fn fs(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
  // 显示方向变换：quad 提供的是显示 UV，这里映射到原图 UV 再采样。
  let image_uv = orient_uv(uv);
  var value: f32;
  if (params.linear > 0.5) {
    value = sampleBilinear(image_uv);
  } else {
    value = sampleNearest(image_uv);
  }
  // 窗宽窗位映射：两种公式都必须与 C++ 侧 voilut::display_ratio 完全一致。
  var mapped: f32;
  if (params.mode > 0.5) {
    // DICOM PS3.3 C.11.2.1.2.1：下界 L-0.5-(W-1)/2，分母 W-1；W==1 时为阈值。
    if (params.width <= 1.0) {
      mapped = select(0.0, 1.0, value > params.level - 0.5);
    } else {
      let center = params.level - 0.5;
      let lower = center - (params.width - 1.0) * 0.5;
      let upper = center + (params.width - 1.0) * 0.5;
      if (value <= lower) {
        mapped = 0.0;
      } else if (value > upper) {
        mapped = 1.0;
      } else {
        mapped = clamp((value - center) / (params.width - 1.0) + 0.5, 0.0, 1.0);
      }
    }
  } else {
    // 本项目简化公式：把 [L - W/2, L + W/2] 线性拉伸到 [0,1]。
    let lower = params.level - params.width * 0.5;
    mapped = clamp((value - lower) / params.width, 0.0, 1.0);
  }
  if (params.invert > 0.5) {
    mapped = 1.0 - mapped;
  }
  return vec4<f32>(mapped, mapped, mapped, 1.0);
}
