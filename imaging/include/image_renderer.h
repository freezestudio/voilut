/// @file image_renderer.h
/// @brief 16 位灰度图像的 Dawn 渲染管线（窗宽窗位 / 反色 / 插值）。
/// @details 着色器为 `shaders/image.wgsl`：16 位纹理 → 窗宽窗位映射 → 8 位显示。

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <webgpu/webgpu_cpp.h>

#include "image_model.h"
#include "orientation.h"
#include "viewport.h"

namespace voilut
{
    class Context;
}

namespace voilut
{
    /// @brief 窗宽窗位参数，与 `shaders/image.wgsl` 的 `WindowParams` 逐字段一致（32 字节）。
    struct WindowParams
    {
        float level = 0.0f;      ///< 窗位（Window Level）。
        float width = 1.0f;      ///< 窗宽（Window Width）。
        float invert = 0.0f;     ///< 反色开关（>0.5 反色）。
        float linear = 0.0f;     ///< 插值开关（>0.5 双线性）。
        float transform = 0.0f;  ///< 显示方向编码（见 `encode_orientation`）。
        float pad0 = 0.0f;       ///< 对齐填充。
        float pad1 = 0.0f;       ///< 对齐填充。
        float pad2 = 0.0f;       ///< 对齐填充。
    };

    /// @brief 使用 Dawn 渲染 16 位灰度图像。
    class ImageRenderer
    {
    public:
        /// @brief 构造渲染器。
        /// @param context WebGPU 环境（不拥有，生命周期由调用方保证）。
        explicit ImageRenderer(Context &context);
        ~ImageRenderer();

        ImageRenderer(const ImageRenderer &) = delete;
        ImageRenderer &operator=(const ImageRenderer &) = delete;

        /// @brief 读取 WGSL 并创建管线（只需调用一次）。
        /// @return 失败时返回错误说明。
        [[nodiscard]] std::expected<void, std::string> init();

        /// @brief 设置视口逻辑尺寸（像素）。
        /// @param css_width 视口宽。
        /// @param css_height 视口高。
        void resize(double css_width, double css_height);

        /// @brief 上传 16 位像素（会重建 GPU 纹理）。
        /// @param width 图像宽。
        /// @param height 图像高。
        /// @param pixels 行优先的 16 位像素。
        void upload(uint32_t width, uint32_t height, std::span<const uint16_t> pixels);

        /// @brief 设置窗宽窗位。
        void set_window(double level, double width);
        /// @brief 设置是否反色。
        void set_invert(bool invert);
        /// @brief 设置是否线性插值。
        void set_interpolation(bool linear);
        /// @brief 设置显示方向（旋转 / 翻转）。
        void set_orientation(const Orientation &orientation);
        /// @brief 设置视口变换（缩放 / 平移）。
        void set_transform(const Viewport &vp);

        /// @brief 把图像绘制命令编码到给定渲染通道，供上层与 UI 合并提交。
        /// @param pass 已开始的渲染通道。
        void record(wgpu::RenderPassEncoder &pass);

    private:
        void create_pipelines(std::string_view image_wgsl);
        void update_quad();

        Context &context_; ///< WebGPU 环境引用。

        wgpu::RenderPipeline image_pipeline_; ///< 图像渲染管线。
        wgpu::Buffer vertex_buffer_;          ///< 四边形（quad）顶点缓冲。
        wgpu::Buffer index_buffer_;           ///< 四边形（quad）索引缓冲。
        wgpu::Buffer params_buffer_;          ///< 窗宽窗位 uniform 缓冲。
        wgpu::BindGroup params_bind_group_;   ///< group 0 绑定。
        wgpu::Texture image_texture_;         ///< 16 位图像纹理。
        wgpu::BindGroup image_bind_group_;    ///< group 1 绑定。

        uint32_t image_width_ = 0;  ///< 图像宽。
        uint32_t image_height_ = 0; ///< 图像高。
        double css_width_ = 1.0;    ///< 视口宽（逻辑像素）。
        double css_height_ = 1.0;   ///< 视口高（逻辑像素）。
        Viewport viewport_{};       ///< 当前视口变换。
        Orientation orientation_{}; ///< 当前显示方向。
        WindowParams params_{};     ///< 当前窗宽窗位参数。
    };
} // namespace voilut
