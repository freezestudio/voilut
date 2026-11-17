/// @file window.h
/// @brief 默认窗口尺寸常量。
/// @details
/// 这里是 96 DPI（100% 缩放）下的设计尺寸；应用启动时会按系统 content scale
/// 放大窗口和 UI，4K/200% 下实际创建 2560×1440 的窗口。窗口允许缩放，
/// 运行时以实际 framebuffer 尺寸为准（见 Context::ConfigureSurface）。
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
