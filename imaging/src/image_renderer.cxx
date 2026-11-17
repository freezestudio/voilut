/// @file image_renderer.cxx
/// @brief ImageRenderer 实现（Dawn 管线）。

#include "image_renderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <vector>

#include "context.h"
#include "shader_loader.h"

namespace voilut
{
    namespace
    {
        /// @brief 四边形（quad）的两个三角形（顶点索引）：0-1-2 与 0-2-3。
        constexpr std::array<uint16_t, 6> kQuadIndices = {0, 1, 2, 0, 2, 3};

        /// @brief 图像纹理格式：16 位无符号单通道。
        constexpr wgpu::TextureFormat kImageFormat = wgpu::TextureFormat::R16Uint;
        /// @brief 浮点图像的纹理格式：用 R32Uint 存 float 的位模式，着色器 bitcast。
        constexpr wgpu::TextureFormat kFloatImageFormat = wgpu::TextureFormat::R32Uint;

        /// @brief 顶点步长：pos.xy + uv.xy = 4 个 float。
        constexpr uint64_t kVertexStride = sizeof(float) * 4;

        /// @brief mipmap 层数：一直降采样到 1×1（供缩小显示时按 LOD 采样）。
        uint32_t mip_level_count(uint32_t width, uint32_t height) noexcept
        {
            uint32_t levels = 1;
            for (uint32_t size = std::max(width, height); size > 1; size /= 2)
            {
                ++levels;
            }
            return levels;
        }

        /// @brief 2×2 盒式滤波降采样（边缘重复采样，避免越界）。
        /// @param src 源像素。
        /// @param src_width 源宽。
        /// @param src_height 源高。
        /// @param out_width 输出宽（`max(1, src_width/2)`）。
        /// @param out_height 输出高。
        /// @return 降采样后的像素。
        std::vector<uint16_t> downsample_2x2(std::span<const uint16_t> src, uint32_t src_width,
                                             uint32_t src_height, uint32_t &out_width,
                                             uint32_t &out_height)
        {
            out_width = std::max(1u, src_width / 2);
            out_height = std::max(1u, src_height / 2);
            std::vector<uint16_t> dst(static_cast<size_t>(out_width) * out_height);
            for (uint32_t y = 0; y < out_height; ++y)
            {
                const uint32_t y0 = std::min(y * 2, src_height - 1);
                const uint32_t y1 = std::min(y * 2 + 1, src_height - 1);
                for (uint32_t x = 0; x < out_width; ++x)
                {
                    const uint32_t x0 = std::min(x * 2, src_width - 1);
                    const uint32_t x1 = std::min(x * 2 + 1, src_width - 1);
                    const uint32_t sum = src[static_cast<size_t>(y0) * src_width + x0] +
                                         src[static_cast<size_t>(y0) * src_width + x1] +
                                         src[static_cast<size_t>(y1) * src_width + x0] +
                                         src[static_cast<size_t>(y1) * src_width + x1];
                    // +2 实现四舍五入；和最大 4×65535，不会溢出 uint32。
                    dst[static_cast<size_t>(y) * out_width + x] =
                        static_cast<uint16_t>((sum + 2) / 4);
                }
            }
            return dst;
        }

        /// @brief 浮点版本的 2×2 盒式滤波（在 float 域平均，而不是按位平均）。
        std::vector<float> downsample_2x2_float(std::span<const float> src, uint32_t src_width,
                                                uint32_t src_height, uint32_t &out_width,
                                                uint32_t &out_height)
        {
            out_width = std::max(1u, src_width / 2);
            out_height = std::max(1u, src_height / 2);
            std::vector<float> dst(static_cast<size_t>(out_width) * out_height);
            for (uint32_t y = 0; y < out_height; ++y)
            {
                const uint32_t y0 = std::min(y * 2, src_height - 1);
                const uint32_t y1 = std::min(y * 2 + 1, src_height - 1);
                for (uint32_t x = 0; x < out_width; ++x)
                {
                    const uint32_t x0 = std::min(x * 2, src_width - 1);
                    const uint32_t x1 = std::min(x * 2 + 1, src_width - 1);
                    const float sum = src[static_cast<size_t>(y0) * src_width + x0] +
                                      src[static_cast<size_t>(y0) * src_width + x1] +
                                      src[static_cast<size_t>(y1) * src_width + x0] +
                                      src[static_cast<size_t>(y1) * src_width + x1];
                    dst[static_cast<size_t>(y) * out_width + x] = sum / 4.0f;
                }
            }
            return dst;
        }

