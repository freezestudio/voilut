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
/// @note 单文件发行：CMake 选项 `VOILUT_EMBED_SHADERS=ON` 会把着色器编进二进制，
///       此时运行时不读文件系统（实现见 CMake 生成的 `embedded_shaders.h`）。
///       开发时可用环境变量 `VOILUT_SHADERS_DIR` 指向源码树的 `shaders/`，
///       修改 WGSL 后无需重新编译即可生效。

#pragma once

#include <string>
#include <expected> // C++23
#include <string_view>

namespace voilut
{

    /// @brief 读取 `shaders/` 目录下的着色器文件，或返回内嵌在二进制中的源码。
    /// @param fileName 只传文件名（如 `"image.wgsl"`）。未嵌入时由编译宏 `SHADERS_DIR`
    ///        与可执行文件位置共同决定查找目录。
    /// @return 成功时含着色器源码，失败时含错误说明。
    [[nodiscard("ReadShaderSource 必须检查返回值")]]
    std::expected<std::string, std::string> ReadShaderSource(std::string_view fileName);

} // namespace voilut
