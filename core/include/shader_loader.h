/// @file shader_loader.h
/// @brief 读取 WGSL 着色器源码。
/// @details
/// 着色器是一段运行在 GPU 上的程序，存放在 `shaders/*.wgsl` 文件里。
/// 程序启动时需要把文件内容读成字符串，再交给 WebGPU 编译。
///
/// 错误处理选择：返回 `std::expected<std::string, std::string>`
///   - 成功：expected 里存着色器源码；
///   - 失败：expected 里存错误说明（如"文件不存在"）。
/// 相比抛异常，这种写法要求调用者显式处理失败，且没有异常开销。
///
/// @note 兼容性：C++26 的 `#embed` 可以把文件内容直接编进二进制，完全避免
///       运行时读取文件；但 GCC 目前尚未完整实现，所以这里仍用文件读取。
///       代码中保留了 `#embed` 的注释版本（见 shader_loader.cxx），
///       待工具链支持后即可切换。

#pragma once

#include <string>
#include <expected> // C++23
#include <string_view>

namespace voilut
{

    /// @brief 读取 `shaders/` 目录下的着色器文件。
    /// @param fileName 只传文件名（如 `"image.wgsl"`），目录由编译宏 `SHADERS_DIR` 与可执行文件位置共同决定。
    /// @return 成功时含着色器源码，失败时含错误说明。
    [[nodiscard("ReadShaderSource 必须检查返回值")]]
    std::expected<std::string, std::string> ReadShaderSource(std::string_view fileName);

} // namespace voilut