        /// @brief 按整数倍做盒式降采样（用于超过纹理上限的大图预览）。
        /// @param src 源像素。
        /// @param src_width 源宽。
        /// @param src_height 源高。
        /// @param factor 降采样倍数（≥ 2）。
        /// @param out_width 输出宽（`max(1, src_width/factor)`）。
        /// @param out_height 输出高。
        std::vector<uint16_t> downsample_u16(std::span<const uint16_t> src, uint32_t src_width,
                                             uint32_t src_height, uint32_t factor,
                                             uint32_t &out_width, uint32_t &out_height)
        {
            out_width = std::max(1u, src_width / factor);
            out_height = std::max(1u, src_height / factor);
            std::vector<uint16_t> dst(static_cast<size_t>(out_width) * out_height);
            std::vector<uint64_t> sums(out_width);
            for (uint32_t oy = 0; oy < out_height; ++oy)
            {
                std::fill(sums.begin(), sums.end(), 0);
                const uint32_t y0 = oy * factor;
                const uint32_t y1 = std::min(y0 + factor, src_height);
                for (uint32_t y = y0; y < y1; ++y)
                {
                    const uint16_t *row = src.data() + static_cast<size_t>(y) * src_width;
                    for (uint32_t ox = 0; ox < out_width; ++ox)
                    {
                        const uint32_t x0 = ox * factor;
                        const uint32_t x1 = std::min(x0 + factor, src_width);
                        uint64_t sum = 0;
                        for (uint32_t x = x0; x < x1; ++x)
                        {
                            sum += row[x];
                        }
                        sums[ox] += sum;
                    }
                }
                for (uint32_t ox = 0; ox < out_width; ++ox)
                {
                    const uint32_t x0 = ox * factor;
                    const uint32_t x1 = std::min(x0 + factor, src_width);
                    const uint64_t count = static_cast<uint64_t>(y1 - y0) * (x1 - x0);
                    dst[static_cast<size_t>(oy) * out_width + ox] =
                        static_cast<uint16_t>((sums[ox] + count / 2) / count);
                }
            }
            return dst;
        }

        /// @brief 浮点版本的整数倍盒式降采样。
        std::vector<float> downsample_f32(std::span<const float> src, uint32_t src_width,
                                          uint32_t src_height, uint32_t factor,
                                          uint32_t &out_width, uint32_t &out_height)
        {
            out_width = std::max(1u, src_width / factor);
            out_height = std::max(1u, src_height / factor);
            std::vector<float> dst(static_cast<size_t>(out_width) * out_height);
            std::vector<double> sums(out_width);
            for (uint32_t oy = 0; oy < out_height; ++oy)
            {
                std::fill(sums.begin(), sums.end(), 0.0);
                const uint32_t y0 = oy * factor;
                const uint32_t y1 = std::min(y0 + factor, src_height);
                for (uint32_t y = y0; y < y1; ++y)
                {
                    const float *row = src.data() + static_cast<size_t>(y) * src_width;
                    for (uint32_t ox = 0; ox < out_width; ++ox)
                    {
                        const uint32_t x0 = ox * factor;
                        const uint32_t x1 = std::min(x0 + factor, src_width);
                        double sum = 0.0;
                        for (uint32_t x = x0; x < x1; ++x)
                        {
                            sum += row[x];
                        }
                        sums[ox] += sum;
                    }
                }
                for (uint32_t ox = 0; ox < out_width; ++ox)
                {
                    const uint32_t x0 = ox * factor;
                    const uint32_t x1 = std::min(x0 + factor, src_width);
                    const double count = static_cast<double>(y1 - y0) * (x1 - x0);
                    dst[static_cast<size_t>(oy) * out_width + ox] =
                        static_cast<float>(sums[ox] / count);
                }
            }
            return dst;
        }
    } // namespace

