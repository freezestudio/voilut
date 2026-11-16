/// @file window.h
/// @brief 默认窗口尺寸常量。
/// @details
/// GLFW 窗口的初始大小与 Surface 的初始配置取自这里。
/// 窗口允许缩放，运行时以实际 framebuffer 尺寸为准（见 Context::ConfigureSurface）。
///
/// @note 仓库根的 `.gitignore` 用 `core.[0-9]*` 而不是裸 `core` 忽略 core dump，
///       否则会把这个 `core/` 目录一起忽略掉。

#pragma once

#include <cstdint> // uint32_t

namespace voilut
{

    /// @brief 默认窗口宽度（像素）。
    inline constexpr uint32_t kWidth = 1280;

    /// @brief 默认窗口高度（像素）。
    inline constexpr uint32_t kHeight = 720;

} // namespace voilut
