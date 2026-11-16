// ============================================================
// shader_loader.cxx —— 读取 WGSL 着色器源码
// ============================================================
// 两种方案：
//   A) 运行时读取文件（默认开发构建）：修改 shader 无需重新编译 C++；
//      缺点是可执行文件依赖 shaders/ 目录的位置。
//   B) 编译期嵌入（CMake 选项 VOILUT_EMBED_SHADERS=ON）：CMake 把 shader
//      转成字节数组写进 generated/embedded_shaders.h，单文件分发、启动更快。
//      开发时仍可用环境变量 VOILUT_SHADERS_DIR 指到源码树临时覆盖。
// ============================================================

#include "shader_loader.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// 嵌入模式下由 CMake 生成（build/generated/embedded_shaders.h），
// 提供 voilut::embedded::kShaders。
#if defined(VOILUT_EMBED_SHADERS)
#include "embedded_shaders.h"
#endif

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

#if defined(VOILUT_EMBED_SHADERS)
    /// @brief 读取环境变量。
    /// @details 只在嵌入模式（开发覆盖路径）使用。MSVC 把 `std::getenv` 标记为
    ///          不安全（C4996），这里用 `_dupenv_s` 获取副本；其它平台仍用标准
    ///          `std::getenv`。
    /// @param name 环境变量名。
    /// @return 变量值；未设置时返回空字符串。
    std::string environment_value(const char *name)
    {
#if defined(_MSC_VER)
        char *value = nullptr;
        size_t size = 0;
        if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
        {
            return {};
        }
        std::string result(value);
        std::free(value);
        return result;
#else
        const char *value = std::getenv(name);
        return value != nullptr ? std::string(value) : std::string{};
#endif
    }
#endif // VOILUT_EMBED_SHADERS

    /// @brief 按候选路径读入整个着色器文件。
    /// @param fileName 文件名（如 `"image.wgsl"`）。
    /// @param preferred_dir 优先尝试的目录（环境变量覆盖；为空时跳过）。
    /// @return 成功时含源码，失败时含错误说明。
    std::expected<std::string, std::string> read_from_disk(
        std::string_view fileName, const std::filesystem::path &preferred_dir)
    {
        // 候选顺序：
        //   0) 环境变量指定的目录（仅嵌入模式下用于开发覆盖）
        //   1) 可执行文件旁的 shaders/  —— 便携版（解压后即可运行）
        //   2) 编译期注入的 SHADERS_DIR —— 构建时复制到输出目录的着色器
        //   3) 当前工作目录的 shaders/  —— 手动指定运行目录
        const std::filesystem::path name(fileName);
        std::vector<std::filesystem::path> candidates;
        if (!preferred_dir.empty())
        {
            candidates.push_back(preferred_dir / name);
        }
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
} // namespace

namespace voilut
{

    std::expected<std::string, std::string> ReadShaderSource(std::string_view fileName)
    {
#if defined(VOILUT_EMBED_SHADERS)
        // 嵌入模式：优先用环境变量指向的目录（开发期改 WGSL 免重编译）。
        const std::string override_dir = environment_value("VOILUT_SHADERS_DIR");
        if (!override_dir.empty())
        {
            return read_from_disk(fileName, std::filesystem::path(override_dir));
        }
        // 正常路径：直接从二进制的只读数据段取字节，不碰文件系统。
        for (const embedded::ShaderSource &shader : embedded::kShaders)
        {
            if (shader.name == fileName)
            {
                return std::string(reinterpret_cast<const char *>(shader.data), shader.size);
            }
        }
        return std::unexpected("未知的内嵌着色器: " + std::string(fileName));
#else
        // 非嵌入模式：按候选路径读文件（开发构建）。
        return read_from_disk(fileName, {});
#endif
    }

} // namespace voilut
