/// @file image_model.h
/// @brief 16 位灰度图像的数据模型与统计（窗宽窗位）。
/// @details 统计走 65536 桶直方图（一次遍历同时得到 min/max 与 p1/p99）。

#pragma once

#include <algorithm> // std::clamp
#include <array>     // std::array
#include <cmath>     // std::exp
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace voilut
{
    /// @brief 图像元数据与统计信息。
    struct ImageMeta
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t bits_per_sample = 0;
        uint32_t samples_per_pixel = 1;
        uint32_t frame_count = 1;   ///< 帧数（多页 TIFF / 多帧 DICOM；目前只显示首帧）。
        double min_value = 0.0;
        double max_value = 0.0;
        double p1 = 0.0;
        double p99 = 0.0;
        double value_range = 0.0;
    };

    /// @brief 窗宽窗位设置。
    struct WindowSetting
    {
        double level = 0.0;
        double width = 1.0;
    };

    /// @brief 图像坐标系的矩形（ROI）。
    struct Rect
    {
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
    };

    /// @brief 解码完成的图像。
    struct DecodedImage
    {
        std::string name;                              ///< 文件名 / 数据源名。
        ImageMeta meta;                                ///< 元数据与统计。
        std::vector<uint16_t> pixels;                  ///< 16 位像素（行优先）。
        std::optional<WindowSetting> default_window;   ///< 文件自带的默认窗（DICOM）。
        bool initial_invert = false;                   ///< 初始是否反色（MONOCHROME1）。
    };

    /// @brief 像素统计结果。
    struct ImageStats
    {
        double min_value = 0.0;
        double max_value = 0.0;
        double p1 = 0.0;
        double p99 = 0.0;
        double value_range = 0.0;
    };

    /// @brief 统计 min/max 与 1% / 99% 分位数（直方图实现）。
    /// @param pixels 16 位像素。
    /// @return 统计结果；空输入返回全 0。
    [[nodiscard]] ImageStats compute_stats(std::span<const uint16_t> pixels);

    /// @brief 依据 p1/p99 计算默认窗（窄窗拉伸对比度）。
    /// @param meta 含 p1/p99 的元数据。
    /// @return 默认窗设置。
    [[nodiscard]] WindowSetting auto_window(const ImageMeta &meta) noexcept;

    /// @brief 计算 ROI 内的 p1/p99 自动窗（步进采样，样本上限 50 万）。
    /// @param pixels 16 位像素。
    /// @param width 图像宽。
    /// @param height 图像高。
    /// @param roi ROI（图像坐标，越界部分裁剪）。
    /// @return ROI 无效（宽或高 < 4）时返回 `std::nullopt`。
    [[nodiscard]] std::optional<WindowSetting> window_for_roi(std::span<const uint16_t> pixels,
                                                              uint32_t width, uint32_t height,
                                                              const Rect &roi);

    /// @brief 读取指定图像坐标的像素值。
    /// @param pixels 16 位像素。
    /// @param width 图像宽。
    /// @param height 图像高。
    /// @param x 图像坐标 x（可为小数，向下取整）。
    /// @param y 图像坐标 y（可为小数，向下取整）。
    /// @return 越界时返回 `std::nullopt`。
    [[nodiscard]] std::optional<uint16_t> pixel_at(std::span<const uint16_t> pixels, uint32_t width,
                                                   uint32_t height, double x, double y) noexcept;

    // ========================================================
    // 调窗核心 API：直方图（可视化）、预设窗（相对值域）、
    // 拖动调窗（鼠标灵敏度换算），以及像素值 → 显示灰度换算
    // （与着色器映射一致，供探针读数使用）。
    // =========================================================

    /// @brief 显示用直方图的桶数。
    inline constexpr uint32_t kHistogramBins = 256;

    /// @brief 显示用直方图（把 16 位值域压缩到 256 桶）。
    struct Histogram
    {
        std::array<uint32_t, kHistogramBins> bins{}; ///< 各桶计数。
        double low = 0.0;                            ///< 第 0 桶左边界（像素值）。
        double high = 1.0;                           ///< 末桶右边界（像素值）。
        uint32_t peak = 0;                           ///< 最大桶计数（绘制时用于归一化）。
    };

    /// @brief 在 `[low, high]` 区间上统计 256 桶直方图。
    /// @param pixels 16 位像素。
    /// @param low 区间下界（像素值，通常取 `meta.min_value`）。
    /// @param high 区间上界（像素值，通常取 `meta.max_value`）。
    /// @return 直方图；区间非法或输入为空时返回全零。
    [[nodiscard]] Histogram compute_histogram(std::span<const uint16_t> pixels, double low,
                                              double high);

    /// @brief 预设窗：以图像值域的比例描述，跨设备 / 跨模态通用。
    struct WindowPreset
    {
        const char *name = "";     ///< 按钮显示名。
        double center_ratio = 0.5;  ///< 窗位在值域中的相对位置（0 = min，1 = max）。
        double width_ratio = 1.0;   ///< 窗宽相对整个值域的比例。
    };

    /// @brief 内置预设窗（只依赖图像统计，无需为每个模态单独配置）。
    inline constexpr std::array<WindowPreset, 6> kWindowPresets = {{
        {"全范围", 0.5, 1.0},
        {"宽窗 80%", 0.5, 0.8},
        {"中窗 50%", 0.5, 0.5},
        {"窄窗 20%", 0.5, 0.2},
        {"低值窗", 0.25, 0.3},
        {"高值窗", 0.75, 0.3},
    }};

    /// @brief 把预设换算成具体窗设置。
    /// @param meta 图像元数据（用 `min_value` / `value_range`）。
    /// @param preset 预设。
    /// @return 窗设置；`width` 最小为 1。
    [[nodiscard]] WindowSetting preset_window(const ImageMeta &meta,
                                              const WindowPreset &preset) noexcept;

    /// @brief 一次鼠标拖动换算出的新窗（水平调窗宽、垂直调窗位）。
    /// @param window 当前窗（原地更新，并为返回值）。
    /// @param dx 屏幕水平位移（像素，右为正）。
    /// @param dy 屏幕垂直位移（像素，下为正）。
    /// @return 更新后的窗；`width` 限制在 [1, 65535]。
    /// @note 窗宽按指数缩放（不同窗宽下的操作灵敏度一致）；窗位按当前窗宽的比例平移
    ///       （窄窗时步进更小，宽窗时步进更大）。
    [[nodiscard]] WindowSetting drag_window(WindowSetting &window, double dx, double dy) noexcept;

    /// @brief 像素值 → 显示灰度系数的映射公式。
    /// @details 两种模式只影响「窗 → 灰度」的线性区间与边界，不影响自动窗 / ROI 窗的计算。
    /// @note C++ 侧 `display_ratio` 与 `shaders/image.wgsl` 必须同时支持两种模式。
    enum class VoiMode
    {
        Simplified, ///< 本项目简化公式：`(v - (L - W/2)) / W`（默认，整数域更直观）。
        Dicom,      ///< DICOM PS3.3 C.11.2.1.2.1 精确公式：下界 `L-0.5-(W-1)/2`，分母 `W-1`。
    };

    /// @brief 像素值 → 显示灰度系数（0..1）。
    /// @param value 像素值。
    /// @param window 窗设置。
    /// @param invert 是否反色。
    /// @param mode 映射公式（默认简化公式）。
    /// @return 映射后的灰度系数；超出窗范围时限制到 0 或 1。
    /// @note DICOM 模式下 `width == 1` 退化为阈值：`value > level - 0.5` 输出 1。
    [[nodiscard]] double display_ratio(double value, const WindowSetting &window,
                                       bool invert = false,
                                       VoiMode mode = VoiMode::Simplified) noexcept;
} // namespace voilut
