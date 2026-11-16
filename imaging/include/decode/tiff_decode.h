/// @file tiff_decode.h
/// @brief 16 位单通道灰度 TIFF 解码（基于 libtiff）。
/// @details 支持 16 位灰度与常见压缩。

#pragma once

#include <expected>
#include <string>

#include "image_model.h"

namespace voilut
{
    /// @brief 读取并解码 16 位单通道灰度 TIFF。
    /// @param path 文件路径。
    /// @return 成功返回 `DecodedImage`，失败返回错误说明。
    /// @note 仅支持 `bitsPerSample == 16` 且 `samplesPerPixel == 1`。
    [[nodiscard]] std::expected<DecodedImage, std::string> decode_tiff(const std::string &path);
} // namespace voilut
