/// @file image_io.cxx
/// @brief 图像列表、后台解码与打开目录。

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>
#else
#include <sys/wait.h>
#endif

#include "image_io.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <future>
#include <print>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "app_ops.h"
#include "decode/image_decode.h"
#include "image_model.h"
#include "region_source.h"
#include "image_source.h"

namespace voilut::app
{
    namespace
    {
        /// @brief 超过该边长时走虚拟预览路径（与渲染器请求的纹理上限一致）。
        constexpr uint32_t kVirtualSourceDimension = 16384;
        /// @brief 虚拟预览纹理的最大边长（有界内存；细节由后续 phase 提供）。
        constexpr uint32_t kVirtualPreviewDimension = 4096;

        /// @brief 解码：小图全量；超大 TIFF 走区域源预览，避免分配整幅像素。
        std::expected<LoadedImage, std::string> load_for_viewer(const std::string &path)
        {
            const auto kind = voilut::kind_from_name(path);
            if (!kind)
            {
                return std::unexpected(std::format("无法识别的图像格式：{}", path));
            }

            {
                if (auto source = voilut::open_region_source(path))
                {
                    const voilut::RegionInfo &info = (*source)->info();
                    const uint64_t bytes_per_pixel =
                        info.sample_type == voilut::SampleType::Float32 ? 4u : 2u;
                    const uint64_t total_bytes = static_cast<uint64_t>(info.width) * info.height *
                                                 info.frame_count * bytes_per_pixel;
                    if (info.width > kVirtualSourceDimension ||
                        info.height > kVirtualSourceDimension ||
                        total_bytes > voilut::kMaxDecodeBytes)
                    {
                        auto preview = (*source)->preview(kVirtualPreviewDimension);
                        if (preview)
                        {
                            return LoadedImage{
                                .image = std::move(preview.value()),
                                .source = std::shared_ptr<voilut::RegionSource>(
                                    std::move(*source)),
                            };
                        }
                        if (total_bytes > voilut::kMaxDecodeBytes)
                        {
                            return std::unexpected(preview.error());
                        }
                        // 预览失败但内存仍可承受时回退全量解码。
                    }
                }
            }
            auto decoded = voilut::decode_image(path, *kind);
            if (!decoded)
            {
                return std::unexpected(decoded.error());
            }
            return LoadedImage{.image = std::move(decoded.value()), .source = {}};
        }
    } // namespace

    void start_load(AppState *state, const std::string &path)
    {
        state->load_path = path;
        state->loading = true;
        GLFWwindow *window = state->window;
        state->load_future = std::async(std::launch::async, [path, window]() {
            auto result = load_for_viewer(path);
            if (window != nullptr)
            {
                glfwPostEmptyEvent(); // 唤醒主循环（GLFW 允许跨线程调用）
            }
            return result;
        });
    }

    /// @brief 解码并显示图像列表中的第 `index` 项（后台解码，完成后在主循环应用）。
    void load_playlist_item(AppState *state, std::size_t index)
    {
        if (state->playlist.empty())
        {
            return;
        }
        const std::size_t count = state->playlist.size();
        const std::size_t target = index % count;
        if (state->loading)
        {
            // 加载中收到翻页请求：记下最后一次，当前解码结束后再加载。
            state->pending_index = target;
            state->has_pending = true;
            return;
        }
        state->playlist_index = target;
        start_load(state, state->playlist[target]);
    }

    /// @brief 主循环每帧调用：把已完成的后台解码结果应用到 UI / GPU。
    void poll_load(AppState *state)
    {
        if (!state->loading || !state->load_future.valid())
        {
            return;
        }
        if (state->load_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return;
        }
        auto result = state->load_future.get();
        state->loading = false;
        if (result)
        {
            apply_image(state, std::move(result->image), state->load_path,
                        std::move(result->source));
        }
        else
        {
            report_error(state, std::format("解码失败：{}", result.error()));
        }
        if (state->has_pending)
        {
            state->has_pending = false;
            load_playlist_item(state, state->pending_index);
        }
        glfwPostEmptyEvent();
    }

    /// @brief 打开单个文件（以所在目录重建图像列表）。
    void open_path(AppState *state, const std::string &path)
    {
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        const std::filesystem::path target = ec ? std::filesystem::path(path) : absolute;

        state->playlist = voilut::collect_images(target.parent_path());
        const std::string wanted = voilut::path_display(target);
        const auto it = std::find(state->playlist.begin(), state->playlist.end(), wanted);
        state->playlist_index =
            it != state->playlist.end() ? static_cast<std::size_t>(it - state->playlist.begin()) : 0;
        if (state->playlist.empty())
        {
            state->playlist.push_back(wanted); // 目录不可读时仍允许单张显示
            state->playlist_index = 0;
        }
        load_playlist_item(state, state->playlist_index);
    }

