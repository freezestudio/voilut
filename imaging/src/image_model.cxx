/// @file image_model.cxx
/// @brief 16 位图像统计与窗宽窗位实现。

#include "image_model.h"

#include <algorithm> // std::max / std::min
#include <cmath>     // std::floor / std::ceil

namespace voilut
{
    ImageStats compute_stats(std::span<const uint16_t> pixels)
    {
        if (pixels.empty())
        {
            return {};
        }

        // 16 位直方图：一次遍历同时得到 min/max 与分布
        std::vector<uint32_t> histogram(65536, 0);
        uint32_t min_value = 65535;
        uint32_t max_value = 0;
        for (const uint16_t value : pixels)
        {
            ++histogram[value];
            min_value = std::min(min_value, static_cast<uint32_t>(value));
            max_value = std::max(max_value, static_cast<uint32_t>(value));
        }

        // 1% 分位数：低端从 0 向上累计，高端从 65535 向下累计
        const size_t rank = std::max<size_t>(1, pixels.size() / 100);
        uint32_t p1 = min_value;
        uint32_t p99 = max_value;
        bool p1_found = false;
        uint64_t acc = 0;
        for (uint32_t value = 0; value < 65536; ++value)
        {
            acc += histogram[value];
            if (!p1_found && acc >= rank)
            {
                p1 = value;
                p1_found = true;
            }
        }
        acc = 0;
        for (int value = 65535; value >= 0; --value)
        {
            acc += histogram[value];
            if (acc >= rank)
            {
                p99 = static_cast<uint32_t>(value);
                break;
            }
        }
        if (p99 <= p1)
        {
            p99 = p1 + 1;
        }

        return {
            .min_value = static_cast<double>(min_value),
            .max_value = static_cast<double>(max_value),
            .p1 = static_cast<double>(p1),
            .p99 = static_cast<double>(p99),
            .value_range = static_cast<double>(max_value - min_value),
        };
    }

    WindowSetting auto_window(const ImageMeta &meta) noexcept
    {
        return {
            .level = (meta.p1 + meta.p99) * 0.5,
            .width = std::max(meta.p99 - meta.p1, 100.0),
        };
    }

    std::optional<WindowSetting> window_for_roi(std::span<const uint16_t> pixels, uint32_t width,
                                                uint32_t height, const Rect &roi)
    {
        const int64_t x0 = std::max<int64_t>(0, static_cast<int64_t>(std::floor(roi.x)));
        const int64_t y0 = std::max<int64_t>(0, static_cast<int64_t>(std::floor(roi.y)));
        const int64_t x1 =
            std::min<int64_t>(width, static_cast<int64_t>(std::ceil(roi.x + roi.width)));
        const int64_t y1 =
            std::min<int64_t>(height, static_cast<int64_t>(std::ceil(roi.y + roi.height)));
        const int64_t rw = x1 - x0;
        const int64_t rh = y1 - y0;
        if (rw < 4 || rh < 4)
        {
            return std::nullopt;
        }

        // 步进采样：把样本量压到 50 万以内，保证大范围 ROI 的耗时可控
        constexpr int64_t kTargetSamples = 500000;
        int64_t stride = 1;
        while (((rw + stride - 1) / stride) * ((rh + stride - 1) / stride) > kTargetSamples)
        {
            ++stride;
        }

        std::vector<uint32_t> hist(65536, 0);
        const int64_t step_w = (rw + stride - 1) / stride;
        const int64_t step_h = (rh + stride - 1) / stride;
        for (int64_t y = 0; y < step_h; ++y)
        {
            const int64_t row = (y0 + y * stride) * width + x0;
            for (int64_t x = 0; x < step_w; ++x)
            {
                ++hist[pixels[static_cast<size_t>(row + x * stride)]];
            }
        }

        const int64_t n = step_w * step_h;
        const int64_t rank = std::max<int64_t>(1, n / 100);
        uint32_t p1 = 0;
        uint32_t p99 = 65535;
        bool p1_found = false;
        int64_t acc = 0;
        for (uint32_t value = 0; value < 65536; ++value)
        {
            acc += hist[value];
            if (!p1_found && acc >= rank)
            {
                p1 = value;
                p1_found = true;
            }
        }
        acc = 0;
        for (int value = 65535; value >= 0; --value)
        {
            acc += hist[value];
            if (acc >= rank)
            {
                p99 = static_cast<uint32_t>(value);
                break;
            }
        }
        if (p99 <= p1)
        {
            p99 = p1 + 1;
        }

        return WindowSetting{
            .level = (static_cast<double>(p1) + static_cast<double>(p99)) * 0.5,
            .width = std::max(static_cast<double>(p99) - static_cast<double>(p1), 100.0),
        };
    }