    ImageRenderer::ImageRenderer(Context &context) : context_(context) {}

    ImageRenderer::~ImageRenderer() = default;

    std::expected<void, std::string> ImageRenderer::init()
    {
        const auto wgsl = ReadShaderSource("image.wgsl");
        if (!wgsl)
        {
            return std::unexpected(std::format("加载 image.wgsl 失败：{}", wgsl.error()));
        }
        // 顶点缓冲：4 个顶点 × 4 个 float
        {
            wgpu::BufferDescriptor desc{
                .usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
                .size = kVertexStride * 4,
            };
            vertex_buffer_ = context_.device.CreateBuffer(&desc);
        }
        // 索引缓冲：固定 6 个索引，创建后写入一次
        {
            wgpu::BufferDescriptor desc{
                .usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst,
                .size = kQuadIndices.size() * sizeof(uint16_t),
            };
            index_buffer_ = context_.device.CreateBuffer(&desc);
            context_.device.GetQueue().WriteBuffer(index_buffer_, 0, kQuadIndices.data(),
                                                   kQuadIndices.size() * sizeof(uint16_t));
        }
        // uniform 缓冲：窗宽窗位参数
        {
            wgpu::BufferDescriptor desc{
                .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                .size = sizeof(WindowParams),
            };
            params_buffer_ = context_.device.CreateBuffer(&desc);
        }

        // 缓冲创建完成后再创建管线（create_pipelines 会引用 params_buffer_）
        create_pipelines(wgsl.value());
        return {};
    }

    void ImageRenderer::resize(double css_width, double css_height)
    {
        css_width_ = css_width;
        css_height_ = css_height;
        update_quad();
    }

    std::expected<void, std::string> ImageRenderer::upload(uint32_t width, uint32_t height,
                                                          std::span<const uint16_t> pixels)
    {
        if (width == 0 || height == 0)
        {
            return std::unexpected(std::string("图像尺寸为 0"));
        }
        if (pixels.size() < static_cast<size_t>(width) * height)
        {
            return std::unexpected(std::string("像素数据长度不足"));
        }

        image_width_ = width;
        image_height_ = height;
        preview_scale_ = 1;
        image_bind_group_ = nullptr;

        // 设备上限由 Context 请求（Dawn 默认只给 8192，科研图像很容易超）。
        // 超过上限时按整数倍盒式滤波降采样成预览纹理，而不是直接报错。
        std::span<const uint16_t> upload_pixels = pixels;
        std::vector<uint16_t> reduced;
        uint32_t tex_width = width;
        uint32_t tex_height = height;
        try
        {
            if (width > context_.max_texture_dimension || height > context_.max_texture_dimension)
            {
                const uint32_t factor = std::max(
                    (width + context_.max_texture_dimension - 1) /
                        context_.max_texture_dimension,
                    (height + context_.max_texture_dimension - 1) /
                        context_.max_texture_dimension);
                uint32_t reduced_width = 1;
                uint32_t reduced_height = 1;
                reduced = downsample_u16(pixels, width, height, factor, reduced_width,
                                         reduced_height);
                upload_pixels = reduced;
                tex_width = reduced_width;
                tex_height = reduced_height;
                preview_scale_ = factor;
            }
        }
        catch (const std::bad_alloc &)
        {
            return std::unexpected(std::string("内存不足：生成大图预览时分配失败"));
        }

        const uint32_t level_count = mip_level_count(tex_width, tex_height);
        wgpu::TextureDescriptor texDesc{
            .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
            .dimension = wgpu::TextureDimension::e2D,
            .size = {.width = tex_width, .height = tex_height, .depthOrArrayLayers = 1},
            .format = kImageFormat,
            .mipLevelCount = level_count,
            .sampleCount = 1,
        };

        try
        {
            image_texture_ = context_.device.CreateTexture(&texDesc);

            // 第 0 层直接写（可能是降采样后的）原图；其后每层由上一层 2×2 盒式滤波得到。
            {
                const wgpu::TexelCopyTextureInfo destination{.texture = image_texture_};
                const wgpu::TexelCopyBufferLayout layout{
                    .offset = 0,
                    .bytesPerRow = tex_width * 2,
                    .rowsPerImage = tex_height,
                };
                const wgpu::Extent3D write_size{
                    .width = tex_width, .height = tex_height, .depthOrArrayLayers = 1};
                context_.device.GetQueue().WriteTexture(&destination, upload_pixels.data(),
                                                        upload_pixels.size() * sizeof(uint16_t),
                                                        &layout, &write_size);
            }

            std::span<const uint16_t> source = upload_pixels;
            uint32_t source_width = tex_width;
            uint32_t source_height = tex_height;
            std::vector<uint16_t> level_pixels;
            for (uint32_t level = 1; level < level_count; ++level)
            {
                uint32_t level_width = 1;
                uint32_t level_height = 1;
                std::vector<uint16_t> next =
                    downsample_2x2(source, source_width, source_height, level_width, level_height);

                const wgpu::TexelCopyTextureInfo destination{
                    .texture = image_texture_,
                    .mipLevel = level,
                };
                const wgpu::TexelCopyBufferLayout layout{
                    .offset = 0,
                    .bytesPerRow = level_width * 2,
                    .rowsPerImage = level_height,
                };
                const wgpu::Extent3D write_size{
                    .width = level_width, .height = level_height, .depthOrArrayLayers = 1};
                context_.device.GetQueue().WriteTexture(
                    &destination, next.data(), next.size() * sizeof(uint16_t), &layout,
                    &write_size);

                level_pixels = std::move(next);
                source = level_pixels;
                source_width = level_width;
                source_height = level_height;
            }
        }
        catch (const std::bad_alloc &)
        {
            image_texture_ = nullptr;
            return std::unexpected(std::string("内存不足：生成 mipmap 时分配失败"));
        }

        const wgpu::BindGroupEntry entry{
            .binding = 0,
            .textureView = image_texture_.CreateView(),
        };
        const wgpu::BindGroupDescriptor bgDesc{
            .layout = image_pipeline_.GetBindGroupLayout(1),
            .entryCount = 1,
            .entries = &entry,
        };
        image_bind_group_ = context_.device.CreateBindGroup(&bgDesc);

        params_.value_type = 0.0f;
        update_quad();
        return {};
    }

