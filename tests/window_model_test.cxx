/// @file window_model_test.cxx
/// @brief 纯计算单元测试：自动窗 / ROI 窗 / 拖动调窗 / 预设窗四条路径。
/// @details 不创建窗口、不初始化 Dawn，无 GPU 环境也能运行（CTest 目标 `window_model_test`）。
#include "image_model.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

using namespace voilut;

namespace
{
    int g_failures = 0;

    void check(bool ok, const char *what)
    {
        std::printf("  [%s] %s\n", ok ? "OK" : "FAIL", what);
        if (!ok)
        {
            ++g_failures;
        }
    }

    bool near(double a, double b, double eps = 1e-9) { return std::abs(a - b) <= eps; }
} // namespace

int main()
{
    constexpr uint32_t W = 256;
    constexpr uint32_t H = 256;
    std::vector<uint16_t> pixels(static_cast<size_t>(W) * H);
    for (uint32_t y = 0; y < H; ++y)
    {
        for (uint32_t x = 0; x < W; ++x)
        {
            // 左半低值区 [1000, 2000)，右半高值区 [30000, 40000)，叠加轻微纹理
            const uint32_t v = x < W / 2
                                   ? 1000u + x * 1000u / (W / 2) + (x * y) % 13u
                                   : 30000u + (x - W / 2) * 10000u / (W / 2) + (x * y) % 13u;
            pixels[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>(v);
        }
    }

    const ImageStats stats = compute_stats(pixels);
    ImageMeta meta;
    meta.width = W;
    meta.height = H;
    meta.bits_per_sample = 16;
    meta.min_value = stats.min_value;
    meta.max_value = stats.max_value;
    meta.p1 = stats.p1;
    meta.p99 = stats.p99;
    meta.value_range = stats.value_range;

    std::printf("统计: min=%.0f max=%.0f p1=%.0f p99=%.0f\n", stats.min_value, stats.max_value,
                stats.p1, stats.p99);

    // ---------------- 1) 自动窗 ----------------
    std::printf("1) 自动窗\n");
    const WindowSetting auto_w = auto_window(meta);
    std::printf("     level=%.1f width=%.1f\n", auto_w.level, auto_w.width);
    check(auto_w.width >= 1.0, "窗宽 >= 1");
    check(auto_w.level > stats.min_value && auto_w.level < stats.max_value, "窗位落在值域内");
    check(near(auto_w.level, (stats.p1 + stats.p99) / 2.0, 1e-6), "窗位 = (p1+p99)/2");
    const double auto_lo = display_ratio(auto_w.level - auto_w.width / 2.0, auto_w);
    const double auto_hi = display_ratio(auto_w.level + auto_w.width / 2.0, auto_w);
    check(auto_lo < 0.02 && auto_hi > 0.98, "窗边界映射到 0 / 1");

    // ---------------- 2) ROI 窗 ----------------
    std::printf("2) ROI 窗\n");
    const Rect roi{static_cast<double>(W) / 2.0, 0.0, static_cast<double>(W) / 2.0,
                   static_cast<double>(H)};
    const auto roi_w = window_for_roi(pixels, W, H, roi);
    check(roi_w.has_value(), "右半 ROI 有效");
    if (roi_w)
    {
        std::printf("     level=%.1f width=%.1f\n", roi_w->level, roi_w->width);
        check(std::abs(roi_w->level - auto_w.level) > 1000.0, "ROI 窗与整图自动窗明显不同");
        check(roi_w->level > auto_w.level + 1000.0, "ROI 窗偏向高值区");
        check(window_for_roi(pixels, W, H, Rect{0, 0, 2, 2}).has_value() == false, "过小 ROI 返回 nullopt");
    }

    // ---------------- 3) 拖动调窗 ----------------
    std::printf("3) 拖动调窗\n");
    WindowSetting wider = auto_w;
    const WindowSetting wider_after = drag_window(wider, 40.0, 0.0);
    check(wider_after.width > auto_w.width, "右拖增大窗宽");
    WindowSetting narrower = auto_w;
    const WindowSetting narrower_after = drag_window(narrower, -40.0, 0.0);
    check(narrower_after.width < auto_w.width, "左拖减小窗宽");
    WindowSetting shifted = auto_w;
    const WindowSetting shifted_after = drag_window(shifted, 0.0, 30.0);
    check(shifted_after.level < auto_w.level, "下拖降低窗位（上拖变亮）");
    // 沿窗边界取探针值：窗变窄后同一像素必然落到窗外（映射被钳制）
    const double probe_value = auto_w.level - auto_w.width * 0.45;
    WindowSetting narrow_drag = auto_w;
    const WindowSetting narrow_drag_after = drag_window(narrow_drag, -200.0, 0.0);
    check(std::abs(display_ratio(probe_value, auto_w) -
                   display_ratio(probe_value, narrow_drag_after)) > 1e-3,
          "调窗改变同一像素的显示灰度");
    WindowSetting clamp_wide{32768.0, 65535.0};
    check(drag_window(clamp_wide, 100000.0, 0.0).width <= 65535.0, "窗宽上限 65535");
    WindowSetting clamp_narrow{32768.0, 1.0};
    check(drag_window(clamp_narrow, -100000.0, 0.0).width >= 1.0, "窗宽下限 1");

    // ---------------- 4) 预设窗 ----------------
    std::printf("4) 预设窗\n");
    WindowSetting previous = preset_window(meta, kWindowPresets[0]);
    check(previous.width >= 1.0, "预设窗宽 >= 1");
    bool descending = true;
    for (size_t i = 1; i < 4; ++i) // 全范围 / 宽 / 中 / 窄：窗宽应递减
    {
        const WindowSetting current = preset_window(meta, kWindowPresets[i]);
        std::printf("     %s: level=%.1f width=%.1f\n", kWindowPresets[i].name, current.level,
                    current.width);
        descending = descending && current.width < previous.width;
        previous = current;
    }
    check(descending, "全范围 → 宽 → 中 → 窄 窗宽递减");
    const WindowSetting low = preset_window(meta, kWindowPresets[4]);
    const WindowSetting high = preset_window(meta, kWindowPresets[5]);
    check(low.level < high.level, "低值窗窗位低于高值窗");
    const WindowSetting narrow = preset_window(meta, kWindowPresets[3]);
    check(std::abs(display_ratio(probe_value, auto_w) - display_ratio(probe_value, narrow)) > 1e-3,
          "预设窗改变同一像素的显示灰度");

    // ---------------- 5) VOI 映射公式（简化 / DICOM） ----------------
    std::printf("5) VOI 映射公式\n");
    const WindowSetting sample{100.0, 100.0};
    // 简化公式：下界 50，上界 150，中心 100。
    check(near(display_ratio(50.0, sample), 0.0), "简化：下界 → 0");
    check(near(display_ratio(100.0, sample), 0.5), "简化：中心 → 0.5");
    check(near(display_ratio(150.0, sample), 1.0), "简化：上界 → 1");
    // DICOM 精确公式：下界 50、上界 149、中心 99.5，分母 W-1。
    check(near(display_ratio(50.0, sample, false, VoiMode::Dicom), 0.0), "DICOM：下界 → 0");
    check(near(display_ratio(99.5, sample, false, VoiMode::Dicom), 0.5), "DICOM：中心 → 0.5");
    check(near(display_ratio(149.0, sample, false, VoiMode::Dicom), 1.0), "DICOM：上界 → 1");
    check(std::abs(display_ratio(149.0, sample) -
                   display_ratio(149.0, sample, false, VoiMode::Dicom)) > 1e-6,
          "两模式在边界附近结果不同");
    check(near(display_ratio(50.0, sample, true, VoiMode::Dicom), 1.0), "DICOM：反色生效");
    // W == 1：DICOM 退化为阈值。
    const WindowSetting unit_width{100.0, 1.0};
    check(near(display_ratio(99.5, unit_width, false, VoiMode::Dicom), 0.0),
          "DICOM：W=1 阈值以下 → 0");
    check(near(display_ratio(100.0, unit_width, false, VoiMode::Dicom), 1.0),
          "DICOM：W=1 阈值以上 → 1");

    // ---------------- 6) 浮点图像（统计 / 窗宽相对下限） ----------------
    std::printf("6) 浮点图像\n");
    const std::vector<float> floats = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f};
    const ImageStats float_stats = compute_stats_float(floats);
    check(near(float_stats.min_value, 0.0) && near(float_stats.max_value, 2.0),
          "浮点 min/max 正确");
    check(float_stats.p1 >= 0.0 && float_stats.p99 <= 2.0, "浮点分位数在值域内");
    ImageMeta float_meta;
    float_meta.width = 5;
    float_meta.height = 1;
    float_meta.is_float = true;
    float_meta.min_value = float_stats.min_value;
    float_meta.max_value = float_stats.max_value;
    float_meta.p1 = float_stats.p1;
    float_meta.p99 = float_stats.p99;
    float_meta.value_range = float_stats.value_range;
    const WindowSetting float_auto = auto_window(float_meta);
    check(float_auto.width > 0.0 && float_auto.width <= 2.0, "浮点自动窗不被 100 下限扩大");
    const WindowSetting float_full = preset_window(float_meta, kWindowPresets[0]);
    check(near(float_full.width, 2.0), "浮点全范围预设宽度 = 值域");
    WindowSetting float_drag = float_auto;
    check(drag_window(float_drag, -1000.0, 0.0, 1e-3, 10.0).width >= 1e-3,
          "浮点拖动窗宽遵循自定义下限");
    check(pixel_at_float(floats, 5, 1, 2.2, 0.0).has_value() &&
              near(*pixel_at_float(floats, 5, 1, 2.2, 0.0), 1.0),
          "浮点探针取值正确");
    const Histogram float_hist = compute_histogram_float(floats, 0.0, 2.01);
    uint32_t float_sum = 0;
    for (const uint32_t count : float_hist.bins)
    {
        float_sum += count;
    }
    check(float_sum == floats.size(), "浮点直方图计数完整");

    // ---------------- 7) 填充值排除（DICOM PixelPaddingValue） ----------------
    std::printf("7) 填充值排除\n");
    std::vector<uint16_t> padded(16, 0); // 前 8 个为填充值 0
    for (size_t i = 8; i < padded.size(); ++i)
    {
        padded[i] = static_cast<uint16_t>(100 + i);
    }
    const ValueExclusion exclude_padding{.enabled = true, .low = 0.0, .high = 0.0};
    const ImageStats padded_stats = compute_stats(padded, exclude_padding);
    check(padded_stats.min_value == 108.0, "统计排除填充值后的最小值");
    check(padded_stats.max_value == 115.0, "统计排除填充值后的最大值");
    const Histogram padded_hist = compute_histogram(padded, 0.0, 200.0, exclude_padding);
    uint32_t padded_sum = 0;
    for (const uint32_t count : padded_hist.bins)
    {
        padded_sum += count;
    }
    check(padded_sum == 8, "直方图排除填充值");
    const auto padded_roi = window_for_roi(padded, 4, 4, Rect{0.0, 0.0, 4.0, 4.0},
                                           exclude_padding);
    check(padded_roi.has_value(), "ROI 窗在排除填充值后仍有效");
    check(padded_roi.has_value() && padded_roi->level >= 108.0, "ROI 窗不被填充值拉低");

    std::printf("\n%s（失败 %d 项）\n", g_failures == 0 ? "全部通过" : "存在失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