    std::optional<uint16_t> pixel_at(std::span<const uint16_t> pixels, uint32_t width,
                                     uint32_t height, double x, double y) noexcept
    {
        const int64_t ix = static_cast<int64_t>(std::floor(x));
        const int64_t iy = static_cast<int64_t>(std::floor(y));
        if (ix < 0 || iy < 0 || ix >= static_cast<int64_t>(width) ||
            iy >= static_cast<int64_t>(height))
        {
            return std::nullopt;
        }
        return pixels[static_cast<size_t>(iy) * width + static_cast<size_t>(ix)];
    }

    Histogram compute_histogram(std::span<const uint16_t> pixels, double low, double high)
    {
        Histogram hist{};
        if (pixels.empty() || !(high > low))
        {
            return hist;
        }
        hist.low = low;
        hist.high = high;

        // 像素值 → 桶序号：先归一化再乘桶数，两端闭合（值 == high 落入末桶）。
        const double span = high - low;
        const double scale = static_cast<double>(kHistogramBins) / span;
        for (const uint16_t value : pixels)
        {
            const double pos = (static_cast<double>(value) - low) * scale;
            const auto bin = static_cast<int64_t>(pos);
            if (bin < 0 || bin >= static_cast<int64_t>(kHistogramBins))
            {
                continue; // 直方图只统计 [low, high] 内的值
            }
            ++hist.bins[static_cast<size_t>(bin)];
        }
        hist.peak = *std::max_element(hist.bins.begin(), hist.bins.end());
        return hist;
    }

    WindowSetting preset_window(const ImageMeta &meta, const WindowPreset &preset) noexcept
    {
        const double range = std::max(meta.value_range, 1.0);
        return {
            .level = meta.min_value + range * preset.center_ratio,
            .width = std::max(range * preset.width_ratio, 1.0),
        };
    }

    WindowSetting drag_window(WindowSetting &window, double dx, double dy) noexcept
    {
        // 水平：窗宽指数缩放（拖动 600 像素约 ×e），向右拖动增大窗宽、降低对比度。
        window.width = std::clamp(window.width * std::exp(dx / 600.0), 1.0, 65535.0);
        // 垂直：窗位按当前窗宽的比例平移（向上拖动变亮），窄窗时变化幅度自动减小。
        window.level -= dy * window.width / 400.0;
        return window;
    }

    double display_ratio(double value, const WindowSetting &window, bool invert,
                         VoiMode mode) noexcept
    {
        if (mode == VoiMode::Dicom)
        {
            // DICOM PS3.3 C.11.2.1.2.1（线性 VOI LUT）：
            //   下界 = L - 0.5 - (W-1)/2，上界 = L - 0.5 + (W-1)/2
            //   W == 1 时退化为阈值（分母为 0）。
            const double width = std::max(window.width, 1e-6);
            double ratio = 0.0;
            if (width <= 1.0)
            {
                ratio = value > window.level - 0.5 ? 1.0 : 0.0;
            }
            else
            {
                const double center = window.level - 0.5;
                const double lower = center - (width - 1.0) * 0.5;
                const double upper = center + (width - 1.0) * 0.5;
                if (value <= lower)
                {
                    ratio = 0.0;
                }
                else if (value > upper)
                {
                    ratio = 1.0;
                }
                else
                {
                    ratio = std::clamp((value - center) / (width - 1.0) + 0.5, 0.0, 1.0);
                }
            }
            return invert ? 1.0 - ratio : ratio;
        }

        const double width = std::max(window.width, 1e-6);
        const double lower = window.level - width * 0.5;
        const double ratio = std::clamp((value - lower) / width, 0.0, 1.0);
        return invert ? 1.0 - ratio : ratio;
    }
} // namespace voilut
