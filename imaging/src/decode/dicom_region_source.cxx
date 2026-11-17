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

        std::vector<double> read_ds_list(const gdcm::DataSet &dataset, const gdcm::Tag &tag)
        {
            std::vector<double> values;
            const std::string text = read_string(dataset, tag);
            size_t start = 0;
            while (start <= text.size())
            {
                const size_t end = text.find('\\', start);
                std::string part =
                    text.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!part.empty())
                {
                    char *parse_end = nullptr;
                    const double value = std::strtod(part.c_str(), &parse_end);
                    if (parse_end != part.c_str())
                    {
                        values.push_back(value);
                    }
                }
                if (end == std::string::npos)
                {
                    break;
                }
                start = end + 1;
            }
            return values;
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

            const auto centers = read_ds_list(dataset, gdcm::Tag(0x0028, 0x1050));
            const auto widths = read_ds_list(dataset, gdcm::Tag(0x0028, 0x1051));
            const size_t pairs = std::min(centers.size(), widths.size());
            for (size_t i = 0; i < pairs; ++i)
            {
                if (widths[i] <= 0.0)
                {
                    continue;
                }
                info.window_presets.push_back(WindowSetting{
                    .level = (centers[i] - intercept) / slope + offset,
                    .width = std::max(widths[i] / slope, 1.0),
                });
            }
            if (!info.window_presets.empty())
            {
                info.default_window = info.window_presets.front();
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
            max_dimension = std::max(1u, max_dimension);
            const uint32_t width = info_.width;
            const uint32_t height = info_.height;

            DecodedImage preview;
            preview.name = info_.name;
            preview.pixel_type = DecodedImage::PixelType::UInt16;
            preview.initial_invert = info_.initial_invert;
            preview.default_window = info_.default_window;
            preview.window_presets = info_.window_presets;
            preview.padding = info_.padding;
            preview.scale = info_.scale;
            preview.modality_unit = info_.modality_unit;

            // 小图直接全量解码；大图走按块区域读取 + 流式降采样。
            if (std::max(width, height) <= max_dimension)
            {
                const uint64_t total_bytes = static_cast<uint64_t>(width) * height *
                                             info_.frame_count * (info_.bits_per_sample / 8);
                if (total_bytes > kMaxDecodeBytes)
                {
                    return std::unexpected(std::string("DICOM 过大：无法一次性解码预览"));
                }
                auto full = decode_dicom(path_);
                if (!full)
                {
                    return std::unexpected(full.error());
                }
                const auto &source = *full;
                preview.pixels.assign(source.frame_pixels(0).begin(),
                                      source.frame_pixels(0).end());
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
                return preview;
            }

            const uint32_t scale = (std::max(width, height) + max_dimension - 1) / max_dimension;
            const uint32_t out_width = std::max(1u, width / scale);
            const uint32_t out_height = std::max(1u, height / scale);
            const size_t out_count = static_cast<size_t>(out_width) * out_height;
            const uint32_t step = std::max(1u, scale / 2);
            preview.preview_width = out_width;
            preview.preview_height = out_height;

            std::vector<uint16_t> counts;
            std::vector<uint64_t> sums;
            std::vector<uint32_t> histogram;
            try
            {
                counts.assign(out_count, 0);
                sums.assign(out_count, 0);
                histogram.assign(65536, 0);
            }
            catch (const std::bad_alloc &)
            {
                return std::unexpected(std::string("内存不足：无法分配 DICOM 预览缓冲区"));
            }

            gdcm::ImageRegionReader reader;
            reader.SetFileName(path_.c_str());
            if (!reader.ReadInformation())
            {
                return std::unexpected(std::string("无法读取 DICOM 信息（预览）"));
            }

            const uint32_t bytes_per_pixel = info_.bits_per_sample / 8;
            const bool is_signed = info_.scale.storage_offset != 0.0;
            const uint32_t chunk_width = std::min(width, 4096u);
            const uint32_t chunk_height = std::min(height, 512u);
            std::vector<char> buffer;

            auto sample_value = [&](const char *data, size_t index) -> uint16_t {
                if (info_.bits_per_sample == 8)
                {
                    const auto raw = static_cast<unsigned char>(data[index]);
                    return is_signed
                               ? static_cast<uint16_t>(static_cast<int32_t>(
                                                         static_cast<int8_t>(raw)) +
                                                         static_cast<int32_t>(kSignedOffset))
                               : raw;
                }
                if (is_signed)
                {
                    int16_t value = 0;
                    std::memcpy(&value, data + index * 2, sizeof(value));
                    return static_cast<uint16_t>(static_cast<int32_t>(value) +
                                                 static_cast<int32_t>(kSignedOffset));
                }
                uint16_t value = 0;
                std::memcpy(&value, data + index * 2, sizeof(value));
                return value;
            };

            for (uint32_t cy = 0; cy < height; cy += chunk_height)
            {
                for (uint32_t cx = 0; cx < width; cx += chunk_width)
                {
                    const uint32_t cw = std::min(chunk_width, width - cx);
                    const uint32_t ch = std::min(chunk_height, height - cy);
                    gdcm::BoxRegion region;
                    region.SetDomain(cx, cx + cw - 1, cy, cy + ch - 1, 0, 0);
                    reader.SetRegion(region);
                    const size_t length = reader.ComputeBufferLength();
                    if (length < static_cast<size_t>(cw) * ch * bytes_per_pixel)
                    {
                        return std::unexpected(std::string("DICOM 预览区域缓冲区异常"));
                    }
                    try
                    {
                        buffer.resize(length);
                    }
                    catch (const std::bad_alloc &)
                    {
                        return std::unexpected(std::string("内存不足：DICOM 预览块分配失败"));
                    }
                    if (!reader.ReadIntoBuffer(buffer.data(), length))
                    {
                        return std::unexpected(
                            std::string("DICOM 预览区域读取失败（可能不支持该压缩语法）"));
                    }

                    const uint32_t gx0 = ((cx + step - 1) / step) * step;
                    const uint32_t gy0 = ((cy + step - 1) / step) * step;
                    for (uint32_t gy = gy0; gy < cy + ch; gy += step)
                    {
                        const size_t local_row = static_cast<size_t>(gy - cy) * cw;
                        for (uint32_t gx = gx0; gx < cx + cw; gx += step)
                        {
                            const uint16_t value =
                                sample_value(buffer.data(), local_row + (gx - cx));
                            const uint32_t ox = gx / scale;
                            const uint32_t oy = gy / scale;
                            if (ox >= out_width || oy >= out_height)
                            {
                                continue;
                            }
                            const size_t index = static_cast<size_t>(oy) * out_width + ox;
                            sums[index] += value;
                            ++counts[index];
                            if (!info_.padding.contains(value))
                            {
                                ++histogram[value];
                            }
                        }
                    }
                }
            }

            std::vector<uint16_t> pixels(out_count);
            size_t source_count = 0;
            uint32_t source_min = 65535;
            uint32_t source_max = 0;
            for (size_t i = 0; i < 65536; ++i)
            {
                if (histogram[i] == 0)
                {
                    continue;
                }
                source_count += histogram[i];
                source_min = std::min(source_min, static_cast<uint32_t>(i));
                source_max = std::max(source_max, static_cast<uint32_t>(i));
            }
            if (source_count == 0)
            {
                return std::unexpected(std::string("DICOM 预览没有有效像素"));
            }
            const size_t rank = std::max<size_t>(1, source_count / 100);
            uint32_t p1 = source_min;
            uint32_t p99 = source_max;
            bool found = false;
            uint64_t acc = 0;
            for (uint32_t value = 0; value < 65536; ++value)
            {
                acc += histogram[value];
                if (!found && acc >= rank)
                {
                    p1 = value;
                    found = true;
                }
            }
            acc = 0;
            for (int value = 65535; value >= 0; --value)
            {
                acc += histogram[static_cast<size_t>(value)];
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
            for (size_t i = 0; i < out_count; ++i)
            {
                pixels[i] = counts[i] == 0
                                ? 0
                                : static_cast<uint16_t>((sums[i] + counts[i] / 2) / counts[i]);
            }
            preview.pixels = std::move(pixels);
            preview.meta = {
                .width = width,
                .height = height,
                .bits_per_sample = info_.bits_per_sample,
                .samples_per_pixel = 1,
                .frame_count = 1,
                .is_float = false,
                .min_value = static_cast<double>(source_min),
                .max_value = static_cast<double>(source_max),
                .p1 = static_cast<double>(p1),
                .p99 = static_cast<double>(p99),
                .value_range = static_cast<double>(source_max - source_min),
            };
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
