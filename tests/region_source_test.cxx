/// @file region_source_test.cxx
/// @brief 区域读取（虚拟切片 Phase 0）回归测试。
/// @details fixtures 由 libtiff 现场生成：stripped / tiled / 大端 / 浮点 / 多页 / 8 位。

#include "region_source.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <tiffio.h>

namespace
{
    int g_failures = 0;
    std::filesystem::path g_dir;

    void check(bool ok, const char *what)
    {
        std::printf("  [%s] %s\n", ok ? "OK" : "FAIL", what);
        if (!ok)
        {
            ++g_failures;
        }
    }

    std::string to_utf8(const std::filesystem::path &path)
    {
        const std::u8string utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    TIFF *open_writer(const std::filesystem::path &path, bool little_endian)
    {
#if defined(_WIN32)
        return TIFFOpenW(path.c_str(), little_endian ? "wl" : "wb");
#else
        return TIFFOpen(to_utf8(path).c_str(), little_endian ? "wl" : "wb");
#endif
    }

    void set_common_fields(TIFF *tif, uint32_t width, uint32_t height, uint16_t bits,
                           uint16_t photometric, uint16_t sample_format = SAMPLEFORMAT_UINT)
    {
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
        TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, sample_format);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, photometric);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    }

    bool write_u16(TIFF *tif, uint32_t width, uint32_t height,
                   const std::function<uint16_t(uint32_t, uint32_t)> &value)
    {
        std::vector<uint16_t> row(width);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                row[x] = value(x, y);
            }
            if (TIFFWriteScanline(tif, row.data(), y, 0) < 0)
            {
                return false;
            }
        }
        return true;
    }

    bool write_f32(TIFF *tif, uint32_t width, uint32_t height,
                   const std::function<float(uint32_t, uint32_t)> &value)
    {
        std::vector<float> row(width);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                row[x] = value(x, y);
            }
            if (TIFFWriteScanline(tif, row.data(), y, 0) < 0)
            {
                return false;
            }
        }
        return true;
    }

    bool write_tiled_u16(TIFF *tif, uint32_t width, uint32_t height, uint32_t tile,
                         const std::function<uint16_t(uint32_t, uint32_t)> &value)
    {
        TIFFSetField(tif, TIFFTAG_TILEWIDTH, tile);
        TIFFSetField(tif, TIFFTAG_TILELENGTH, tile);
        std::vector<uint16_t> buffer(static_cast<size_t>(tile) * tile);
        for (uint32_t ty = 0; ty < height; ty += tile)
        {
            for (uint32_t tx = 0; tx < width; tx += tile)
            {
                for (uint32_t y = 0; y < tile; ++y)
                {
                    for (uint32_t x = 0; x < tile; ++x)
                    {
                        buffer[static_cast<size_t>(y) * tile + x] =
                            value(std::min(tx + x, width - 1), std::min(ty + y, height - 1));
                    }
                }
                if (TIFFWriteTile(tif, buffer.data(), tx, ty, 0, 0) < 0)
                {
                    return false;
                }
            }
        }
        return true;
    }

    /// @brief 读取区域并与期望值逐像素比较（自动处理边界裁剪）。
    void check_u16_region(voilut::RegionSource &source, uint32_t frame, uint32_t x, uint32_t y,
                          uint32_t width, uint32_t height,
                          const std::function<uint16_t(uint32_t, uint32_t)> &value)
    {
        auto region = source.read_region(x, y, width, height, frame);
        check(region.has_value(), "区域读取成功");
        if (!region)
        {
            std::printf("    错误: %s\n", region.error().c_str());
            return;
        }
        const auto &info = source.info();
        const uint32_t expect_width = std::min(width, info.width - x);
        const uint32_t expect_height = std::min(height, info.height - y);
        check(region->width == expect_width && region->height == expect_height, "区域尺寸正确");
        bool same = region->u16.size() == static_cast<size_t>(expect_width) * expect_height;
        for (uint32_t ry = 0; same && ry < expect_height; ++ry)
        {
            for (uint32_t rx = 0; rx < expect_width; ++rx)
            {
                const uint16_t actual =
                    region->u16[static_cast<size_t>(ry) * expect_width + rx];
                const uint16_t expected = value(x + rx, y + ry);
                if (actual != expected)
                {
                    std::printf("    期望 %u 实际 %u（x=%u y=%u）\n", expected, actual,
                                x + rx, y + ry);
                    same = false;
                    break;
                }
            }
        }
        check(same, "区域像素与期望一致");

        if (region->x != x || region->y != y)
        {
            check(false, "区域原点正确");
        }
    }

    void check_f32_region(voilut::RegionSource &source, uint32_t frame, uint32_t x, uint32_t y,
                          uint32_t width, uint32_t height,
                          const std::function<float(uint32_t, uint32_t)> &value)
    {
        auto region = source.read_region(x, y, width, height, frame);
        check(region.has_value(), "浮点区域读取成功");
        if (!region)
        {
            return;
        }
        const auto &info = source.info();
        const uint32_t expect_width = std::min(width, info.width - x);
        const uint32_t expect_height = std::min(height, info.height - y);
        bool same = region->f32.size() == static_cast<size_t>(expect_width) * expect_height;
        for (uint32_t ry = 0; same && ry < expect_height; ++ry)
        {
            for (uint32_t rx = 0; rx < expect_width; ++rx)
            {
                const float actual = region->f32[static_cast<size_t>(ry) * expect_width + rx];
                const float expected = value(x + rx, y + ry);
                if (std::abs(actual - expected) > 1e-6f)
                {
                    same = false;
                    break;
                }
            }
        }
        check(same, "浮点区域像素与期望一致");
    }

    void check_preview(voilut::RegionSource &source, uint32_t max_dimension)
    {
        auto preview = source.preview(max_dimension);
        check(preview.has_value(), "预览生成成功");
        if (!preview)
        {
            std::printf("    错误: %s\n", preview.error().c_str());
            return;
        }
        const uint32_t texture_width = preview->texture_width();
        const uint32_t texture_height = preview->texture_height();
        check(texture_width <= max_dimension && texture_height <= max_dimension,
              "预览纹理尺寸不超过上限");
        check(texture_width > 0 && texture_height > 0, "预览尺寸非零");
        const size_t expected = static_cast<size_t>(texture_width) * texture_height;
        const size_t actual = preview->pixel_type == voilut::DecodedImage::PixelType::Float32
                                  ? preview->float_pixels.size()
                                  : preview->pixels.size();
        check(actual == expected, "预览像素数量正确");
    }
} // namespace

