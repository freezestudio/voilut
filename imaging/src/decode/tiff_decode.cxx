/// @file tiff_decode.cxx
/// @brief 16 位单通道灰度 TIFF 解码（libtiff 实现）。

#include "decode/tiff_decode.h"

#include <cstdint>
#include <cstring> // std::memcpy
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include <tiffio.h>

namespace voilut
{
    namespace
    {
        /// @brief TIFF 句柄的 RAII 包装（退出时自动 TIFFClose）。
        using TiffHandle = std::unique_ptr<TIFF, decltype(&TIFFClose)>;

        /// @brief 取路径中的文件名部分。
        std::string file_name_of(const std::string &path)
        {
            const size_t slash = path.find_last_of("/\\");
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        /// @brief 打开 TIFF：项目内部路径统一为 UTF-8。
        /// @details libtiff 的 `TIFFOpen` 在 Windows 上按 ANSI 代码页解释路径，
        ///          中文文件名会打不开；这里转成宽路径调用 `TIFFOpenW`。
        /// @param path UTF-8 路径。
        /// @return TIFF 句柄，失败时为 nullptr。
        TIFF *open_tiff(const std::string &path)
        {
#if defined(_WIN32)
            const std::filesystem::path wide_path(std::u8string(path.begin(), path.end()));
            return TIFFOpenW(wide_path.c_str(), "r");
#else
            return TIFFOpen(path.c_str(), "r");
#endif
        }
    } // namespace

    std::expected<DecodedImage, std::string> decode_tiff(const std::string &path)
    {
        TiffHandle tif(open_tiff(path), &TIFFClose);
        if (!tif)
        {
            return std::unexpected(std::format("无法打开 TIFF 文件：{}", path));
        }

        uint32_t width = 0;
        uint32_t height = 0;
        if (TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &width) != 1 ||
            TIFFGetField(tif.get(), TIFFTAG_IMAGELENGTH, &height) != 1 || width == 0 || height == 0)
        {
            return std::unexpected("TIFF 文件不包含有效的图像尺寸");
        }

        uint16_t bits_per_sample = 0;
        uint16_t samples_per_pixel = 1;
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_BITSPERSAMPLE, &bits_per_sample);
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel);

        if ((bits_per_sample != 8 && bits_per_sample != 16) || samples_per_pixel != 1)
        {
            return std::unexpected(
                std::format("仅支持 8 位或 16 位单通道灰度 TIFF，当前为 {}bit × {} 通道",
                            bits_per_sample, samples_per_pixel));
        }

        // 逐行读取：分配一块扫描行缓冲，避免依赖具体的 strip/tile 组织。
        const tsize_t scanline_size = TIFFScanlineSize(tif.get());
        const tsize_t bytes_per_pixel = bits_per_sample / 8;
        if (scanline_size < static_cast<tsize_t>(width) * bytes_per_pixel)
        {
            return std::unexpected("TIFF 扫描行尺寸异常（小于图像宽度 × 每像素字节数）");
        }

        std::vector<uint16_t> pixels(static_cast<size_t>(width) * height);
        std::vector<uint8_t> row_buffer(static_cast<size_t>(scanline_size));
        for (uint32_t row = 0; row < height; ++row)
        {
            if (TIFFReadScanline(tif.get(), row_buffer.data(), row) < 0)
            {
                return std::unexpected(std::format("读取 TIFF 第 {} 行失败", row));
            }
            const size_t base = static_cast<size_t>(row) * width;
            if (bits_per_sample == 8)
            {
                for (uint32_t x = 0; x < width; ++x)
                {
                    pixels[base + x] = row_buffer[x];
                }
            }
            else
            {
                std::memcpy(pixels.data() + base, row_buffer.data(),
                            static_cast<size_t>(width) * 2);
            }
        }

        const ImageStats stats = compute_stats(pixels);
        DecodedImage image;
        image.name = file_name_of(path);
        image.meta = {
            .width = width,
            .height = height,
            .bits_per_sample = bits_per_sample,
            .samples_per_pixel = samples_per_pixel,
            .min_value = stats.min_value,
            .max_value = stats.max_value,
            .p1 = stats.p1,
            .p99 = stats.p99,
            .value_range = stats.value_range,
        };
        image.pixels = std::move(pixels);
        return image;
    }
} // namespace voilut
