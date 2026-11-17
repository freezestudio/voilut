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
#include <span>
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

        /// @brief 整数倍盒式降采样（预览用）。
        std::vector<uint16_t> downsample_u16(std::span<const uint16_t> src, uint32_t sw,
                                             uint32_t sh, uint32_t factor, uint32_t &dw,
                                             uint32_t &dh)
        {
            dw = std::max(1u, sw / factor);
            dh = std::max(1u, sh / factor);
            std::vector<uint16_t> dst(static_cast<size_t>(dw) * dh);
            std::vector<uint64_t> sums(dw);
            for (uint32_t oy = 0; oy < dh; ++oy)
            {
                std::fill(sums.begin(), sums.end(), 0);
                const uint32_t y0 = oy * factor;
                const uint32_t y1 = std::min(y0 + factor, sh);
                for (uint32_t y = y0; y < y1; ++y)
                {
                    const uint16_t *row = src.data() + static_cast<size_t>(y) * sw;
                    for (uint32_t ox = 0; ox < dw; ++ox)
                    {
                        const uint32_t x0 = ox * factor;
                        const uint32_t x1 = std::min(x0 + factor, sw);
                        uint64_t sum = 0;
                        for (uint32_t x = x0; x < x1; ++x)
                        {
                            sum += row[x];
                        }
                        sums[ox] += sum;
                    }
                }
                for (uint32_t ox = 0; ox < dw; ++ox)
                {
                    const uint32_t x0 = ox * factor;
                    const uint32_t x1 = std::min(x0 + factor, sw);
                    const uint64_t count = static_cast<uint64_t>(y1 - y0) * (x1 - x0);
                    dst[static_cast<size_t>(oy) * dw + ox] =
                        static_cast<uint16_t>((sums[ox] + count / 2) / count);
                }
            }
            return dst;
        }

        /// @brief 浮点版本的整数倍盒式降采样。
        std::vector<float> downsample_f32(std::span<const float> src, uint32_t sw, uint32_t sh,
                                          uint32_t factor, uint32_t &dw, uint32_t &dh)
        {
            dw = std::max(1u, sw / factor);
            dh = std::max(1u, sh / factor);
            std::vector<float> dst(static_cast<size_t>(dw) * dh);
            std::vector<double> sums(dw);
            for (uint32_t oy = 0; oy < dh; ++oy)
            {
                std::fill(sums.begin(), sums.end(), 0.0);
                const uint32_t y0 = oy * factor;
                const uint32_t y1 = std::min(y0 + factor, sh);
                for (uint32_t y = y0; y < y1; ++y)
                {
                    const float *row = src.data() + static_cast<size_t>(y) * sw;
                    for (uint32_t ox = 0; ox < dw; ++ox)
                    {
                        const uint32_t x0 = ox * factor;
                        const uint32_t x1 = std::min(x0 + factor, sw);
                        double sum = 0.0;
                        for (uint32_t x = x0; x < x1; ++x)
                        {
                            sum += row[x];
                        }
                        sums[ox] += sum;
                    }
                }
                for (uint32_t ox = 0; ox < dw; ++ox)
                {
                    const uint32_t x0 = ox * factor;
                    const uint32_t x1 = std::min(x0 + factor, sw);
                    const double count = static_cast<double>(y1 - y0) * (x1 - x0);
                    dst[static_cast<size_t>(oy) * dw + ox] =
                        static_cast<float>(sums[ox] / count);
                }
            }
            return dst;
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
            // Phase 0：用现成的全量解码生成首帧预览；Phase 1 改为流式降采样。
            auto full = decode_tiff(path_);
            if (!full)
            {
                return std::unexpected(full.error());
            }
            const auto &source = *full;
            const uint32_t source_width = source.meta.width;
            const uint32_t source_height = source.meta.height;
            max_dimension = std::max(1u, max_dimension);
            const uint32_t factor =
                std::max(1u, (std::max(source_width, source_height) + max_dimension - 1) /
                                 max_dimension);
            uint32_t width = 1;
            uint32_t height = 1;

            DecodedImage preview;
            preview.name = source.name;
            preview.pixel_type = source.pixel_type;
            preview.initial_invert = source.initial_invert;
            preview.scale = info_.scale;
            preview.padding = info_.padding;
            preview.modality_unit = info_.modality_unit;

            if (source.pixel_type == DecodedImage::PixelType::Float32)
            {
                std::vector<float> pixels = downsample_f32(
                    source.frame_floats(0), source_width, source_height, factor, width, height);
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
                std::vector<uint16_t> pixels = downsample_u16(
                    source.frame_pixels(0), source_width, source_height, factor, width, height);
                const ImageStats stats = compute_stats(pixels);
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
            return preview;
        }
    } // namespace

    std::expected<std::unique_ptr<RegionSource>, std::string>
    open_region_source(const std::string &path)
    {
        const std::string suffix = lower_suffix(path);
        if (suffix == "dcm" || suffix == "dicom")
        {
            return std::unexpected(
                std::string("DICOM 区域读取将在后续阶段支持（当前请使用普通解码路径）"));
        }
        if (suffix != "tif" && suffix != "tiff")
        {
            return std::unexpected(std::format("不支持的图像格式：{}", path));
        }
        auto info = read_tiff_info(path);
        if (!info)
        {
            return std::unexpected(info.error());
        }
        return std::make_unique<TiffRegionSource>(path, std::move(info.value()));
    }
} // namespace voilut
