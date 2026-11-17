/// @file image_decode.h
/// @brief 图像解码的统一入口（按类型分派 TIFF / DICOM）。
/// @details 按后缀分派到 TIFF / DICOM 解码器。

#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "image_model.h"

namespace voilut
{
    /// @brief 解码时允许的最大像素内存（多帧 / 多页超过时只加载前 N 帧）。
    /// @details 多帧需要一次性读入 GDCM 缓冲区，超出上限的巨型序列应先做降采样或
    ///          按需分帧解码（尚未实现）。
    inline constexpr size_t kMaxDecodeBytes = 1'500'000'000;
    /// @brief 图像类型。
    enum class ImageKind
    {
        Tiff,  ///< TIFF（.tif / .tiff）。
        Dicom  ///< DICOM（.dcm）。
    };

    /// @brief 依据文件名后缀判断图像类型（大小写不敏感）。
    /// @param name 文件名（可含路径）。
    /// @return 未知后缀返回 `std::nullopt`。
    [[nodiscard]] std::optional<ImageKind> kind_from_name(std::string_view name) noexcept;

    /// @brief 解码图像文件（按 `kind` 分派）。
    /// @param path 文件路径。
    /// @param kind 图像类型。
    /// @return 成功返回 `DecodedImage`，失败返回错误说明。
    [[nodiscard]] std::expected<DecodedImage, std::string> decode_image(const std::string &path,
                                                                        ImageKind kind);
} // namespace voilut
