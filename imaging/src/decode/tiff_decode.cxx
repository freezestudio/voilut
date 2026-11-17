/// @file tiff_decode.cxx
/// @brief 16 位单通道灰度 TIFF 解码（libtiff 实现）。
/// @details 支持 stripped / tiled、多页（受内存上限）、8 / 16 位、大端字节交换、
///          PHOTOMETRIC_MINISWHITE；暂不支持浮点 / 有符号 / 多通道。

#include "decode/tiff_decode.h"

#include <algorithm>
#include <cstdint>
#include <cstring> // std::memcpy
#include <filesystem>
#include <format>
#include <memory>
#include <new> // std::bad_alloc
#include <print>
#include <span>
#include <string>
#include <vector>

#include <tiffio.h>

#include "decode/image_decode.h" // kMaxDecodeBytes

namespace voilut
{
    namespace
    {
        /// @brief TIFF 句柄的 RAII 包装（退出时自动 TIFFClose）。
        using TiffHandle = std::unique_ptr<TIFF, decltype(&TIFFClose)>;

        /// @brief 16 位像素字节交换（大端 TIFF 需要）。
        void swap_bytes_16(uint16_t *values, size_t count) noexcept
        {
            for (size_t i = 0; i < count; ++i)
            {
                const uint16_t value = values[i];
                values[i] = static_cast<uint16_t>((value >> 8) | (value << 8));
            }
        }

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

        /// @brief 把当前 TIFF 目录读入 `dst`（统一输出为 16 位）。
        /// @param tif 已定位到目标页。
        /// @param width 图像宽。
        /// @param height 图像高。
        /// @param bits_per_sample 8 或 16。
        /// @param dst 目标缓冲区（width × height）。
        /// @return 成功为空；失败为错误说明。
        std::expected<void, std::string> read_page(TIFF *tif, uint32_t width, uint32_t height,
                                                   uint16_t bits_per_sample, uint16_t *dst)
        {
            const size_t bytes_per_pixel = bits_per_sample / 8;
            const bool byte_swapped = TIFFIsByteSwapped(tif) != 0;

            if (TIFFIsTiled(tif))
            {
                uint32_t tile_width = 0;
                uint32_t tile_height = 0;
                if (TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tile_width) != 1 ||
                    TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_height) != 1 ||
                    tile_width == 0 || tile_height == 0)
                {
                    return std::unexpected(std::string("TIFF 缺少有效的 tile 尺寸"));
                }
                const tmsize_t tile_size = TIFFTileSize(tif);
                if (tile_size <= 0 ||
                    tile_size < static_cast<tmsize_t>(static_cast<size_t>(tile_width) *
                                                       tile_height * bytes_per_pixel))
                {
                    return std::unexpected(std::string("TIFF tile 缓冲区尺寸异常"));
                }
                std::vector<uint8_t> tile(static_cast<size_t>(tile_size));
                for (uint32_t y0 = 0; y0 < height; y0 += tile_height)
                {
                    for (uint32_t x0 = 0; x0 < width; x0 += tile_width)
                    {
                        if (TIFFReadTile(tif, tile.data(), x0, y0, 0, 0) < 0)
                        {
                            return std::unexpected(
                                std::format("读取 TIFF tile ({}, {}) 失败", x0, y0));
                        }
                        const uint32_t copy_width = std::min(tile_width, width - x0);
                        const uint32_t copy_height = std::min(tile_height, height - y0);
                        for (uint32_t ty = 0; ty < copy_height; ++ty)
                        {
                            const uint8_t *src =
                                tile.data() +
                                static_cast<size_t>(ty) * tile_width * bytes_per_pixel;
                            uint16_t *row = dst + static_cast<size_t>(y0 + ty) * width + x0;
                            if (bits_per_sample == 8)
                            {
                                for (uint32_t x = 0; x < copy_width; ++x)
                                {
                                    row[x] = src[x];
                                }
                            }
                            else
                            {
                                std::memcpy(row, src, static_cast<size_t>(copy_width) * 2);
                                if (byte_swapped)
                                {
                                    swap_bytes_16(row, copy_width);
                                }
                            }
                        }
                    }
                }
                return {};
            }

