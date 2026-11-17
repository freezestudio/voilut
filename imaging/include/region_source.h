/// @file region_source.h
/// @brief 支持区域读取的图像源（虚拟切片的基础接口）。
/// @details
/// 与 `decode_image` 的一次性全量解码不同，`RegionSource` 用于超大图像：
/// 打开时只读取元数据，预览与细节都按需读取，内存有界。
///
/// Phase 0：TIFF（stripped / tiled / 多页 / 大端 / 浮点）。
/// Phase 2：DICOM `ImageRegionReader` 与压缩回退。

#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "image_model.h"

namespace voilut
{
    /// @brief 像素采样类型。
    enum class SampleType
    {
        UInt16,  ///< 16 位无符号整数。
        Float32, ///< 32 位浮点。
    };

    /// @brief 区域图像源的元数据（不包含像素）。
    struct RegionInfo
    {
        uint32_t width = 0;        ///< 原图宽。
        uint32_t height = 0;       ///< 原图高。
        uint32_t frame_count = 1;  ///< 帧数 / 页数。
        SampleType sample_type = SampleType::UInt16; ///< 像素类型。
        uint32_t bits_per_sample = 0; ///< 每像素位数（8 / 16 / 32）。
        bool initial_invert = false;  ///< MINISWHITE / MONOCHROME1。
        ValueScale scale;              ///< 存储域 ↔ 模态值。
        ValueExclusion padding;        ///< 需要排除的填充值。
        std::optional<WindowSetting> default_window; ///< 文件自带默认窗。
        std::vector<WindowSetting> window_presets;   ///< 文件自带的多组窗值。
        std::string name;              ///< 文件名。
        std::string modality_unit;     ///< 模态单位（如 HU）。
    };

    /// @brief 一块原图区域及其像素。
    struct PixelRegion
    {
        uint32_t x = 0;      ///< 左上角 x（原图坐标）。
        uint32_t y = 0;      ///< 左上角 y。
        uint32_t width = 0;  ///< 宽。
        uint32_t height = 0; ///< 高。
        std::vector<uint16_t> u16; ///< `sample_type == UInt16` 时有效。
        std::vector<float> f32;    ///< `sample_type == Float32` 时有效。
    };

    /// @brief 可按区域读取的图像源。
    /// @note 同一实例上的 `read_region` / `preview` 需要调用方串行化
    ///       （Phase 1 在单个后台解码线程中使用）。
    class RegionSource
    {
    public:
        virtual ~RegionSource() = default;

        RegionSource(const RegionSource &) = delete;
        RegionSource &operator=(const RegionSource &) = delete;

        /// @brief 元数据。
        [[nodiscard]] virtual const RegionInfo &info() const noexcept = 0;

        /// @brief 全图预览（首帧），最长边不超过 `max_dimension`。
        /// @param max_dimension 预览最大边长。
        /// @return 预览图像；失败返回错误说明。
        [[nodiscard]] virtual std::expected<DecodedImage, std::string>
        preview(uint32_t max_dimension) = 0;

        /// @brief 读取一块原图区域。
        /// @param x 左上角 x（越界部分会被裁剪）。
        /// @param y 左上角 y。
        /// @param width 宽。
        /// @param height 高。
        /// @param frame 帧序号（0-based）。
        /// @return 区域像素；失败返回错误说明。
        [[nodiscard]] virtual std::expected<PixelRegion, std::string>
        read_region(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                    uint32_t frame) = 0;

    protected:
        RegionSource() = default;
    };

    /// @brief 按文件后缀打开区域图像源。
    /// @param path 文件路径（UTF-8）。
    /// @return 成功返回图像源；不支持的格式/损坏文件返回错误说明。
    [[nodiscard]] std::expected<std::unique_ptr<RegionSource>, std::string>
    open_region_source(const std::string &path);

    /// @brief 打开 TIFF 区域源（Phase 0）。
    [[nodiscard]] std::expected<std::unique_ptr<RegionSource>, std::string>
    open_tiff_region_source(const std::string &path);

    /// @brief 打开 DICOM 区域源（Phase 2）。
    [[nodiscard]] std::expected<std::unique_ptr<RegionSource>, std::string>
    open_dicom_region_source(const std::string &path);
} // namespace voilut
