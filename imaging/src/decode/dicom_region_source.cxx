/// @file dicom_region_source.cxx
/// @brief DICOM 区域读取（Phase 2：gdcm::ImageRegionReader）。
/// @details 元数据通过 `ReadInformation()` 读取（不加载 PixelData）；区域读取支持
///          未压缩与 GDCM 能按区域解码的压缩传输语法；预览暂用全量解码（受
///          `kMaxDecodeBytes` 限制）。

#include "region_source.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gdcmBoxRegion.h>
#include <gdcmByteValue.h>
#include <gdcmDataElement.h>
#include <gdcmDataSet.h>
#include <gdcmFile.h>
#include <gdcmImageRegionReader.h>

#include "decode/dicom_decode.h"
#include "decode/image_decode.h" // kMaxDecodeBytes

namespace voilut
{
    namespace
    {
        constexpr double kSignedOffset = 32768.0;

        std::string file_name_of(const std::string &path)
        {
            const size_t slash = path.find_last_of("/\\");
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        std::string read_string(const gdcm::DataSet &dataset, const gdcm::Tag &tag)
        {
            if (!dataset.FindDataElement(tag))
            {
                return {};
            }
            const gdcm::ByteValue *bytes = dataset.GetDataElement(tag).GetByteValue();
            if (bytes == nullptr || bytes->GetLength() == 0)
            {
                return {};
            }
            std::string text(bytes->GetPointer(), bytes->GetLength());
            while (!text.empty() &&
                   (text.back() == ' ' || text.back() == '\0' || text.back() == '\r'))
            {
                text.pop_back();
            }
            return text;
        }

        std::optional<unsigned int> read_us(const gdcm::DataSet &dataset, const gdcm::Tag &tag)
        {
            if (!dataset.FindDataElement(tag))
            {
                return std::nullopt;
            }
            const gdcm::ByteValue *bytes = dataset.GetDataElement(tag).GetByteValue();
            if (bytes == nullptr || bytes->GetLength() < 2)
            {
                return std::nullopt;
            }
            const auto *raw = reinterpret_cast<const unsigned char *>(bytes->GetPointer());
            return static_cast<unsigned int>(raw[0] | (static_cast<unsigned int>(raw[1]) << 8));
        }

        std::optional<int> read_ss(const gdcm::DataSet &dataset, const gdcm::Tag &tag)
        {
            if (!dataset.FindDataElement(tag))
            {
                return std::nullopt;
            }
            const gdcm::ByteValue *bytes = dataset.GetDataElement(tag).GetByteValue();
            if (bytes == nullptr || bytes->GetLength() < 2)
            {
                return std::nullopt;
            }
            int16_t value = 0;
            std::memcpy(&value, bytes->GetPointer(), sizeof(value));
            return static_cast<int>(value);
        }

        std::optional<double> read_ds(const gdcm::DataSet &dataset, const gdcm::Tag &tag)
        {
            const std::string text = read_string(dataset, tag);
            if (text.empty())
            {
                return std::nullopt;
            }
            char *end = nullptr;
            const double value = std::strtod(text.c_str(), &end);
            if (end == text.c_str())
            {
                return std::nullopt;
            }
            return value;
        }

        std::expected<RegionInfo, std::string> read_dicom_info(const std::string &path)
        {
            gdcm::ImageRegionReader reader;
            reader.SetFileName(path.c_str());
            if (!reader.ReadInformation())
            {
                return std::unexpected(std::format("无法读取 DICOM 信息：{}", path));
            }
            const gdcm::DataSet &dataset = reader.GetFile().GetDataSet();

            const auto columns = read_us(dataset, gdcm::Tag(0x0028, 0x0011));
            const auto rows = read_us(dataset, gdcm::Tag(0x0028, 0x0010));
            if (!columns || !rows || *columns == 0 || *rows == 0)
            {
                return std::unexpected("DICOM 缺少图像尺寸（Rows/Columns）");
            }
            const auto samples = read_us(dataset, gdcm::Tag(0x0028, 0x0002)).value_or(1);
            const auto bits_allocated = read_us(dataset, gdcm::Tag(0x0028, 0x0100));
            const auto bits_stored =
                read_us(dataset, gdcm::Tag(0x0028, 0x0101)).value_or(bits_allocated.value_or(0));
            const auto representation =
                read_us(dataset, gdcm::Tag(0x0028, 0x0103)).value_or(0);
            const std::string photometric = read_string(dataset, gdcm::Tag(0x0028, 0x0004));
            if (samples != 1)
            {
                return std::unexpected(
                    std::format("仅支持单通道灰度 DICOM，当前为 {} 通道", samples));
            }
            if (!bits_allocated || (*bits_allocated != 8 && *bits_allocated != 16))
            {
                return std::unexpected(
                    std::format("仅支持 8 位或 16 位灰度 DICOM，当前为 {} 位",
                                bits_allocated.value_or(0)));
            }
            if (photometric != "MONOCHROME1" && photometric != "MONOCHROME2")
            {
                return std::unexpected("仅支持 MONOCHROME 光度解释的 DICOM");
            }

            uint32_t frames = 1;
            const std::string frame_text = read_string(dataset, gdcm::Tag(0x0028, 0x0008));
            if (!frame_text.empty())
            {
                const long value = std::strtol(frame_text.c_str(), nullptr, 10);
                if (value > 1)
                {
                    frames = static_cast<uint32_t>(value);
                }
            }

            const double slope_raw = read_ds(dataset, gdcm::Tag(0x0028, 0x1053)).value_or(1.0);
            const double slope = slope_raw != 0.0 ? slope_raw : 1.0;
            const double intercept = read_ds(dataset, gdcm::Tag(0x0028, 0x1052)).value_or(0.0);
            const bool is_signed = representation != 0;
            const double offset = is_signed ? kSignedOffset : 0.0;

            RegionInfo info;
            info.width = *columns;
            info.height = *rows;
            info.frame_count = frames;
            info.bits_per_sample = *bits_allocated;
            info.name = file_name_of(path);
            info.initial_invert = photometric == "MONOCHROME1";
            info.scale = ValueScale{
                .slope = slope,
                .intercept = intercept,
                .storage_offset = offset,
                .is_float = false,
            };
            info.sample_type = SampleType::UInt16;
            info.modality_unit = read_string(dataset, gdcm::Tag(0x0028, 0x1054));
            if (info.modality_unit.empty() &&
                read_string(dataset, gdcm::Tag(0x0008, 0x0060)) == "CT")
            {
                info.modality_unit = "HU";
            }

            // PixelPaddingValue / RangeLimit → 存储域排除区间。
            std::optional<double> padding_value;
            if (is_signed)
            {
                if (const auto value = read_ss(dataset, gdcm::Tag(0x0028, 0x0120)))
                {
                    padding_value = static_cast<double>(*value) + offset;
                }
            }
            else if (const auto value = read_us(dataset, gdcm::Tag(0x0028, 0x0120)))
            {
                padding_value = static_cast<double>(*value) + offset;
            }
            if (padding_value)
            {
                double low = *padding_value;
                double high = *padding_value;
                if (is_signed)
                {
                    if (const auto limit = read_ss(dataset, gdcm::Tag(0x0028, 0x0121)))
                    {
                        const double value = static_cast<double>(*limit) + offset;
                        low = std::min(low, value);
                        high = std::max(high, value);
                    }
                }
                else if (const auto limit = read_us(dataset, gdcm::Tag(0x0028, 0x0121)))
                {
                    const double value = static_cast<double>(*limit) + offset;
                    low = std::min(low, value);
                    high = std::max(high, value);
                }
                info.padding = ValueExclusion{.enabled = true, .low = low, .high = high};
            }
            (void)bits_stored;
            return info;
        }

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

        class DicomRegionSource final : public RegionSource
        {
        public:
            DicomRegionSource(std::string path, RegionInfo info)
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
        DicomRegionSource::read_region(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
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

            gdcm::ImageRegionReader reader;
            reader.SetFileName(path_.c_str());
            if (!reader.ReadInformation())
            {
                return std::unexpected(std::string("无法读取 DICOM 信息（区域）"));
            }
            gdcm::BoxRegion region;
            region.SetDomain(x0, x1 - 1, y0, y1 - 1, frame, frame);
            reader.SetRegion(region);

            const size_t bytes_per_pixel = info_.bits_per_sample / 8;
            const size_t expected = static_cast<size_t>(rw) * rh * bytes_per_pixel;
            const size_t length = reader.ComputeBufferLength();
            if (length < expected)
            {
                return std::unexpected(
                    std::format("DICOM 区域缓冲区异常：需要 {} 字节，实际 {}", expected, length));
            }
            std::vector<char> buffer(length);
            if (!reader.ReadIntoBuffer(buffer.data(), length))
            {
                return std::unexpected(std::string("DICOM 区域读取失败（可能不支持该压缩语法）"));
            }

            PixelRegion result;
            result.x = x0;
            result.y = y0;
            result.width = rw;
            result.height = rh;
            const size_t count = static_cast<size_t>(rw) * rh;
            try
            {
                result.u16.resize(count);
            }
            catch (const std::bad_alloc &)
            {
                return std::unexpected(std::string("内存不足：无法分配 DICOM 区域缓冲区"));
            }

            const bool is_signed = info_.scale.storage_offset != 0.0;
            if (info_.bits_per_sample == 8)
            {
                for (size_t i = 0; i < count; ++i)
                {
                    result.u16[i] = is_signed
                                        ? static_cast<uint16_t>(
                                              static_cast<int32_t>(
                                                  static_cast<int8_t>(buffer[i])) +
                                              static_cast<int32_t>(kSignedOffset))
                                        : static_cast<uint16_t>(
                                              static_cast<unsigned char>(buffer[i]));
                }
            }
            else if (is_signed)
            {
                const auto *source = reinterpret_cast<const int16_t *>(buffer.data());
                for (size_t i = 0; i < count; ++i)
                {
                    result.u16[i] = static_cast<uint16_t>(
                        static_cast<int32_t>(source[i]) + static_cast<int32_t>(kSignedOffset));
                }
            }
            else
            {
                std::memcpy(result.u16.data(), buffer.data(), count * sizeof(uint16_t));
            }
            return result;
        }

        std::expected<DecodedImage, std::string> DicomRegionSource::preview(uint32_t max_dimension)
        {
            const uint64_t bytes_per_pixel = info_.bits_per_sample / 8;
            const uint64_t total_bytes = static_cast<uint64_t>(info_.width) * info_.height *
                                         info_.frame_count * bytes_per_pixel;
            if (total_bytes > kMaxDecodeBytes)
            {
                return std::unexpected(std::string(
                    "DICOM 过大：预览需要区域流式读取（尚未实现），请使用支持的区域格式"));
            }

            auto full = decode_dicom(path_);
            if (!full)
            {
                return std::unexpected(full.error());
            }
            const auto &source = *full;
            max_dimension = std::max(1u, max_dimension);
            const uint32_t factor =
                std::max(1u, (std::max(source.meta.width, source.meta.height) + max_dimension - 1) /
                                 max_dimension);
            uint32_t width = 1;
            uint32_t height = 1;
            std::vector<uint16_t> pixels =
                downsample_u16(source.frame_pixels(0), source.meta.width, source.meta.height,
                               factor, width, height);

            DecodedImage preview;
            preview.name = source.name;
            preview.pixel_type = DecodedImage::PixelType::UInt16;
            preview.preview_width = factor > 1 ? width : 0;
            preview.preview_height = factor > 1 ? height : 0;
            preview.initial_invert = source.initial_invert;
            preview.default_window = source.default_window;
            preview.window_presets = source.window_presets;
            preview.padding = source.padding;
            preview.scale = source.scale;
            preview.modality_unit = source.modality_unit;
            // 全量解码已得到首帧完整统计，直接沿用（包含填充值排除）。
            preview.meta = {
                .width = source.meta.width,
                .height = source.meta.height,
                .bits_per_sample = info_.bits_per_sample,
                .samples_per_pixel = 1,
                .frame_count = 1,
                .is_float = false,
                .min_value = source.meta.min_value,
                .max_value = source.meta.max_value,
                .p1 = source.meta.p1,
                .p99 = source.meta.p99,
                .value_range = source.meta.value_range,
            };
            preview.pixels = std::move(pixels);
            return preview;
        }
    } // namespace

    std::expected<std::unique_ptr<RegionSource>, std::string>
    open_dicom_region_source(const std::string &path)
    {
        auto info = read_dicom_info(path);
        if (!info)
        {
            return std::unexpected(info.error());
        }
        return std::make_unique<DicomRegionSource>(path, std::move(info.value()));
    }
} // namespace voilut
