/// @file orientation.h
/// @brief 图像的显示方向（旋转 / 翻转）及其坐标变换。
/// @details 旋转只影响显示，不改变像素数据本身。
///          显示坐标 → 原图坐标的变换必须与 `shaders/image.wgsl` 的 UV 变换
///          完全一致，供渲染、悬停取像素与 ROI 统计共用。

#pragma once

#include <cstdint>
#include <utility>

namespace voilut
{
    /// @brief 显示方向：顺时针旋转 90° 的倍数 + 水平 / 垂直翻转。
    struct Orientation
    {
        int rotation = 0;       ///< 顺时针旋转，取值 0 / 1 / 2 / 3（对应 0° / 90° / 180° / 270°）。
        bool flip_h = false;    ///< 是否水平翻转（左右）。
        bool flip_v = false;    ///< 是否垂直翻转（上下）。
    };

    /// @brief 顺时针旋转 90°。
    /// @param o 当前方向。
    /// @return 旋转后的方向（旋转角 +90°）。
    [[nodiscard]] Orientation rotate_cw(const Orientation &o) noexcept;

    /// @brief 编码为 0..15 的整数：bit0-1 旋转，bit2 水平翻转，bit3 垂直翻转。
    /// @param o 方向。
    /// @return 编码值（可直接传给着色器 uniform）。
    [[nodiscard]] int encode_orientation(const Orientation &o) noexcept;

    /// @brief 旋转 / 翻转后的显示尺寸（旋转 90° / 270° 时宽高互换）。
    /// @param width 原图宽。
    /// @param height 原图高。
    /// @param o 方向。
    /// @return `{显示宽, 显示高}`。
    [[nodiscard]] std::pair<uint32_t, uint32_t> display_size(uint32_t width, uint32_t height,
                                                             const Orientation &o) noexcept;

    /// @brief 显示归一化坐标（UV）→ 原图归一化坐标。
    /// @details 与 `image.wgsl` 中 `orient_uv` 的公式保持一致。
    /// @param u 显示 UV 的 x（0..1）。
    /// @param v 显示 UV 的 y（0..1）。
    /// @param o 方向。
    /// @return `{原图 UV x, 原图 UV y}`。
    [[nodiscard]] std::pair<double, double> display_uv_to_image_uv(double u, double v,
                                                                   const Orientation &o) noexcept;

    /// @brief 原图归一化坐标（UV）→ 显示归一化坐标（`display_uv_to_image_uv` 的逆）。
    /// @details 供把原图坐标映射到当前显示方向。
    /// @param u 原图 UV 的 x（0..1）。
    /// @param v 原图 UV 的 y（0..1）。
    /// @param o 方向。
    /// @return `{显示 UV x, 显示 UV y}`。
    [[nodiscard]] std::pair<double, double> image_uv_to_display_uv(double u, double v,
                                                                   const Orientation &o) noexcept;
} // namespace voilut
