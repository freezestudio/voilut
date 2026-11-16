/// @file image_source.cxx
/// @brief 图像来源的目录扫描实现。

#include "image_source.h"

#include <algorithm>
#include <cctype>

#include "decode/image_decode.h"

namespace voilut
{
    namespace
    {
        /// @brief 名称的字典序比较（ASCII 大小写不敏感）。
        bool name_less(const std::string &lhs, const std::string &rhs) noexcept
        {
            const std::size_t count = std::min(lhs.size(), rhs.size());
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto lower = [](unsigned char c) { return std::tolower(c); };
                const int a = lower(static_cast<unsigned char>(lhs[i]));
                const int b = lower(static_cast<unsigned char>(rhs[i]));
                if (a != b)
                {
                    return a < b;
                }
            }
            return lhs.size() < rhs.size();
        }

        /// @brief 排序：目录优先，其次按名称（大小写不敏感）。
        bool entry_less(const DirEntry &lhs, const DirEntry &rhs) noexcept
        {
            if (lhs.is_directory != rhs.is_directory)
            {
                return lhs.is_directory;
            }
            return name_less(lhs.name, rhs.name);
        }

        /// @brief 取绝对路径（失败时回退为原路径）。
        std::string absolute_string(const std::filesystem::path &path)
        {
            std::error_code ec;
            const std::filesystem::path abs = std::filesystem::absolute(path, ec);
            return path_display(ec ? path : abs);
        }
    } // namespace

    bool is_image_file(const std::filesystem::path &path) noexcept
    {
        const std::string name = path_display(path.filename());
        return kind_from_name(name).has_value();
    }

    std::vector<DirEntry> list_directory(const std::filesystem::path &dir)
    {
        std::vector<DirEntry> entries;
        std::error_code ec;
        std::filesystem::directory_iterator iterator(dir, ec);
        if (ec)
        {
            return entries;
        }
        for (const std::filesystem::directory_entry &item : iterator)
        {
            std::error_code type_ec;
            if (item.is_directory(type_ec))
            {
                if (!type_ec)
                {
                    entries.push_back(
                        DirEntry{path_display(item.path().filename()),
                                 absolute_string(item.path()), true});
                }
                continue;
            }
            if (!item.is_regular_file(type_ec) || type_ec)
            {
                continue;
            }
            if (is_image_file(item.path()))
            {
                entries.push_back(DirEntry{path_display(item.path().filename()),
                                           absolute_string(item.path()), false});
            }
        }
        std::sort(entries.begin(), entries.end(), entry_less);
        return entries;
    }

    std::vector<std::string> collect_images(const std::filesystem::path &dir)
    {
        std::vector<std::string> images;
        std::error_code ec;
        std::filesystem::directory_iterator iterator(dir, ec);
        if (ec)
        {
            return images;
        }
        for (const std::filesystem::directory_entry &item : iterator)
        {
            std::error_code type_ec;
            if (!item.is_regular_file(type_ec) || type_ec || !is_image_file(item.path()))
            {
                continue;
            }
            images.push_back(absolute_string(item.path()));
        }
        std::sort(images.begin(), images.end(), name_less);
        return images;
    }
} // namespace voilut