    std::expected<void, std::string> ImageRenderer::upload_float(uint32_t width, uint32_t height,
                                                                 std::span<const float> pixels)
    {
        if (width == 0 || height == 0)
        {
            return std::unexpected(std::string("图像尺寸为 0"));
        }
        if (pixels.size() < static_cast<size_t>(width) * height)
        {
            return std::unexpected(std::string("浮点像素数据长度不足"));
        }

        image_width_ = width;
        image_height_ = height;
        preview_scale_ = 1;
        image_bind_group_ = nullptr;

        std::span<const float> upload_pixels = pixels;
        std::vector<float> reduced;
        uint32_t tex_width = width;
        uint32_t tex_height = height;
        try
        {
            if (width > context_.max_texture_dimension || height > context_.max_texture_dimension)
            {
                const uint32_t factor = std::max(
                    (width + context_.max_texture_dimension - 1) /
                        context_.max_texture_dimension,
                    (height + context_.max_texture_dimension - 1) /
                        context_.max_texture_dimension);
                uint32_t reduced_width = 1;
                uint32_t reduced_height = 1;
                reduced = downsample_f32(pixels, width, height, factor, reduced_width,
                                         reduced_height);
                upload_pixels = reduced;
                tex_width = reduced_width;
                tex_height = reduced_height;
                preview_scale_ = factor;
            }
        }
        catch (const std::bad_alloc &)
        {
            return std::unexpected(std::string("内存不足：生成浮点大图预览时分配失败"));
        }

        const uint32_t level_count = mip_level_count(tex_width, tex_height);
        wgpu::TextureDescriptor texDesc{
            .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
            .dimension = wgpu::TextureDimension::e2D,
            .size = {.width = tex_width, .height = tex_height, .depthOrArrayLayers = 1},
            .format = kFloatImageFormat,
            .mipLevelCount = level_count,
            .sampleCount = 1,
        };

        try
        {
            image_texture_ = context_.device.CreateTexture(&texDesc);

            {
                const wgpu::TexelCopyTextureInfo destination{.texture = image_texture_};
                const wgpu::TexelCopyBufferLayout layout{
                    .offset = 0,
                    .bytesPerRow = tex_width * static_cast<uint32_t>(sizeof(float)),
                    .rowsPerImage = tex_height,
                };
                const wgpu::Extent3D write_size{
                    .width = tex_width, .height = tex_height, .depthOrArrayLayers = 1};
                context_.device.GetQueue().WriteTexture(
                    &destination, upload_pixels.data(), upload_pixels.size() * sizeof(float),
                    &layout, &write_size);
            }

            std::span<const float> source = upload_pixels;
            uint32_t source_width = tex_width;
            uint32_t source_height = tex_height;
            std::vector<float> level_pixels;
            for (uint32_t level = 1; level < level_count; ++level)
            {
                uint32_t level_width = 1;
                uint32_t level_height = 1;
                std::vector<float> next = downsample_2x2_float(source, source_width,
                                                               source_height, level_width,
                                                               level_height);

                const wgpu::TexelCopyTextureInfo destination{
                    .texture = image_texture_,
                    .mipLevel = level,
                };
                const wgpu::TexelCopyBufferLayout layout{
                    .offset = 0,
                    .bytesPerRow = level_width * static_cast<uint32_t>(sizeof(float)),
                    .rowsPerImage = level_height,
                };
                const wgpu::Extent3D write_size{
                    .width = level_width, .height = level_height, .depthOrArrayLayers = 1};
                context_.device.GetQueue().WriteTexture(&destination, next.data(),
                                                        next.size() * sizeof(float), &layout,
                                                        &write_size);

                level_pixels = std::move(next);
                source = level_pixels;
                source_width = level_width;
                source_height = level_height;
            }
        }
        catch (const std::bad_alloc &)
        {
            image_texture_ = nullptr;
            return std::unexpected(std::string("内存不足：生成浮点 mipmap 时分配失败"));
        }

        const wgpu::BindGroupEntry entry{
            .binding = 0,
            .textureView = image_texture_.CreateView(),
        };
        const wgpu::BindGroupDescriptor bgDesc{
            .layout = image_pipeline_.GetBindGroupLayout(1),
            .entryCount = 1,
            .entries = &entry,
        };
        image_bind_group_ = context_.device.CreateBindGroup(&bgDesc);

        params_.value_type = 1.0f;
        update_quad();
        return {};
    }