            const tsize_t scanline_size = TIFFScanlineSize(tif);
            if (scanline_size <
                static_cast<tsize_t>(static_cast<size_t>(width) * bytes_per_pixel))
            {
                return std::unexpected(std::string("TIFF 扫描行尺寸异常（小于图像宽度 × 每像素字节数）"));
            }
            std::vector<uint8_t> row_buffer(static_cast<size_t>(scanline_size));
            for (uint32_t row = 0; row < height; ++row)
            {
                if (TIFFReadScanline(tif, row_buffer.data(), row) < 0)
                {
                    return std::unexpected(std::format("读取 TIFF 第 {} 行失败", row));
                }
                uint16_t *dst_row = dst + static_cast<size_t>(row) * width;
                if (bits_per_sample == 8)
                {
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        dst_row[x] = row_buffer[x];
                    }
                }
                else
                {
                    std::memcpy(dst_row, row_buffer.data(), static_cast<size_t>(width) * 2);
                    if (byte_swapped)
                    {
                        swap_bytes_16(dst_row, width);
                    }
                }
            }
            return {};
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
        uint16_t sample_format = SAMPLEFORMAT_UINT;
        uint16_t photometric = PHOTOMETRIC_MINISBLACK;
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_BITSPERSAMPLE, &bits_per_sample);
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel);
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLEFORMAT, &sample_format);
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_PHOTOMETRIC, &photometric);

        if (sample_format == SAMPLEFORMAT_IEEEFP)
        {
            return std::unexpected("暂不支持浮点 TIFF（IEEE float），请先转换为 16 位整数 TIFF");
        }
        if (sample_format == SAMPLEFORMAT_INT || sample_format == SAMPLEFORMAT_COMPLEXINT ||
            sample_format == SAMPLEFORMAT_COMPLEXIEEEFP)
        {
            return std::unexpected("暂不支持有符号 / 复数像素的 TIFF");
        }
        if ((bits_per_sample != 8 && bits_per_sample != 16) || samples_per_pixel != 1)
        {
            return std::unexpected(
                std::format("仅支持 8 位或 16 位单通道灰度 TIFF，当前为 {}bit × {} 通道",
                            bits_per_sample, samples_per_pixel));
        }

        // 多页 TIFF：页数只用于显示/导航；实际加载多少页受 kMaxDecodeBytes 限制。
        const tdir_t directory_count = TIFFNumberOfDirectories(tif.get());
        const uint32_t available_frames =
            directory_count > 0 ? static_cast<uint32_t>(directory_count) : 1;
        if (TIFFSetDirectory(tif.get(), 0) != 1)
        {
            return std::unexpected("无法定位 TIFF 第 0 页（多页文件需要随机访问）");
        }

        const size_t frame_pixels = static_cast<size_t>(width) * height;
        const size_t bytes_per_output_frame = frame_pixels * sizeof(uint16_t);
        const size_t max_frames =
            bytes_per_output_frame == 0
                ? 1
                : std::max<size_t>(1, kMaxDecodeBytes / bytes_per_output_frame);
        const uint32_t frames_to_load =
            static_cast<uint32_t>(std::min<size_t>(available_frames, max_frames));

        std::vector<uint16_t> pixels;
        try
        {
            pixels.resize(frame_pixels * frames_to_load);
        }
        catch (const std::bad_alloc &)
        {
            return std::unexpected(
                std::format("内存不足：无法为 {}×{}×{} 帧分配像素缓冲区", width, height,
                            frames_to_load));
        }

        uint32_t loaded = 0;
        for (uint32_t frame = 0; frame < frames_to_load; ++frame)
        {
            if (frame > 0)
            {
                if (TIFFReadDirectory(tif.get()) != 1)
                {
                    break;
                }
                // 多页文件必须同尺寸/同格式，否则停止（不能把不同尺寸的页拼在一起）。
                uint32_t page_width = 0;
                uint32_t page_height = 0;
                uint16_t page_bits = 0;
                uint16_t page_samples = 1;
                TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &page_width);
                TIFFGetField(tif.get(), TIFFTAG_IMAGELENGTH, &page_height);
                TIFFGetFieldDefaulted(tif.get(), TIFFTAG_BITSPERSAMPLE, &page_bits);
                TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &page_samples);
                if (page_width != width || page_height != height ||
                    page_bits != bits_per_sample || page_samples != 1)
                {
                    break;
                }
            }
            auto page = read_page(tif.get(), width, height, bits_per_sample,
                                  pixels.data() + frame_pixels * loaded);
            if (!page)
            {
                return std::unexpected(page.error());
            }
            ++loaded;
        }
        if (loaded == 0)
        {
            return std::unexpected("TIFF 没有可解码的页面");
        }
        if (loaded < available_frames)
        {
            std::println("警告: 多页 TIFF 共 {} 页，受内存上限限制仅加载前 {} 页", available_frames,
                         loaded);
        }
        pixels.resize(frame_pixels * loaded);

        const ImageStats stats =
            compute_stats(std::span<const uint16_t>(pixels.data(), frame_pixels));
        DecodedImage image;
        image.name = file_name_of(path);
        image.meta = {
            .width = width,
            .height = height,
            .bits_per_sample = bits_per_sample,
            .samples_per_pixel = samples_per_pixel,
            .frame_count = loaded,
            .min_value = stats.min_value,
            .max_value = stats.max_value,
            .p1 = stats.p1,
            .p99 = stats.p99,
            .value_range = stats.value_range,
        };
        image.pixels = std::move(pixels);
        // PHOTOMETRIC_MINISWHITE 与 DICOM MONOCHROME1 一样需要初始反色。
        image.initial_invert = photometric == PHOTOMETRIC_MINISWHITE;
        return image;
    }
} // namespace voilut
