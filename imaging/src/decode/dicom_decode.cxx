/// @file dicom_decode.cxx
/// @brief DICOM 解码实现（GDCM）。

#include "decode/dicom_decode.h"

#include <algorithm> // std::max
#include <cstdint>
#include <cstdlib> // std::strtod
#include <cstring> // std::memcpy
#include <format>
#include <new> // std::bad_alloc
#include <optional>
#include <print>
#include <string>
#include <vector>

#include "decode/image_decode.h" // kMaxDecodeBytes

#include <gdcmByteValue.h>
#include <gdcmDataElement.h>
#include <gdcmDataSet.h>
#include <gdcmFile.h>
#include <gdcmImage.h>
#include <gdcmImageReader.h>
#include <gdcmPhotometricInterpretation.h>
#include <gdcmPixelFormat.h>
#include <gdcmReader.h>
#include <gdcmTag.h>
#include <gdcmTrace.h>
#include <gdcmTransferSyntax.h>

namespace voilut
{
    namespace
    {
        /// @brief 有符号像素统一加偏移 32768，转换为 16 位无符号。
        constexpr double kSignedOffset = 32768.0;

        std::string file_name_of(const std::string &path)
        {
            const size_t slash = path.find_last_of("/\\");
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        /// @brief 读取一个 DS（十进制字符串）标签，取第一个值。
        std::optional<double> read_ds(const gdcm::DataSet &dataset, const gdcm::Tag &tag)
        {
            if (!dataset.FindDataElement(tag))
            {
                return std::nullopt;
            }
            const gdcm::DataElement &element = dataset.GetDataElement(tag);
            const gdcm::ByteValue *bytes = element.GetByteValue();
            if (bytes == nullptr || bytes->GetLength() == 0)
            {
                return std::nullopt;
            }
            const std::string text(bytes->GetPointer(), bytes->GetLength());
            char *end = nullptr;
            const double value = std::strtod(text.c_str(), &end);
            if (end == text.c_str())
            {
                return std::nullopt;
            }
            return value;
        }

        /// @brief 读取 US（无符号短整数）标签（小端）。缺失时返回 `std::nullopt`。
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

        /// @brief 读取字符串标签，去掉尾部空格 / NUL。
        std::string read_string(const gdcm::DataSet &dataset, const gdcm::Tag &tag)
        {
            if (!dataset.FindDataElement(tag))
            {
                return {};
            }
            const gdcm::ByteValue *bytes = dataset.GetDataElement(tag).GetByteValue();
            if (bytes == nullptr)
            {
                return {};
            }
            std::string text(bytes->GetPointer(), bytes->GetLength());
            while (!text.empty() && (text.back() == ' ' || text.back() == '\0'))
            {
                text.pop_back();
            }
            return text;
        }

        /// @brief 组装解码结果：像素转 16 位 + Rescale + 默认窗 + 统计。
        /// @param path 文件路径（仅用于取文件名）。
        /// @param raw 原始像素字节（未压缩或已由 GDCM 解压）。
        /// @param width 宽（列数）。
        /// @param height 高（行数）。
        /// @param bits_allocated 每像素分配位数（8 / 16）。
        /// @param bits_stored 有效位数。
        /// @param is_signed 像素是否为有符号。
        /// @param monochrome1 光度解释是否为 MONOCHROME1（需反色）。
        /// @param frame_count 帧数（多帧只取第 1 帧，但缓冲区按全部帧大小分配）。
        /// @param dataset 数据集（用于读取 Rescale / 窗设置）。
        /// @return 成功返回 `DecodedImage`，失败返回错误说明。
        std::expected<DecodedImage, std::string> assemble_decoded(
            const std::string &path, const uint8_t *raw, unsigned int width, unsigned int height,
            unsigned int bits_allocated, unsigned int bits_stored, bool is_signed, bool monochrome1,
            unsigned int frame_count, const gdcm::DataSet &dataset)
        {
            if (width == 0 || height == 0)
            {
                return std::unexpected(std::string("DICOM 缺少图像尺寸（Rows/Columns）"));
            }
            if (bits_allocated != 8 && bits_allocated != 16)
            {
                return std::unexpected(
                    std::format("仅支持 8 位或 16 位灰度 DICOM，当前为 {} 位", bits_allocated));
            }

            // GDCM 已按传输语法解压，此处得到的是原始像素字节；统一转换为 16 位。
            // 多帧时按 `frame_count` 连续存放（第 f 帧从 f*count 开始）。
            const size_t count = static_cast<size_t>(width) * height;
            const uint32_t frames = std::max(1u, frame_count);
            const size_t bytes_per_frame = count * (bits_allocated / 8);
            std::vector<uint16_t> pixels;
            try
            {
                pixels.resize(count * frames);
            }
            catch (const std::bad_alloc &)
            {
                return std::unexpected(
                    std::format("内存不足：无法为 {}×{}×{} 帧分配像素缓冲区", width, height,
                                frames));
            }

            for (uint32_t frame = 0; frame < frames; ++frame)
            {
                const uint8_t *frame_raw = raw + static_cast<size_t>(frame) * bytes_per_frame;
                uint16_t *dst = pixels.data() + static_cast<size_t>(frame) * count;
                if (bits_allocated == 8)
                {
                    for (size_t i = 0; i < count; ++i)
                    {
                        if (is_signed)
                        {
                            dst[i] = static_cast<uint16_t>(
                                static_cast<int32_t>(static_cast<int8_t>(frame_raw[i])) +
                                static_cast<int32_t>(kSignedOffset));
                        }
                        else
                        {
                            dst[i] = frame_raw[i];
                        }
                    }
                }
                else if (is_signed)
                {
                    const auto *source = reinterpret_cast<const int16_t *>(frame_raw);
                    for (size_t i = 0; i < count; ++i)
                    {
                        dst[i] = static_cast<uint16_t>(static_cast<int32_t>(source[i]) +
                                                          static_cast<int32_t>(kSignedOffset));
                    }
                }
                else
                {
                    std::memcpy(dst, frame_raw, count * sizeof(uint16_t));
                }
            }

            // Rescale / 默认窗（优先用 DICOM 自带的 WindowCenter / WindowWidth）
            const double slope = read_ds(dataset, gdcm::Tag(0x0028, 0x1053)).value_or(1.0);
            const double intercept = read_ds(dataset, gdcm::Tag(0x0028, 0x1052)).value_or(0.0);

            std::optional<WindowSetting> default_window;
            const auto window_center = read_ds(dataset, gdcm::Tag(0x0028, 0x1050));
            const auto window_width = read_ds(dataset, gdcm::Tag(0x0028, 0x1051));
            if (window_center && window_width && *window_width > 0.0)
            {
                const double offset = is_signed ? kSignedOffset : 0.0;
                default_window = WindowSetting{
                    .level = (*window_center - intercept) / slope + offset,
                    .width = std::max(*window_width / slope, 1.0),
                };
            }

            const ImageStats stats = compute_stats(
                std::span<const uint16_t>(pixels.data(), count));
            DecodedImage result;
            result.name = file_name_of(path);
            result.meta = {
                .width = width,
                .height = height,
                .bits_per_sample = bits_stored,
                .samples_per_pixel = 1,
                .frame_count = std::max(1u, frame_count),
                .min_value = stats.min_value,
                .max_value = stats.max_value,
                .p1 = stats.p1,
                .p99 = stats.p99,
                .value_range = stats.value_range,
            };
            result.pixels = std::move(pixels);
            result.default_window = default_window;
            result.initial_invert = monochrome1;
            return result;
        }
    } // namespace