int main()
{
    g_dir = std::filesystem::temp_directory_path() / "voilut_region_test";
    std::error_code ec;
    std::filesystem::remove_all(g_dir, ec);
    std::filesystem::create_directories(g_dir, ec);

    // ---------------- 1) stripped 16-bit ----------------
    {
        std::printf("1) stripped 16 位区域读取\n");
        const uint32_t w = 37;
        const uint32_t h = 23;
        const auto value = [](uint32_t x, uint32_t y) {
            return static_cast<uint16_t>(100 + x * 3 + y * 7);
        };
        const auto path = g_dir / "stripped16.tif";
        TIFF *tif = open_writer(path, true);
        check(tif != nullptr, "创建 fixture");
        if (tif)
        {
            set_common_fields(tif, w, h, 16, PHOTOMETRIC_MINISBLACK);
            check(write_u16(tif, w, h, value), "写入扫描行");
            TIFFClose(tif);
        }
        auto source = voilut::open_region_source(to_utf8(path));
        check(source.has_value(), "打开区域源");
        if (source)
        {
            check((*source)->info().width == w && (*source)->info().height == h, "元数据尺寸");
            check((*source)->info().sample_type == voilut::SampleType::UInt16, "像素类型");
            check_u16_region(**source, 0, 0, 0, w, h, value);
            check_u16_region(**source, 0, 5, 3, 17, 9, value);
            check_u16_region(**source, 0, w - 3, h - 2, 10, 10, value);
            check_preview(**source, 16);
        }
    }

    // ---------------- 2) tiled 16-bit ----------------
    {
        std::printf("2) tiled 16 位区域读取\n");
        const uint32_t w = 130;
        const uint32_t h = 70;
        const auto value = [](uint32_t x, uint32_t y) {
            return static_cast<uint16_t>(500 + x * 5 + y * 11);
        };
        const auto path = g_dir / "tiled16.tif";
        TIFF *tif = open_writer(path, true);
        check(tif != nullptr, "创建 fixture");
        if (tif)
        {
            set_common_fields(tif, w, h, 16, PHOTOMETRIC_MINISBLACK);
            check(write_tiled_u16(tif, w, h, 64, value), "写入 tile");
            TIFFClose(tif);
        }
        auto source = voilut::open_region_source(to_utf8(path));
        check(source.has_value(), "打开区域源");
        if (source)
        {
            check_u16_region(**source, 0, 0, 0, w, h, value);
            check_u16_region(**source, 0, 60, 20, 70, 50, value);
            check_u16_region(**source, 0, 127, 67, 10, 10, value);
            check_preview(**source, 32);
        }
    }

    // ---------------- 3) 大端 16-bit ----------------
    {
        std::printf("3) 大端 16 位区域读取\n");
        const uint32_t w = 16;
        const uint32_t h = 9;
        const auto value = [](uint32_t x, uint32_t y) {
            return static_cast<uint16_t>(0x1000 + x * 13 + y * 29);
        };
        const auto path = g_dir / "big_endian16.tif";
        TIFF *tif = open_writer(path, false);
        check(tif != nullptr, "创建 fixture");
        if (tif)
        {
            set_common_fields(tif, w, h, 16, PHOTOMETRIC_MINISBLACK);
            check(write_u16(tif, w, h, value), "写入扫描行");
            TIFFClose(tif);
        }
        auto source = voilut::open_region_source(to_utf8(path));
        check(source.has_value(), "打开区域源");
        if (source)
        {
            check_u16_region(**source, 0, 3, 2, 10, 5, value);
        }
    }

    // ---------------- 4) 浮点 32 位 ----------------
    {
        std::printf("4) 浮点区域读取\n");
        const uint32_t w = 20;
        const uint32_t h = 11;
        const auto value = [](uint32_t x, uint32_t y) {
            return 0.5f + static_cast<float>(x) * 0.25f + static_cast<float>(y) * 1.5f;
        };
        const auto path = g_dir / "float32.tif";
        TIFF *tif = open_writer(path, true);
        check(tif != nullptr, "创建 fixture");
        if (tif)
        {
            set_common_fields(tif, w, h, 32, PHOTOMETRIC_MINISBLACK, SAMPLEFORMAT_IEEEFP);
            check(write_f32(tif, w, h, value), "写入扫描行");
            TIFFClose(tif);
        }
        auto source = voilut::open_region_source(to_utf8(path));
        check(source.has_value(), "打开区域源");
        if (source)
        {
            check((*source)->info().sample_type == voilut::SampleType::Float32, "浮点类型");
            check_f32_region(**source, 0, 0, 0, w, h, value);
            check_f32_region(**source, 0, 7, 4, 8, 5, value);
            check_preview(**source, 8);
        }
    }

    // ---------------- 5) 多页 16-bit ----------------
    {
        std::printf("5) 多页区域读取\n");
        const uint32_t w = 12;
        const uint32_t h = 7;
        const auto value0 = [](uint32_t x, uint32_t y) {
            return static_cast<uint16_t>(1000 + x * 5 + y * 11);
        };
        const auto value1 = [](uint32_t x, uint32_t y) {
            return static_cast<uint16_t>(30000 + x * 5 + y * 11);
        };
        const auto path = g_dir / "multipage.tif";
        TIFF *tif = open_writer(path, true);
        check(tif != nullptr, "创建 fixture");
        if (tif)
        {
            set_common_fields(tif, w, h, 16, PHOTOMETRIC_MINISBLACK);
            check(write_u16(tif, w, h, value0), "写第 1 页");
            check(TIFFWriteDirectory(tif) == 1, "切到第 2 页");
            set_common_fields(tif, w, h, 16, PHOTOMETRIC_MINISBLACK);
            check(write_u16(tif, w, h, value1), "写第 2 页");
            TIFFClose(tif);
        }
        auto source = voilut::open_region_source(to_utf8(path));
        check(source.has_value(), "打开区域源");
        if (source)
        {
            check((*source)->info().frame_count == 2, "frame_count = 2");
            check_u16_region(**source, 0, 2, 1, 8, 5, value0);
            check_u16_region(**source, 1, 2, 1, 8, 5, value1);
        }
    }

    // ---------------- 6) 8 位 MINISWHITE ----------------
    {
        std::printf("6) 8 位 MINISWHITE\n");
        const uint32_t w = 10;
        const uint32_t h = 7;
        const auto value = [](uint32_t x, uint32_t y) {
            return static_cast<uint8_t>(10 + x + y);
        };
        const auto path = g_dir / "miniswhite8.tif";
        TIFF *tif = open_writer(path, true);
        check(tif != nullptr, "创建 fixture");
        if (tif)
        {
            set_common_fields(tif, w, h, 8, PHOTOMETRIC_MINISWHITE);
            const bool ok = [&] {
                std::vector<uint8_t> row(w);
                for (uint32_t y = 0; y < h; ++y)
                {
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        row[x] = value(x, y);
                    }
                    if (TIFFWriteScanline(tif, row.data(), y, 0) < 0)
                    {
                        return false;
                    }
                }
                return true;
            }();
            check(ok, "写入扫描行");
            TIFFClose(tif);
        }
        auto source = voilut::open_region_source(to_utf8(path));
        check(source.has_value(), "打开区域源");
        if (source)
        {
            check((*source)->info().initial_invert, "MINISWHITE 标记");
            check_u16_region(**source, 0, 1, 1, 6, 4,
                             [&](uint32_t x, uint32_t y) {
                                 return static_cast<uint16_t>(value(x, y));
                             });
        }
    }

    // ---------------- 7) 大端 tiled ----------------
    {
        std::printf("7) 大端 tiled 区域读取\n");
        const uint32_t w = 40;
        const uint32_t h = 30;
        const auto value = [](uint32_t x, uint32_t y) {
            return static_cast<uint16_t>(0x2000 + x * 17 + y * 31);
        };
        const auto path = g_dir / "big_endian_tiled.tif";
        TIFF *tif = open_writer(path, false);
        check(tif != nullptr, "创建 fixture");
        if (tif)
        {
            set_common_fields(tif, w, h, 16, PHOTOMETRIC_MINISBLACK);
            check(write_tiled_u16(tif, w, h, 16, value), "写入 tile");
            TIFFClose(tif);
        }
        auto source = voilut::open_region_source(to_utf8(path));
        check(source.has_value(), "打开区域源");
        if (source)
        {
            check_u16_region(**source, 0, 5, 4, 20, 18, value);
        }
    }

    std::filesystem::remove_all(g_dir, ec);
    std::printf("\n%s（失败 %d 项）\n", g_failures == 0 ? "全部通过" : "存在失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
