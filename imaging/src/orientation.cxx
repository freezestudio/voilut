/// @file orientation.cxx
/// @brief 显示方向（旋转 / 翻转）的实现。

#include "orientation.h"

namespace voilut
{
    Orientation rotate_cw(const Orientation &o) noexcept
    {
        Orientation result = o;
        result.rotation = (o.rotation + 1) % 4;
        return result;
    }

    int encode_orientation(const Orientation &o) noexcept
    {
        int code = ((o.rotation % 4) + 4) % 4;
        if (o.flip_h)
        {
            code |= 0x4;
        }
        if (o.flip_v)
        {
            code |= 0x8;
        }
        return code;
    }

    std::pair<uint32_t, uint32_t> display_size(uint32_t width, uint32_t height,
                                               const Orientation &o) noexcept
    {
        const int rotation = ((o.rotation % 4) + 4) % 4;
        if (rotation == 1 || rotation == 3)
        {
            return {height, width};
        }
        return {width, height};
    }

    std::pair<double, double> display_uv_to_image_uv(double u, double v,
                                                     const Orientation &o) noexcept
    {
        const int code = encode_orientation(o);
        const int rotation = code & 3;
        double x = u;
        double y = v;
        switch (rotation)
        {
        case 1:
            x = v;
            y = 1.0 - u;
            break;
        case 2:
            x = 1.0 - u;
            y = 1.0 - v;
            break;
        case 3:
            x = 1.0 - v;
            y = u;
            break;
        default:
            break;
        }
        if ((code & 0x4) != 0)
        {
            x = 1.0 - x;
        }
        if ((code & 0x8) != 0)
        {
            y = 1.0 - y;
        }
        return {x, y};
    }

    std::pair<double, double> image_uv_to_display_uv(double u, double v,
                                                     const Orientation &o) noexcept
    {
        const int code = encode_orientation(o);
        // 先撤销翻转（翻转作用于旋转后的坐标）
        double x = u;
        double y = v;
        if ((code & 0x4) != 0)
        {
            x = 1.0 - x;
        }
        if ((code & 0x8) != 0)
        {
            y = 1.0 - y;
        }
        // 再撤销旋转
        double display_u = x;
        double display_v = y;
        switch (code & 3)
        {
        case 1:
            display_u = 1.0 - y;
            display_v = x;
            break;
        case 2:
            display_u = 1.0 - x;
            display_v = 1.0 - y;
            break;
        case 3:
            display_u = y;
            display_v = 1.0 - x;
            break;
        default:
            break;
        }
        return {display_u, display_v};
    }
} // namespace voilut
