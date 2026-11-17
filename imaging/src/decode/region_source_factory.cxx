/// @file region_source_factory.cxx
/// @brief 按后缀分派 TIFF / DICOM 区域源。

#include "region_source.h"

#include <cctype>
#include <format>
#include <string>

namespace voilut
{
    namespace
    {
        std::string lower_suffix(const std::string &path)
        {
            const size_t dot = path.find_last_of('.');
            if (dot == std::string::npos)
            {
                return {};
            }
            std::string suffix = path.substr(dot + 1);
            for (char &c : suffix)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return suffix;
        }
    } // namespace

    std::expected<std::unique_ptr<RegionSource>, std::string>
    open_region_source(const std::string &path)
    {
        const std::string suffix = lower_suffix(path);
        if (suffix == "tif" || suffix == "tiff")
        {
            return open_tiff_region_source(path);
        }
        if (suffix == "dcm" || suffix == "dicom")
        {
            return open_dicom_region_source(path);
        }
        return std::unexpected(std::format("不支持的图像格式：{}", path));
    }
} // namespace voilut
