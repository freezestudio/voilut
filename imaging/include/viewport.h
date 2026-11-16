/// @file viewport.h
/// @brief 视口变换（缩放 / 平移）。
///          坐标系：原点在视口左上角，x 向右、y 向下，单位为像素。

#pragma once

#include <array>   // std::array
#include <utility> // std::pair

namespace voilut
{
    /// @brief 视口变换：`scale` 为缩放倍数，`x` / `y` 为图像左上角在视口中的像素位置。
    struct Viewport
    {
        double scale = 1.0;
        double x = 0.0;
        double y = 0.0;
    };

    inline constexpr double kMinScale = 0.02; ///< 最小缩放倍数。
    inline constexpr double kMaxScale = 32.0; ///< 最大缩放倍数。
    inline constexpr double kWheelFactor = 1.12; ///< 每格滚轮的缩放因子。

    /// @brief 计算"适应视口"的初始变换（等比缩放并居中）。
    /// @param image_width 图像宽（像素）。
    /// @param image_height 图像高（像素）。
    /// @param viewport_width 视口宽（像素）。
    /// @param viewport_height 视口高（像素）。
    /// @return 适应后的视口变换；任一尺寸非正时返回默认变换。
    [[nodiscard]] Viewport compute_fit(double image_width, double image_height,
                                       double viewport_width, double viewport_height) noexcept;

    /// @brief 以屏幕点 `(screen_x, screen_y)` 为锚点缩放。
    /// @param vp 当前视口。
    /// @param screen_x 锚点 x（视口坐标）。
    /// @param screen_y 锚点 y（视口坐标）。
    /// @param factor 缩放因子（>1 放大，<1 缩小）。
    /// @return 缩放后的视口；缩放倍数限制在 [kMinScale, kMaxScale]。
    [[nodiscard]] Viewport zoom_at(const Viewport &vp, double screen_x, double screen_y,
                                   double factor) noexcept;

    /// @brief 平移视口。
    /// @param vp 当前视口。
    /// @param dx x 方向位移（像素）。
    /// @param dy y 方向位移（像素）。
    /// @return 平移后的视口。
    [[nodiscard]] Viewport pan(const Viewport &vp, double dx, double dy) noexcept;

    /// @brief 屏幕坐标 → 图像坐标。
    /// @param vp 当前视口。
    /// @param screen_x 屏幕 x（视口坐标）。
    /// @param screen_y 屏幕 y（视口坐标）。
    /// @return 对应的图像坐标 `(x, y)`，可能落在图像范围之外。
    [[nodiscard]] std::pair<double, double> screen_to_image(const Viewport &vp, double screen_x,
                                                            double screen_y) noexcept;

    /// @brief 计算图像四边形（quad）在裁剪空间（NDC）中的矩形。
    /// @param scale 缩放倍数。
    /// @param pan_x 图像左上角在视口中的 x。
    /// @param pan_y 图像左上角在视口中的 y。
    /// @param image_width 图像宽（像素）。
    /// @param image_height 图像高（像素）。
    /// @param viewport_width 视口宽（像素）。
    /// @param viewport_height 视口高（像素）。
    /// @return `{x0, y0, x1, y1}`，NDC 坐标（原点在中心）。
    [[nodiscard]] std::array<double, 4> quad_positions(double scale, double pan_x, double pan_y,
                                                       double image_width, double image_height,
                                                       double viewport_width,
                                                       double viewport_height) noexcept;

    /// @brief 生成四边形（quad）的顶点数据（位置 + UV）。
    /// @param quad `quad_positions` 的输出。
    /// @return 4 个顶点 × 4 个 float（pos.xy, uv.xy），直接用于顶点缓冲。
    [[nodiscard]] std::array<float, 16> quad_vertex_buffer(
        const std::array<double, 4> &quad) noexcept;
} // namespace voilut
