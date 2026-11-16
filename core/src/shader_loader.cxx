// ============================================================
// shader_loader.cxx —— 读取 WGSL 着色器源码
// ============================================================
// 两种方案：
//   A) 运行时读取文件（当前采用）：实现简单，修改 shader 无需重新编译 C++；
//      缺点是可执行文件依赖 shaders/ 目录的位置。
//   B) C++26 #embed（待工具链支持）：编译期把 shader 内嵌进二进制，
//      单文件分发、启动更快。
// ============================================================

#include "shader_loader.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Windows 下用 GetModuleFileNameW 定位可执行文件目录（便携版把 shaders/ 放在 exe 旁边）。
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

// SHADERS_DIR 由 CMake 通过 target_compile_definitions 注入：
//   桌面端 = 相对路径 shaders（不写入构建机绝对路径）
//   Web 端 = 虚拟文件系统里的 /shaders
#ifndef SHADERS_DIR
#define SHADERS_DIR "shaders" // 兜底：手动指定默认值
#endif

namespace
{
    /// @brief 取当前可执行文件所在目录（便携版据此定位随附的 shaders/）。
    /// @return 目录路径；无法确定时返回空路径。
    std::filesystem::path executable_directory()
    {
#if defined(_WIN32)
        std::wstring buffer(32768, L'\0');
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                                  static_cast<DWORD>(buffer.size()));
        if (length > 0 && length < static_cast<DWORD>(buffer.size()))
        {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
#elif defined(__linux__)
        std::error_code ec;
        const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec)
        {
            return exe.parent_path();
        }
#endif
        return {};
    }
} // namespace

namespace voilut
{

    std::expected<std::string, std::string> ReadShaderSource(std::string_view fileName)
    {
        // ----------------------------------------------------------
        // 方案 B（C++26 #embed）预留写法，等 GCC 完整支持后启用：
        //   constexpr auto shaderData = #embed EMBEDDED_SHADER_PATH;
        //   return std::string(shaderData.begin(), shaderData.end());
        // 对应的编译宏在 CMakeLists.txt 里（SHADER_EMBED）。
        // ----------------------------------------------------------

        // ----------------------------------------------------------
        // 方案 A：按候选路径依次查找并整文件读入
        //   1) 可执行文件旁的 shaders/  —— 便携版（解压后即可运行）
        //   2) 编译期注入的 SHADERS_DIR —— 构建时复制到输出目录的着色器
        //   3) 当前工作目录的 shaders/  —— 手动指定运行目录
        // ----------------------------------------------------------
        const std::filesystem::path name(fileName);
        std::vector<std::filesystem::path> candidates;
        const std::filesystem::path exe_dir = executable_directory();
        if (!exe_dir.empty())
        {
            candidates.push_back(exe_dir / "shaders" / name);
        }
        candidates.push_back(std::filesystem::path(SHADERS_DIR) / name);
        candidates.push_back(std::filesystem::path("shaders") / name);

        for (const std::filesystem::path &path : candidates)
        {
            std::ifstream file(path, std::ios::binary);
            if (file)
            {
                // 用 rdbuf() 一次性读入整个文件，避免手动逐行读取
                std::ostringstream buffer;
                buffer << file.rdbuf();
                return buffer.str();
            }
        }

        // 返回错误而不是抛异常：调用者必须显式处理
        return std::unexpected("无法读取着色器文件: " + std::string(fileName) +
                               "（已尝试可执行文件旁、构建配置的着色器目录与当前目录的 shaders/）");
    }

} // namespace voilut