    /// @brief 切换图像（`delta` 为 ±1，循环）。
    void step_playlist(AppState *state, int delta)
    {
        if (state->playlist.empty())
        {
            return;
        }
        const long count = static_cast<long>(state->playlist.size());
        long index = (static_cast<long>(state->playlist_index) + delta) % count;
        if (index < 0)
        {
            index += count;
        }
        load_playlist_item(state, static_cast<std::size_t>(index));
    }

    /// @brief 打开目录（展开为图像列表并加载首张）。
    void open_directory(AppState *state, const std::string &dir)
    {
        const auto images = voilut::collect_images(dir);
        if (images.empty())
        {
            report_error(state, std::format("目录中没有可加载的图像：{}", dir));
            return;
        }
        state->playlist = images;
        state->playlist_index = 0;
        load_playlist_item(state, 0);
    }

    // ------------------------------------------------------------
    // 目录选择对话框（Windows 用 COM，Linux 用 zenity）
    // ------------------------------------------------------------

#if defined(_WIN32)
    /// @brief 宽字符 → UTF-8。
    std::string wide_to_utf8(const wchar_t *text)
    {
        if (text == nullptr)
        {
            return {};
        }
        const int size =
            ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(size - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
        return result;
    }

    /// @brief Windows：用 IFileOpenDialog 选择目录。
    std::string choose_directory_dialog_impl()
    {
        std::string result;
        const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool uninit = SUCCEEDED(init);

        IFileOpenDialog *dialog = nullptr;
        if (SUCCEEDED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&dialog))))
        {
            DWORD options = 0;
            if (SUCCEEDED(dialog->GetOptions(&options)))
            {
                dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            }
            const wchar_t kTitle[] = L"选择图像目录";
            dialog->SetTitle(kTitle);
            if (SUCCEEDED(dialog->Show(nullptr)))
            {
                IShellItem *item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)))
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                    {
                        result = wide_to_utf8(path);
                        ::CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }
        if (uninit)
        {
            ::CoUninitialize();
        }
        return result;
    }
#elif defined(__APPLE__)
    /// @brief macOS：调用系统自带的 AppleScript 目录选择框（无需 zenity）。
    /// @details `choose folder` 返回 alias，`POSIX path of` 转成 `/Users/...` 形式；
    ///          用户取消时 osascript 以非零状态退出且没有输出，按空结果处理。
    std::string choose_directory_dialog_impl()
    {
        FILE *pipe =
            popen("osascript -e 'POSIX path of (choose folder with prompt "
                  "\"选择图像目录\")' 2>/dev/null",
                  "r");
        if (pipe == nullptr)
        {
            return {};
        }
        std::string result;
        char buffer[4096];
        while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
        {
            result += buffer;
        }
        (void)pclose(pipe);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        {
            result.pop_back();
        }
        return result;
    }
#else
    /// @brief Linux：调用 zenity 选择目录。
    std::string choose_directory_dialog_impl()
    {
        FILE *pipe = popen("zenity --file-selection --directory --title='选择图像目录' 2>/dev/null",
                           "r");
        if (pipe == nullptr)
        {
            return {};
        }
        std::string result;
        char buffer[4096];
        while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
        {
            result += buffer;
        }
        const int status = pclose(pipe);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        {
            result.pop_back();
        }
        // 退出码 127 = 命令不存在；用户取消时 zenity 返回 1，不应报错。
        if (result.empty() && WIFEXITED(status) && WEXITSTATUS(status) == 127)
        {
            std::println("警告: 未找到 zenity，无法打开目录选择框；"
                         "请安装 zenity，或把图像 / 目录直接拖入窗口。");
        }
        return result;
    }
#endif

    /// @brief 打开目录选择框并加载其中的图像。
    void open_folder(AppState *state)
    {
        const std::string dir = choose_directory_dialog_impl();
        if (!dir.empty())
        {
            open_directory(state, dir);
        }
    }

    /// @brief 顶部主菜单栏：文件 / 图像 / 视图。

    std::vector<std::string> build_playlist(const std::vector<std::string> &args)
    {
        std::vector<std::string> list;
        for (const std::string &arg : args)
        {
            std::error_code ec;
            if (std::filesystem::is_directory(arg, ec) && !ec)
            {
                const auto images = voilut::collect_images(arg);
                list.insert(list.end(), images.begin(), images.end());
            }
            else if (voilut::is_image_file(arg))
            {
                const std::filesystem::path absolute = std::filesystem::absolute(arg, ec);
                list.push_back(voilut::path_display(ec ? std::filesystem::path(arg) : absolute));
            }
            else
            {
                std::println("跳过无法识别的参数：{}", arg);
            }
        }
        return list;
    }
} // namespace voilut::app