    void ImageRenderer::set_window(double level, double width)
    {
        params_.level = static_cast<float>(level);
        params_.width = static_cast<float>(width);
    }

    void ImageRenderer::set_voi_mode(VoiMode mode)
    {
        params_.mode = mode == VoiMode::Dicom ? 1.0f : 0.0f;
    }

    void ImageRenderer::set_invert(bool invert)
    {
        params_.invert = invert ? 1.0f : 0.0f;
    }

    void ImageRenderer::set_interpolation(bool linear)
    {
        params_.linear = linear ? 1.0f : 0.0f;
    }

    void ImageRenderer::set_orientation(const Orientation &orientation)
    {
        orientation_ = orientation;
        params_.transform = static_cast<float>(encode_orientation(orientation));
        update_quad();
    }

    void ImageRenderer::set_transform(const Viewport &vp)
    {
        viewport_ = vp;
        update_quad();
    }

    void ImageRenderer::record(wgpu::RenderPassEncoder &pass)
    {
        if (!image_bind_group_)
        {
            return;
        }
        context_.device.GetQueue().WriteBuffer(params_buffer_, 0, &params_, sizeof(params_));
        pass.SetPipeline(image_pipeline_);
        pass.SetVertexBuffer(0, vertex_buffer_);
        pass.SetIndexBuffer(index_buffer_, wgpu::IndexFormat::Uint16);
        pass.SetBindGroup(0, params_bind_group_);
        pass.SetBindGroup(1, image_bind_group_);
        pass.DrawIndexed(static_cast<uint32_t>(kQuadIndices.size()));
    }

