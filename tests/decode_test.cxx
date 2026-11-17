/// @file decode_test.cxx
/// @brief TIFF 解码回归测试（fixtures 由 libtiff 在临时目录现场生成，不入库）。
/// @details 覆盖：stripped / tiled、小端 / 大端、8 位 / 16 位、MINISWHITE、多页计数、
///          浮点 TIFF、多页逐帧校验（受 kMaxDecodeBytes 限制）。

#include "decode/tiff_decode.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
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

    /// @brief 路径转 UTF-8（与 `voilut::path_display` 一致，避免 `path::string()` 在 GCC 17 的弃用警告）。
    std::string to_utf8(const std::filesystem::path &path)
    {
        const std::u8string utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    /// @brief 打开写模式：`little_endian` 决定 `wl` / `wb`。
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

    /// @brief 写 stripped 的 16 位页面。
    bool write_scanline_16(TIFF *tif, uint32_t width, uint32_t height,
                           const std::vector<uint16_t> &pixels)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            if (TIFFWriteScanline(tif, const_cast<uint16_t *>(pixels.data() + y * width), y, 0) < 0)
            {
                return false;
            }
        }
        return true;
    }

    /// @brief 写 stripped 的 8 位页面。
    bool write_scanline_8(TIFF *tif, uint32_t width, uint32_t height,
                          const std::vector<uint16_t> &pixels)
    {
        std::vector<uint8_t> row(width);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                row[x] = static_cast<uint8_t>(pixels[y * width + x]);
            }
            if (TIFFWriteScanline(tif, row.data(), y, 0) < 0)
            {
                return false;
            }
        }
        return true;
    }

    /// @brief 写 tiled 的 16 位页面。
    bool write_tiled_16(TIFF *tif, uint32_t width, uint32_t height, uint32_t tile,
                        const std::vector<uint16_t> &pixels)
    {
        TIFFSetField(tif, TIFFTAG_TILEWIDTH, tile);
        TIFFSetField(tif, TIFFTAG_TILELENGTH, tile);
        std::vector<uint16_t> buffer(static_cast<size_t>(tile) * tile);
        for (uint32_t y0 = 0; y0 < height; y0 += tile)
        {
            for (uint32_t x0 = 0; x0 < width; x0 += tile)
            {
                for (uint32_t ty = 0; ty < tile; ++ty)
                {
                    for (uint32_t tx = 0; tx < tile; ++tx)
                    {
                        const uint32_t x = std::min(x0 + tx, width - 1);
                        const uint32_t y = std::min(y0 + ty, height - 1);
                        buffer[static_cast<size_t>(ty) * tile + tx] = pixels[y * width + x];
                    }
                }
                if (TIFFWriteTile(tif, buffer.data(), x0, y0, 0, 0) < 0)
                {
                    return false;
                }
            }
        }
        return true;
    }

    std::vector<uint16_t> make_pattern(uint32_t width, uint32_t height, uint16_t base)
    {
        std::vector<uint16_t> pixels(static_cast<size_t>(width) * height);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                pixels[static_cast<size_t>(y) * width + x] =
                    static_cast<uint16_t>(base + x * 7 + y * 131);
            }
        }
        return pixels;
    }

    bool same_pixels(const std::vector<uint16_t> &actual, const std::vector<uint16_t> &expected)
    {
        return actual.size() == expected.size() &&
               std::equal(actual.begin(), actual.end(), expected.begin());
    }

    /// @brief 解析一个 fixture 并检查基本元数据。
    std::optional<voilut::DecodedImage> decode(const std::filesystem::path &path)
    {
        auto image = voilut::decode_tiff(to_utf8(path));
        if (!image)
        {
            std::printf("    解码失败：%s\n", image.error().c_str());
            return std::nullopt;
        }
        return std::move(image.value());
    }
} // namespace

