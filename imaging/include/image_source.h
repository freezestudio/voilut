/// @file image_source.h
/// @brief 图像来源的目录扫描（文件浏览器与目录展开共用）。
/// @details 只依赖扩展名判断是否受支持（见 `kind_from_name`），
///          非递归遍历目录，供目录选择与启动参数展开使用。

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace voilut
{
    /// @brief 目录中的一条条目。
    struct DirEntry
    {
        std::string name;          ///< 条目名（不含路径）。
        std::string path;          ///< 完整路径。
        bool is_directory = false; ///< 是否为目录。
    };

    /// @brief 把路径转换为用于显示 / 日志的 UTF-8 字符串。
    /// @details C++26 的 `std::filesystem::path::display_string()` 在 MSVC STL 尚未实现
    ///          （缺少该成员），这里改用 `u8string()`：Windows 下把宽字符路径按 UTF-8 编码，
    ///          保证中文路径在 ImGui / std::println 中正确显示，避免按 ANSI 编码时丢失。
    /// @param path 路径。
    /// @return UTF-8 编码的字符串。
    [[nodiscard]] inline std::string path_display(const std::filesystem::path &path)
    {
        const std::u8string utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    /// @brief 判断路径是否为受支持的图像文件（按扩展名，大小写不敏感）。
    /// @param path 文件路径。
    /// @return 后缀为 `.tif` / `.tiff` / `.dcm` 时返回 `true`。
    [[nodiscard]] bool is_image_file(const std::filesystem::path &path) noexcept;

    /// @brief 列出目录内容：子目录在前，图像文件在后，各自按名称排序。
    /// @param dir 目录路径。
    /// @return 条目列表；目录不可读时返回空列表。
    [[nodiscard]] std::vector<DirEntry> list_directory(const std::filesystem::path &dir);

    /// @brief 收集目录中的图像文件（非递归，按名称排序）。
    /// @param dir 目录路径。
    /// @return 图像文件的绝对路径列表；目录不可读时返回空列表。
    [[nodiscard]] std::vector<std::string> collect_images(const std::filesystem::path &dir);
} // namespace voilut