    void ImageRenderer::create_pipelines(std::string_view image_wgsl)
    {
        wgpu::ShaderSourceWGSL source{{.code = image_wgsl.data()}};
        wgpu::ShaderModuleDescriptor moduleDesc{.nextInChain = &source};
        wgpu::ShaderModule module = context_.device.CreateShaderModule(&moduleDesc);

        // group 0：窗宽窗位 uniform
        wgpu::BindGroupLayoutEntry uniformEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Fragment,
            .buffer = {.type = wgpu::BufferBindingType::Uniform,
                       .minBindingSize = sizeof(WindowParams)},
        };
        wgpu::BindGroupLayoutDescriptor uniformLayoutDesc{.entryCount = 1, .entries = &uniformEntry};
        wgpu::BindGroupLayout uniformLayout =
            context_.device.CreateBindGroupLayout(&uniformLayoutDesc);

        // group 1：16 位整数纹理
        wgpu::BindGroupLayoutEntry textureEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Fragment,
            .texture = {.sampleType = wgpu::TextureSampleType::Uint},
        };
        wgpu::BindGroupLayoutDescriptor textureLayoutDesc{.entryCount = 1, .entries = &textureEntry};
        wgpu::BindGroupLayout textureLayout =
            context_.device.CreateBindGroupLayout(&textureLayoutDesc);

        const std::array<wgpu::BindGroupLayout, 2> layouts = {uniformLayout, textureLayout};
        wgpu::PipelineLayoutDescriptor layoutDesc{
            .bindGroupLayoutCount = layouts.size(),
            .bindGroupLayouts = layouts.data(),
        };
        wgpu::PipelineLayout pipelineLayout = context_.device.CreatePipelineLayout(&layoutDesc);

        // group 0 的绑定（params_buffer_ 已在 init() 中创建；
        // params_bind_group_ 随后使用管线自带的布局创建）。
        const std::array<wgpu::VertexAttribute, 2> attributes = {
            wgpu::VertexAttribute{.format = wgpu::VertexFormat::Float32x2,
                                  .offset = 0,
                                  .shaderLocation = 0},
            wgpu::VertexAttribute{.format = wgpu::VertexFormat::Float32x2,
                                  .offset = 8,
                                  .shaderLocation = 1},
        };
        wgpu::VertexBufferLayout vertexLayout{
            .stepMode = wgpu::VertexStepMode::Vertex,
            .arrayStride = kVertexStride,
            .attributeCount = attributes.size(),
            .attributes = attributes.data(),
        };
        wgpu::VertexState vertexState{
            .module = module,
            .entryPoint = "vs",
            .bufferCount = 1,
            .buffers = &vertexLayout,
        };
        wgpu::ColorTargetState colorTarget{.format = context_.format};
        wgpu::FragmentState fragmentState{
            .module = module,
            .entryPoint = "fs",
            .targetCount = 1,
            .targets = &colorTarget,
        };
        wgpu::RenderPipelineDescriptor pipelineDesc{
            .layout = pipelineLayout,
            .vertex = vertexState,
            .fragment = &fragmentState,
        };
        image_pipeline_ = context_.device.CreateRenderPipeline(&pipelineDesc);

        const wgpu::BindGroupEntry paramsEntry{
            .binding = 0,
            .buffer = params_buffer_,
            .size = sizeof(WindowParams),
        };
        const wgpu::BindGroupDescriptor paramsBgDesc{
            .layout = image_pipeline_.GetBindGroupLayout(0),
            .entryCount = 1,
            .entries = &paramsEntry,
        };
        params_bind_group_ = context_.device.CreateBindGroup(&paramsBgDesc);
    }

    void ImageRenderer::update_quad()
    {
        if (image_width_ == 0 || image_height_ == 0)
        {
            return;
        }
        const auto [display_width, display_height] =
            display_size(image_width_, image_height_, orientation_);
        const auto quad = quad_positions(viewport_.scale, viewport_.x, viewport_.y,
                                         static_cast<double>(display_width),
                                         static_cast<double>(display_height), css_width_,
                                         css_height_);
        const auto vertices = quad_vertex_buffer(quad);
        context_.device.GetQueue().WriteBuffer(vertex_buffer_, 0, vertices.data(),
                                               sizeof(float) * vertices.size());
    }
} // namespace voilut