    std::expected<DecodedImage, std::string> decode_dicom(const std::string &path)
    {
        // GDCM 对未知 SOP Class 会输出大量 warning / debug 噪声，这里只保留 error；
        // 解码失败原因由本函数的返回值给出。
        gdcm::Trace::SetDebug(false);
        gdcm::Trace::SetWarning(false);

        // ---- 标准路径：GDCM ImageReader（支持压缩传输语法与已知 SOP Class）----
        {
            gdcm::ImageReader reader;
            reader.SetFileName(path.c_str());
            if (reader.Read())
            {
                const gdcm::Image &image = reader.GetImage();
                const gdcm::PixelFormat &pixel_format = image.GetPixelFormat();
                const unsigned int width = image.GetDimensions()[0];
                const unsigned int height = image.GetDimensions()[1];
                const unsigned int samples = pixel_format.GetSamplesPerPixel();
                const unsigned int bits_allocated = pixel_format.GetBitsAllocated();
                const unsigned int bits_stored = pixel_format.GetBitsStored();
                const gdcm::PixelFormat::ScalarType scalar_type = pixel_format.GetScalarType();
                const bool is_signed = scalar_type == gdcm::PixelFormat::INT8 ||
                                       scalar_type == gdcm::PixelFormat::INT16;

                if (width == 0 || height == 0)
                {
                    return std::unexpected(std::string("DICOM 缺少图像尺寸（Rows/Columns）"));
                }
                if (samples != 1)
                {
                    return std::unexpected(
                        std::format("仅支持单通道灰度 DICOM，当前为 {} 通道", samples));
                }
                if (bits_allocated != 8 && bits_allocated != 16)
                {
                    return std::unexpected(
                        std::format("仅支持 8 位或 16 位灰度 DICOM，当前为 {} 位", bits_allocated));
                }

                const gdcm::PhotometricInterpretation::PIType photo =
                    image.GetPhotometricInterpretation().GetType();
                if (photo != gdcm::PhotometricInterpretation::MONOCHROME1 &&
                    photo != gdcm::PhotometricInterpretation::MONOCHROME2)
                {
                    return std::unexpected(std::string("仅支持 MONOCHROME 光度解释的 DICOM"));
                }

                // GDCM 已按传输语法解压，此处得到的是原始像素字节；统一转换为 16 位。
                // 多帧时 GetBuffer 会写入「全部帧」长度的数据（GetBufferLength 已乘帧数），
                // 因此必须按全量分配，否则会堆溢出；目前只显示第 1 帧。
                const size_t count = static_cast<size_t>(width) * height;
                const unsigned int frame_count =
                    image.GetNumberOfDimensions() >= 3 ? image.GetDimensions()[2] : 1;
                const size_t expected = count * (bits_allocated / 8) * std::max(1u, frame_count);
                const size_t buffer_length = image.GetBufferLength();
                if (buffer_length < expected)
                {
                    return std::unexpected(std::format(
                        "DICOM 像素缓冲区异常：需要至少 {} 字节，实际 {} 字节", expected,
                        buffer_length));
                }
                if (buffer_length > kMaxDecodeBytes)
                {
                    return std::unexpected(std::format(
                        "DICOM 像素数据过大（{} 字节，上限 {}）：需要按需分帧解码",
                        buffer_length, kMaxDecodeBytes));
                }
                // GDCM 缓冲区必须全量分配（GetBuffer 会写全部帧）；像素副本再受剩余预算限制。
                const size_t budget = kMaxDecodeBytes - buffer_length;
                const size_t max_frames_by_budget =
                    std::max<size_t>(1, budget / (count * sizeof(uint16_t)));
                const unsigned int loaded_frames = static_cast<unsigned int>(
                    std::min<size_t>(frame_count, max_frames_by_budget));
                if (loaded_frames < frame_count)
                {
                    std::println("警告: DICOM 共 {} 帧，受内存上限限制仅加载前 {} 帧", frame_count,
                                 loaded_frames);
                }
                std::vector<uint8_t> buffer;
                try
                {
                    buffer.resize(buffer_length);
                }
                catch (const std::bad_alloc &)
                {
                    return std::unexpected(std::format(
                        "内存不足：无法为 {}×{}×{} 帧的 DICOM 分配像素缓冲区", width, height,
                        frame_count));
                }
                if (!image.GetBuffer(reinterpret_cast<char *>(buffer.data())))
                {
                    return std::unexpected(std::string("DICOM 像素数据解码失败"));
                }

                return assemble_decoded(path, buffer.data(), width, height, bits_allocated,
                                        bits_stored, is_signed,
                                        photo == gdcm::PhotometricInterpretation::MONOCHROME1,
                                        loaded_frames, reader.GetFile().GetDataSet());
            }
        }

        // ---- 兼容路径：未知 SOP Class（如 DCMTK 私有 SOP Class）----
        // GDCM 的 MediaStorage 字典不认识该类 SOP Class 时会直接跳过；
        // 这里改用底层 Reader 读取数据集（不检查 Media Storage），仅支持未压缩像素。
        {
            gdcm::Reader reader;
            reader.SetFileName(path.c_str());
            if (!reader.Read())
            {
                return std::unexpected(std::format("无法读取 DICOM 文件：{}", path));
            }

            const gdcm::DataSet &dataset = reader.GetFile().GetDataSet();
            const gdcm::TransferSyntax &transfer_syntax =
                reader.GetFile().GetHeader().GetDataSetTransferSyntax();
            if (transfer_syntax.IsEncapsulated())
            {
                return std::unexpected(std::string("未知 SOP Class 且像素为压缩封装，暂不支持"));
            }

            // DICOM：Rows(0028,0010) 为行数（高），Columns(0028,0011) 为列数（宽）
            const auto columns = read_us(dataset, gdcm::Tag(0x0028, 0x0011));
            const auto rows = read_us(dataset, gdcm::Tag(0x0028, 0x0010));
            const auto samples = read_us(dataset, gdcm::Tag(0x0028, 0x0002)).value_or(1);
            const auto bits_allocated = read_us(dataset, gdcm::Tag(0x0028, 0x0100));
            const auto bits_stored =
                read_us(dataset, gdcm::Tag(0x0028, 0x0101)).value_or(bits_allocated.value_or(0));
            const auto pixel_representation = read_us(dataset, gdcm::Tag(0x0028, 0x0103)).value_or(0);
            const std::string photometric = read_string(dataset, gdcm::Tag(0x0028, 0x0004));

            if (!columns || !rows || *columns == 0 || *rows == 0)
            {
                return std::unexpected(std::string("DICOM 缺少图像尺寸（Rows/Columns）"));
            }
            if (samples != 1)
            {
                return std::unexpected(
                    std::format("仅支持单通道灰度 DICOM，当前为 {} 通道", samples));
            }
            if (!bits_allocated || (*bits_allocated != 8 && *bits_allocated != 16))
            {
                return std::unexpected(std::format("仅支持 8 位或 16 位灰度 DICOM，当前为 {} 位",
                                                   bits_allocated.value_or(0)));
            }
            if (photometric != "MONOCHROME1" && photometric != "MONOCHROME2")
            {
                return std::unexpected(std::string("仅支持 MONOCHROME 光度解释的 DICOM"));
            }

            const gdcm::Tag pixel_tag(0x7fe0, 0x0010);
            if (!dataset.FindDataElement(pixel_tag))
            {
                return std::unexpected(std::string("DICOM 缺少像素数据（PixelData）"));
            }
            const gdcm::ByteValue *pixel_bytes =
                dataset.GetDataElement(pixel_tag).GetByteValue();
            if (pixel_bytes == nullptr)
            {
                return std::unexpected(std::string("DICOM 像素数据不是原始字节（可能为封装格式）"));
            }
            const size_t frame_bytes =
                static_cast<size_t>(*columns) * *rows * (*bits_allocated / 8);
            if (pixel_bytes->GetLength() < frame_bytes)
            {
                return std::unexpected(std::string("DICOM 像素数据长度不足"));
            }
            // 未知 SOP 的兼容路径按原始长度推算帧数；像素副本受内存上限限制。
            const unsigned int frame_count = static_cast<unsigned int>(
                std::max<size_t>(1, pixel_bytes->GetLength() / std::max<size_t>(1, frame_bytes)));
            const size_t max_frames = std::max<size_t>(
                1, kMaxDecodeBytes /
                       std::max<size_t>(1, static_cast<size_t>(*columns) * *rows * sizeof(uint16_t)));
            const unsigned int loaded_frames =
                static_cast<unsigned int>(std::min<size_t>(frame_count, max_frames));
            if (loaded_frames < frame_count)
            {
                std::println("警告: DICOM 共 {} 帧，受内存上限限制仅加载前 {} 帧", frame_count,
                             loaded_frames);
            }

            return assemble_decoded(
                path, reinterpret_cast<const uint8_t *>(pixel_bytes->GetPointer()), *columns, *rows,
                *bits_allocated, bits_stored, pixel_representation != 0,
                photometric == "MONOCHROME1", loaded_frames, dataset);
        }
    }
} // namespace voilut
