/// @file tiff_region_source.cxx
/// @brief TIFF 区域读取实现（Phase 0：stripped / tiled / 多页 / 大端 / 浮点）。

#include "region_source.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <tiffio.h>

#include "decode/tiff_decode.h" // preview 的全量回退

namespace voilut
{
    namespace
    {
        using TiffHandle = std::unique_ptr<TIFF, decltype(&TIFFClose)>;

        /// @brief 文件名后缀（小写，不含点）。
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

        /// @brief 打开 TIFF（UTF-8 路径；Windows 走 TIFFOpenW）。
        TIFF *open_tiff(const std::string &path)
        {
#if defined(_WIN32)
            const std::filesystem::path wide_path(std::u8string(path.begin(), path.end()));
            return TIFFOpenW(wide_path.c_str(), "r");
#else
            return TIFFOpen(path.c_str(), "r");
#endif
        }

        std::string file_name_of(const std::string &path)
        {
            const size_t slash = path.find_last_of("/\\");
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        /// @brief 把一行源字节拷贝到目标（处理 8 位扩展；libtiff 已完成字节交换）。
        template <typename T>
        void copy_row(T *dst, const uint8_t *src, uint32_t count, uint16_t bits)
        {
            if (bits == 8)
            {
                for (uint32_t i = 0; i < count; ++i)
                {
                    dst[i] = static_cast<T>(src[i]);
                }
                return;
            }
            std::memcpy(dst, src, static_cast<size_t>(count) * sizeof(T));
        }

        /// @brief 读取 TIFF 元数据（不加载像素）。
        std::expected<RegionInfo, std::string> read_tiff_info(const std::string &path)
        {
            TiffHandle tif(open_tiff(path), &TIFFClose);
            if (!tif)
            {
                return std::unexpected(std::format("无法打开 TIFF 文件：{}", path));
            }

            uint32_t width = 0;
            uint32_t height = 0;
            if (TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &width) != 1 ||
                TIFFGetField(tif.get(), TIFFTAG_IMAGELENGTH, &height) != 1 || width == 0 ||
                height == 0)
            {
                return std::unexpected("TIFF 文件不包含有效的图像尺寸");
            }

            uint16_t bits = 0;
            uint16_t samples = 1;
            uint16_t sample_format = SAMPLEFORMAT_UINT;
            uint16_t photometric = PHOTOMETRIC_MINISBLACK;
            TIFFGetFieldDefaulted(tif.get(), TIFFTAG_BITSPERSAMPLE, &bits);
            TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &samples);
            TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLEFORMAT, &sample_format);
            TIFFGetFieldDefaulted(tif.get(), TIFFTAG_PHOTOMETRIC, &photometric);

            if (samples != 1)
            {
                return std::unexpected(
                    std::format("仅支持单通道 TIFF，当前为 {} 通道", samples));
            }
            if (sample_format == SAMPLEFORMAT_INT || sample_format == SAMPLEFORMAT_COMPLEXINT ||
                sample_format == SAMPLEFORMAT_COMPLEXIEEEFP)
            {
                return std::unexpected("暂不支持有符号 / 复数像素的 TIFF");
            }

            RegionInfo info;
            info.width = width;
            info.height = height;
            info.bits_per_sample = bits;
            info.name = file_name_of(path);
            info.initial_invert = photometric == PHOTOMETRIC_MINISWHITE;
            info.scale.is_float = sample_format == SAMPLEFORMAT_IEEEFP;
            info.sample_type = info.scale.is_float ? SampleType::Float32 : SampleType::UInt16;

            if (sample_format == SAMPLEFORMAT_IEEEFP)
            {
                if (bits != 32)
                {
                    return std::unexpected(
                        std::format("浮点 TIFF 仅支持 32 位 float，当前为 {} 位", bits));
                }
            }
            else if (bits != 8 && bits != 16)
            {
                return std::unexpected(
                    std::format("仅支持 8 位或 16 位整数 TIFF，当前为 {} 位", bits));
            }

