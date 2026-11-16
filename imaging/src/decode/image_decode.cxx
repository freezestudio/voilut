/// @file image_decode.cxx
/// @brief 图像解码的统一入口实现。

#include "decode/image_decode.h"

#include <cstddef>

#include "decode/dicom_decode.h"
#include "decode/tiff_decode.h"

namespace voilut
{
    namespace
    {
        /// @brief 判断字符串是否以指定后缀结尾（大小写不敏感）。
        bool ends_with_ci(std::string_view text, std::string_view suffix) noexcept
        {
            if (text.size() < suffix.size())
            {
                return false;
            }
            const size_t offset = text.size() - suffix.size();
            for (size_t i = 0; i < suffix.size(); ++i)
            {
                const char lhs = text[offset + i];
                const char rhs = suffix[i];
                const char lhs_lower = (lhs >= 'A' && lhs <= 'Z') ? static_cast<char>(lhs + 32) : lhs;
                const char rhs_lower = (rhs >= 'A' && rhs <= 'Z') ? static_cast<char>(rhs + 32) : rhs;
                if (lhs_lower != rhs_lower)
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    std::optional<ImageKind> kind_from_name(std::string_view name) noexcept
    {
        if (ends_with_ci(name, ".dcm"))
        {
            return ImageKind::Dicom;
        }
        if (ends_with_ci(name, ".tif") || ends_with_ci(name, ".tiff"))
        {
            return ImageKind::Tiff;
        }
        return std::nullopt;
    }

    std::expected<DecodedImage, std::string> decode_image(const std::string &path, ImageKind kind)
    {
        switch (kind)
        {
        case ImageKind::Tiff:
            return decode_tiff(path);
        case ImageKind::Dicom:
            return decode_dicom(path);
        }
        return std::unexpected(std::string("未知的图像类型"));
    }
} // namespace voilut
