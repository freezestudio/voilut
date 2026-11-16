/// @file dicom_decode.h
/// @brief DICOM 解码（基于 GDCM，支持压缩传输语法）。
/// @details 压缩传输语法也由 GDCM 处理。

#pragma once

#include <expected>
#include <string>

#include "image_model.h"

namespace voilut
{
    /// @brief 读取并解码 DICOM 文件。
    /// @param path 文件路径。
    /// @return 成功返回 `DecodedImage`，失败返回错误说明。
    /// @note 仅支持单通道、16 位分配、MONOCHROME 光度解释；
    ///       压缩传输语法（JPEG / JPEG-LS / JPEG2000 / RLE）由 GDCM 解压。
    [[nodiscard]] std::expected<DecodedImage, std::string> decode_dicom(const std::string &path);
} // namespace voilut