int main()
{
    g_dir = std::filesystem::temp_directory_path() / "voilut_decode_test";
    std::error_code ec;
    std::filesystem::remove_all(g_dir, ec);
    std::filesystem::create_directories(g_dir, ec);

    // ---------------- 1) stripped / 小端 / 16 位 ----------------
    {
        std::printf("1) stripped 小端 16 位\n");
        const uint32_t w = 5;
        const uint32_t h = 3;
        const auto pixels = make_pattern(w, h, 1000);
        const auto path = g_dir / "scanline_le16.tif";
        TIFF *tif = open_writer(path, true);
        check(tif != nullptr, "创建 fixture");
        if (tif)
        {
            set_common_fields(tif, w, h, 16, PHOTOMETRIC_MINISBLACK);
            check(write_scanline_16(tif, w, h, pixels), "写入扫描行");
            TIFFClose(tif);
        }
        if (auto image = decode(path))
        {
            check(image->meta.width == w && image->meta.height == h, "尺寸正确");
            check(image->meta.frame_count == 1, "单页 frame_count = 1");
            check(same_pixels(image->pixels, pixels), "像素值一致");
            check(!image->initial_invert, "MINISBLACK 不反色");
            check(image->meta.min_value == 1000.0, "min 正确");
        }
    }

    // ---------------- 2) tiled / 16 位（旧实现直接失败） ----------------
    {
        std::printf("2) tiled 16 位\n");
        const uint32_t w = 130;
        const uint32_t h = 70;
        const auto pixels = make_pattern(w, h, 500);
        const auto path = g_dir / "tiled16.tif";
        TIFF *tif = open_writer(path, true);
        check(tif != nullptr, "创建 fixture");
        if (tif)
        {
            set_common_fields(tif, w, h, 16, PHOTOMETRIC_MINISBLACK);
            check(write_tiled_16(tif, w, h, 64, pixels), "写入 tile");
            TIFFClose(tif);
        }
        if (auto image = decode(path))
        {
            check(image->meta.width == w && image->meta.height == h, "尺寸正确");
            check(same_pixels(image->pixels, pixels), "tile 拼接后的像素一致");
        }
    }

    // ---------------- 3) 大端 16 位（需要字节交换） ----------------
    {
        std::printf("3) 大端 16 位\n");
        const uint32_t w = 4;
        const uint32_t h = 2;
        const std::vector<uint16_t> pixels = {0x0102, 0x0304, 0x0506, 0x0708, 0x1122, 0x3344,
                                              0x5566, 0x7788};
        const auto path = g_dir / "big_endian16.tif";
        TIFF *tif = open_writer(path, false);
        check(tif != nullptr, "创建大端 fixture");
        if (tif)
        {
            set_common_fields(tif, w, h, 16, PHOTOMETRIC_MINISBLACK);
            check(write_scanline_16(tif, w, h, pixels), "写入扫描行");
            TIFFClose(tif);
        }
        if (auto image = decode(path))
        {
            check(same_pixels(image->pixels, pixels), "字节交换后像素值一致");
        }
    }

    // ---------------- 4) 8 位 MINISWHITE ----------------
    {
        std::printf("4) 8 位 MINISWHITE\n");
        const uint32_t w = 3;
        const uint32_t h = 2;
        const std::vector<uint16_t> pixels = {0, 128, 255, 10, 20, 30};
        const auto path = g_dir / "miniswhite8.tif";
        TIFF *tif = open_writer(path, true);
        check(tif != nullptr, "创建 fixture");
        if (tif)
        {
            set_common_fields(tif, w, h, 8, PHOTOMETRIC_MINISWHITE);
            check(write_scanline_8(tif, w, h, pixels), "写入扫描行");
            TIFFClose(tif);
        }
        if (auto image = decode(path))
        {
            check(same_pixels(image->pixels, pixels), "8 位像素值一致");
            check(image->initial_invert, "MINISWHITE 初始反色");
        }
    }

    // ---------------- 5) 多页 TIFF：frame_count 正确、首帧可解码 ----------------
    {
        std::printf("5) 多页 TIFF\n");
        const uint32_t w = 8;
        const uint32_t h = 6;
        const auto page0 = make_pattern(w, h, 100);
        const auto page1 = make_pattern(w, h, 30000);
        const auto path = g_dir / "multipage.tif";
        TIFF *tif = open_writer(path, true);
        check(tif != nullptr, "创建 fixture");
        if (tif)
        {
            set_common_fields(tif, w, h, 16, PHOTOMETRIC_MINISBLACK);
            check(write_scanline_16(tif, w, h, page0), "写第 1 页");
            check(TIFFWriteDirectory(tif) == 1, "切到第 2 页");
            set_common_fields(tif, w, h, 16, PHOTOMETRIC_MINISBLACK);
            check(write_scanline_16(tif, w, h, page1), "写第 2 页");
            TIFFClose(tif);
        }
        if (auto image = decode(path))
        {
            check(image->meta.frame_count == 2, "frame_count = 2");
            const auto first = image->frame_pixels(0);
            const auto second = image->frame_pixels(1);
            check(same_pixels(std::vector<uint16_t>(first.begin(), first.end()), page0),
                  "第 1 帧内容正确");
            check(same_pixels(std::vector<uint16_t>(second.begin(), second.end()), page1),
                  "第 2 帧内容正确");
            check(image->frame_pixels(2).empty(), "越界帧返回空 span");
        }
    }

    // ---------------- 6) 浮点 TIFF（32 位 float） ----------------
    {
        std::printf("6) 浮点 TIFF\n");
        const uint32_t w = 4;
        const uint32_t h = 2;
        std::vector<float> values(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < values.size(); ++i)
        {
            values[i] = 0.5f + static_cast<float>(i) * 0.25f;
        }
        const auto path = g_dir / "float32.tif";
        TIFF *tif = open_writer(path, true);
        check(tif != nullptr, "创建 fixture");
        if (tif)
        {
            set_common_fields(tif, w, h, 32, PHOTOMETRIC_MINISBLACK, SAMPLEFORMAT_IEEEFP);
            for (uint32_t y = 0; y < h; ++y)
            {
                TIFFWriteScanline(tif, values.data() + static_cast<size_t>(y) * w, y, 0);
            }
            TIFFClose(tif);
        }
        if (auto image = decode(path))
        {
            check(image->pixel_type == voilut::DecodedImage::PixelType::Float32,
                  "pixel_type = Float32");
            check(image->meta.is_float, "meta.is_float");
            check(std::abs(image->meta.min_value - 0.5) < 1e-6, "min = 0.5");
            check(std::abs(image->meta.max_value - 2.25) < 1e-6, "max = 2.25");
            const auto floats = image->frame_floats(0);
            check(floats.size() == values.size(), "float 帧长度正确");
            check(std::equal(floats.begin(), floats.end(), values.begin()), "浮点像素值一致");
        }
    }

    std::filesystem::remove_all(g_dir, ec);
    std::printf("\n%s（失败 %d 项）\n", g_failures == 0 ? "全部通过" : "存在失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