            const tdir_t directories = TIFFNumberOfDirectories(tif.get());
            info.frame_count = directories > 0 ? static_cast<uint32_t>(directories) : 1;
            return info;
        }

        /// @brief TIFF 区域读取实现。
        class TiffRegionSource final : public RegionSource
        {
        public:
            TiffRegionSource(std::string path, RegionInfo info)
                : path_(std::move(path)), info_(std::move(info))
            {
            }

            [[nodiscard]] const RegionInfo &info() const noexcept override { return info_; }

            [[nodiscard]] std::expected<DecodedImage, std::string>
            preview(uint32_t max_dimension) override;

            [[nodiscard]] std::expected<PixelRegion, std::string>
            read_region(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                        uint32_t frame) override;

        private:
            std::string path_;
            RegionInfo info_;
        };

        std::expected<PixelRegion, std::string>
        TiffRegionSource::read_region(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                      uint32_t frame)
        {
            if (frame >= info_.frame_count)
            {
                return std::unexpected(
                    std::format("帧序号越界：{}（共 {} 帧）", frame, info_.frame_count));
            }
            if (x >= info_.width || y >= info_.height || width == 0 || height == 0)
            {
                return std::unexpected(std::string("区域超出图像范围"));
            }

            const uint32_t x0 = x;
            const uint32_t y0 = y;
            const uint32_t x1 = std::min(info_.width, x + width);
            const uint32_t y1 = std::min(info_.height, y + height);
            const uint32_t rw = x1 - x0;
            const uint32_t rh = y1 - y0;

            TiffHandle tif(open_tiff(path_), &TIFFClose);
            if (!tif)
            {
                return std::unexpected(std::format("无法打开 TIFF 文件：{}", path_));
            }
            if (TIFFSetDirectory(tif.get(), static_cast<tdir_t>(frame)) != 1)
            {
                return std::unexpected(std::format("无法定位 TIFF 第 {} 页", frame));
            }

            PixelRegion region;
            region.x = x0;
            region.y = y0;
            region.width = rw;
            region.height = rh;
            const size_t count = static_cast<size_t>(rw) * rh;
            try
            {
                if (info_.sample_type == SampleType::Float32)
                {
                    region.f32.resize(count);
                }
                else
                {
                    region.u16.resize(count);
                }
            }
            catch (const std::bad_alloc &)
            {
                return std::unexpected(std::string("内存不足：无法分配区域缓冲区"));
            }

            const bool is_float = info_.sample_type == SampleType::Float32;
            const size_t bytes_per_pixel = info_.bits_per_sample / 8;

            if (TIFFIsTiled(tif.get()))
            {
                uint32_t tile_width = 0;
                uint32_t tile_height = 0;
                if (TIFFGetField(tif.get(), TIFFTAG_TILEWIDTH, &tile_width) != 1 ||
                    TIFFGetField(tif.get(), TIFFTAG_TILELENGTH, &tile_height) != 1 ||
                    tile_width == 0 || tile_height == 0)
                {
                    return std::unexpected(std::string("TIFF 缺少有效的 tile 尺寸"));
                }
                const tmsize_t tile_size = TIFFTileSize(tif.get());
                if (tile_size <= 0)
                {
                    return std::unexpected(std::string("TIFF tile 缓冲区尺寸异常"));
                }
                std::vector<uint8_t> tile(static_cast<size_t>(tile_size));
                for (uint32_t ty = (y0 / tile_height) * tile_height; ty < y1;
                     ty += tile_height)
                {
                    for (uint32_t tx = (x0 / tile_width) * tile_width; tx < x1;
                         tx += tile_width)
                    {
                        if (TIFFReadTile(tif.get(), tile.data(), tx, ty, 0, 0) < 0)
                        {
                            return std::unexpected(
                                std::format("读取 TIFF tile ({}, {}) 失败", tx, ty));
                        }
                        const uint32_t rx0 = std::max(x0, tx);
                        const uint32_t rx1 = std::min(x1, tx + tile_width);
                        const uint32_t ry0 = std::max(y0, ty);
                        const uint32_t ry1 = std::min(y1, ty + tile_height);
                        for (uint32_t ry = ry0; ry < ry1; ++ry)
                        {
                            const uint8_t *src =
                                tile.data() +
                                (static_cast<size_t>(ry - ty) * tile_width + (rx0 - tx)) *
                                    bytes_per_pixel;
                            const size_t dst_index =
                                static_cast<size_t>(ry - y0) * rw + (rx0 - x0);
                            if (is_float)
                            {
                                copy_row(region.f32.data() + dst_index, src, rx1 - rx0,
                                         info_.bits_per_sample);
                            }
                            else
                            {
                                copy_row(region.u16.data() + dst_index, src, rx1 - rx0,
                                         info_.bits_per_sample);
                            }
                        }
                    }
                }
            }
            else
            {
                const tsize_t scanline_size = TIFFScanlineSize(tif.get());
                if (scanline_size < static_cast<tsize_t>(info_.width * bytes_per_pixel))
                {
                    return std::unexpected(std::string("TIFF 扫描行尺寸异常"));
                }
                std::vector<uint8_t> row_buffer(static_cast<size_t>(scanline_size));
                for (uint32_t ry = y0; ry < y1; ++ry)
                {
                    if (TIFFReadScanline(tif.get(), row_buffer.data(), ry) < 0)
                    {
                        return std::unexpected(std::format("读取 TIFF 第 {} 行失败", ry));
                    }
                    const uint8_t *src = row_buffer.data() + static_cast<size_t>(x0) * bytes_per_pixel;
                    const size_t dst_index = static_cast<size_t>(ry - y0) * rw;
                    if (is_float)
                    {
                        copy_row(region.f32.data() + dst_index, src, rw, info_.bits_per_sample);
                    }
                    else
                    {
                        copy_row(region.u16.data() + dst_index, src, rw, info_.bits_per_sample);
                    }
                }
            }
            return region;
        }

        std::expected<DecodedImage, std::string> TiffRegionSource::preview(uint32_t max_dimension)
        {
            max_dimension = std::max(1u, max_dimension);
            const uint32_t width = info_.width;
            const uint32_t height = info_.height;
            const uint32_t max_side = std::max(width, height);
            const bool is_float = info_.sample_type == SampleType::Float32;

            DecodedImage preview;
            preview.name = info_.name;
            preview.pixel_type = is_float ? DecodedImage::PixelType::Float32
                                          : DecodedImage::PixelType::UInt16;
            preview.initial_invert = info_.initial_invert;
            preview.scale = info_.scale;
            preview.padding = info_.padding;
            preview.modality_unit = info_.modality_unit;

            if (max_side <= max_dimension)
            {
                auto region = read_region(0, 0, width, height, 0);
                if (!region)
                {
                    return std::unexpected(region.error());
                }
                if (is_float)
                {
                    preview.float_pixels = std::move(region->f32);
                    const ImageStats stats = compute_stats_float(preview.float_pixels);
                    preview.meta = {
                        .width = width,
                        .height = height,
                        .bits_per_sample = info_.bits_per_sample,
                        .samples_per_pixel = 1,
                        .frame_count = 1,
                        .is_float = true,
                        .min_value = stats.min_value,
                        .max_value = stats.max_value,
                        .p1 = stats.p1,
                        .p99 = stats.p99,
                        .value_range = stats.value_range,
                    };
                }
                else
                {
                    preview.pixels = std::move(region->u16);
                    const ImageStats stats = compute_stats(preview.pixels);
                    preview.meta = {
                        .width = width,
                        .height = height,
                        .bits_per_sample = info_.bits_per_sample,
                        .samples_per_pixel = 1,
                        .frame_count = 1,
                        .is_float = false,
                        .min_value = stats.min_value,
                        .max_value = stats.max_value,
                        .p1 = stats.p1,
                        .p99 = stats.p99,
                        .value_range = stats.value_range,
                    };
                }
                return preview;
            }

            // 流式降采样：每输出像素取最多约 4×4 个源像素求平均，不做全图分配。
            const uint32_t scale = (max_side + max_dimension - 1) / max_dimension;
            const uint32_t out_width = std::max(1u, width / scale);
            const uint32_t out_height = std::max(1u, height / scale);
            const size_t out_count = static_cast<size_t>(out_width) * out_height;
            const uint32_t step = std::max(1u, scale / 2);
            preview.preview_width = out_width;
            preview.preview_height = out_height;

            TiffHandle tif(open_tiff(path_), &TIFFClose);
            if (!tif)
            {
                return std::unexpected(std::format("无法打开 TIFF 文件：{}", path_));
            }
            if (TIFFSetDirectory(tif.get(), 0) != 1)
            {
                return std::unexpected(std::string("无法定位 TIFF 第 0 页"));
            }

            const size_t bytes_per_pixel = info_.bits_per_sample / 8;
            std::vector<uint16_t> counts;
            std::vector<uint64_t> sums_u16;
            std::vector<double> sums_f32;
            // 全图抽样统计（仅整数）：直方图用于自动窗，避免只看预览像素。
            std::vector<uint32_t> source_histogram;
            uint32_t source_min = 65535;
            uint32_t source_max = 0;
            size_t source_count = 0;
            try
            {
                counts.assign(out_count, 0);
                if (is_float)
                {
                    sums_f32.assign(out_count, 0.0);
                }
                else
                {
                    sums_u16.assign(out_count, 0);
                    source_histogram.assign(65536, 0);
                }
            }
            catch (const std::bad_alloc &)
            {
                return std::unexpected(std::string("内存不足：无法分配预览累加缓冲区"));
            }

            auto add_sample = [&](uint32_t gx, uint32_t gy, uint32_t value_u16, float value_f32) {
                const uint32_t ox = gx / scale;
                const uint32_t oy = gy / scale;
                if (ox >= out_width || oy >= out_height)
                {
                    return;
                }
                const size_t index = static_cast<size_t>(oy) * out_width + ox;
                if (is_float)
                {
                    sums_f32[index] += value_f32;
                }
                else
                {
                    sums_u16[index] += value_u16;
                    if (!info_.padding.contains(value_u16))
                    {
                        ++source_histogram[value_u16];
                        source_min = std::min(source_min, value_u16);
                        source_max = std::max(source_max, value_u16);
                        ++source_count;
                    }
                }
                ++counts[index];
            };

            if (TIFFIsTiled(tif.get()))
            {
                uint32_t tile_width = 0;
                uint32_t tile_height = 0;
                if (TIFFGetField(tif.get(), TIFFTAG_TILEWIDTH, &tile_width) != 1 ||
                    TIFFGetField(tif.get(), TIFFTAG_TILELENGTH, &tile_height) != 1 ||
                    tile_width == 0 || tile_height == 0)
                {
                    return std::unexpected(std::string("TIFF 缺少有效的 tile 尺寸"));
                }
                const tmsize_t tile_size = TIFFTileSize(tif.get());
                if (tile_size <= 0)
                {
                    return std::unexpected(std::string("TIFF tile 缓冲区尺寸异常"));
                }
                std::vector<uint8_t> tile(static_cast<size_t>(tile_size));
                for (uint32_t ty = 0; ty < height; ty += tile_height)
                {
                    for (uint32_t tx = 0; tx < width; tx += tile_width)
                    {
                        if (TIFFReadTile(tif.get(), tile.data(), tx, ty, 0, 0) < 0)
                        {
                            return std::unexpected(
                                std::format("读取 TIFF tile ({}, {}) 失败", tx, ty));
                        }
                        const uint32_t gx0 = ((tx + step - 1) / step) * step;
                        const uint32_t gy0 = ((ty + step - 1) / step) * step;
                        const uint32_t gx1 = std::min(tx + tile_width, width);
                        const uint32_t gy1 = std::min(ty + tile_height, height);
                        for (uint32_t gy = gy0; gy < gy1; gy += step)
                        {
                            for (uint32_t gx = gx0; gx < gx1; gx += step)
                            {
                                const size_t local =
                                    (static_cast<size_t>(gy - ty) * tile_width + (gx - tx)) *
                                    bytes_per_pixel;
                                if (is_float)
                                {
                                    float value = 0.0f;
                                    std::memcpy(&value, tile.data() + local, sizeof(value));
                                    add_sample(gx, gy, 0, value);
                                }
                                else if (info_.bits_per_sample == 8)
                                {
                                    add_sample(gx, gy, tile[local], 0.0f);
                                }
                                else
                                {
                                    uint16_t value = 0;
                                    std::memcpy(&value, tile.data() + local, sizeof(value));
                                    add_sample(gx, gy, value, 0.0f);
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                const tsize_t scanline_size = TIFFScanlineSize(tif.get());
                if (scanline_size < static_cast<tsize_t>(width * bytes_per_pixel))
                {
                    return std::unexpected(std::string("TIFF 扫描行尺寸异常"));
                }
                std::vector<uint8_t> row_buffer(static_cast<size_t>(scanline_size));
                for (uint32_t gy = 0; gy < height; gy += step)
                {
                    if (TIFFReadScanline(tif.get(), row_buffer.data(), gy) < 0)
                    {
                        return std::unexpected(std::format("读取 TIFF 第 {} 行失败", gy));
                    }
                    for (uint32_t gx = 0; gx < width; gx += step)
                    {
                        const size_t offset = static_cast<size_t>(gx) * bytes_per_pixel;
                        if (is_float)
                        {
                            float value = 0.0f;
                            std::memcpy(&value, row_buffer.data() + offset, sizeof(value));
                            add_sample(gx, gy, 0, value);
                        }
                        else if (info_.bits_per_sample == 8)
                        {
                            add_sample(gx, gy, row_buffer[offset], 0.0f);
                        }
                        else
                        {
                            uint16_t value = 0;
                            std::memcpy(&value, row_buffer.data() + offset, sizeof(value));
                            add_sample(gx, gy, value, 0.0f);
                        }
                    }
                }
            }

            // 整数图像用全图抽样直方图计算 p1/p99；浮点仍用预览统计。
            ImageStats sampled_stats{};
            bool has_sampled_stats = false;
            if (!is_float && source_count > 0)
            {
                const size_t rank = std::max<size_t>(1, source_count / 100);
                uint32_t p1 = source_min;
                uint32_t p99 = source_max;
                bool found = false;
                uint64_t acc = 0;
                for (uint32_t value = 0; value < 65536; ++value)
                {
                    acc += source_histogram[value];
                    if (!found && acc >= rank)
                    {
                        p1 = value;
                        found = true;
                    }
                }
                acc = 0;
                for (int value = 65535; value >= 0; --value)
                {
                    acc += source_histogram[static_cast<size_t>(value)];
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
                sampled_stats = {
                    .min_value = static_cast<double>(source_min),
                    .max_value = static_cast<double>(source_max),
                    .p1 = static_cast<double>(p1),
                    .p99 = static_cast<double>(p99),
                    .value_range = static_cast<double>(source_max - source_min),
                };
                has_sampled_stats = true;
            }

            try
            {
                if (is_float)
                {
                    std::vector<float> pixels(out_count);
                    for (size_t i = 0; i < out_count; ++i)
                    {
                        pixels[i] = counts[i] == 0
                                        ? 0.0f
                                        : static_cast<float>(sums_f32[i] / counts[i]);
                    }
                    const ImageStats stats = compute_stats_float(pixels);
                    preview.meta = {
                        .width = width,
                        .height = height,
                        .bits_per_sample = info_.bits_per_sample,
                        .samples_per_pixel = 1,
                        .frame_count = 1,
                        .is_float = true,
                        .min_value = stats.min_value,
                        .max_value = stats.max_value,
                        .p1 = stats.p1,
                        .p99 = stats.p99,
                        .value_range = stats.value_range,
                    };
                    preview.float_pixels = std::move(pixels);
                }
                else
                {
                    std::vector<uint16_t> pixels(out_count);
                    for (size_t i = 0; i < out_count; ++i)
                    {
                        pixels[i] = counts[i] == 0
                                        ? 0
                                        : static_cast<uint16_t>((sums_u16[i] + counts[i] / 2) /
                                                                counts[i]);
                    }
                    const ImageStats stats =
                        has_sampled_stats ? sampled_stats : compute_stats(pixels);
                    preview.meta = {
                        .width = width,
                        .height = height,
                        .bits_per_sample = info_.bits_per_sample,
                        .samples_per_pixel = 1,
                        .frame_count = 1,
                        .is_float = false,
                        .min_value = stats.min_value,
                        .max_value = stats.max_value,
                        .p1 = stats.p1,
                        .p99 = stats.p99,
                        .value_range = stats.value_range,
                    };
                    preview.pixels = std::move(pixels);
                }
            }
            catch (const std::bad_alloc &)
            {
                return std::unexpected(std::string("内存不足：无法分配预览像素"));
            }
            return preview;
        }
    } // namespace

    std::expected<std::unique_ptr<RegionSource>, std::string>
    open_tiff_region_source(const std::string &path)
    {
        const std::string suffix = lower_suffix(path);
        if (suffix != "tif" && suffix != "tiff")
        {
            return std::unexpected(std::format("不是 TIFF 文件：{}", path));
        }
        auto info = read_tiff_info(path);
        if (!info)
        {
            return std::unexpected(info.error());
        }
        return std::make_unique<TiffRegionSource>(path, std::move(info.value()));
    }
} // namespace voilut
