/// @file viewport.cxx
/// @brief 视口变换的实现。

#include "viewport.h"

#include <algorithm> // std::clamp / std::min

namespace voilut
{
    Viewport compute_fit(double image_width, double image_height, double viewport_width,
                         double viewport_height) noexcept
    {
        if (image_width <= 0.0 || image_height <= 0.0 || viewport_width <= 0.0 ||
            viewport_height <= 0.0)
        {
            return {};
        }
        const double scale =
            std::min(viewport_width / image_width, viewport_height / image_height);
        return {
            .scale = scale,
            .x = (viewport_width - image_width * scale) * 0.5,
            .y = (viewport_height - image_height * scale) * 0.5,
        };
    }

    Viewport zoom_at(const Viewport &vp, double screen_x, double screen_y, double factor) noexcept
    {
        const double scale = std::clamp(vp.scale * factor, kMinScale, kMaxScale);
        if (scale == vp.scale)
        {
            return vp;
        }
        const double ratio = scale / vp.scale;
        return {
            .scale = scale,
            .x = screen_x - (screen_x - vp.x) * ratio,
            .y = screen_y - (screen_y - vp.y) * ratio,
        };
    }

    Viewport pan(const Viewport &vp, double dx, double dy) noexcept
    {
        return {.scale = vp.scale, .x = vp.x + dx, .y = vp.y + dy};
    }

    std::pair<double, double> screen_to_image(const Viewport &vp, double screen_x,
                                              double screen_y) noexcept
    {
        return {
            (screen_x - vp.x) / vp.scale,
            (screen_y - vp.y) / vp.scale,
        };
    }

    std::array<double, 4> quad_positions(double scale, double pan_x, double pan_y,
                                         double image_width, double image_height,
                                         double viewport_width, double viewport_height) noexcept
    {
        const double left = pan_x;
        const double top = pan_y;
        const double right = left + image_width * scale;
        const double bottom = top + image_height * scale;
        const double x0 = (left / viewport_width) * 2.0 - 1.0;
        const double x1 = (right / viewport_width) * 2.0 - 1.0;
        const double y0 = 1.0 - (top / viewport_height) * 2.0;
        const double y1 = 1.0 - (bottom / viewport_height) * 2.0;
        return {x0, y0, x1, y1};
    }

    std::array<float, 16> quad_vertex_buffer(const std::array<double, 4> &quad) noexcept
    {
        const auto [x0, y0, x1, y1] = quad;
        const auto f = [](double value) { return static_cast<float>(value); };
        return {
            f(x0), f(y0), 0.0f, 0.0f, // 左上：位置 + UV(0,0)
            f(x1), f(y0), 1.0f, 0.0f, // 右上：UV(1,0)
            f(x1), f(y1), 1.0f, 1.0f, // 右下：UV(1,1)
            f(x0), f(y1), 0.0f, 1.0f, // 左下：UV(0,1)
        };
    }
} // namespace voilut
