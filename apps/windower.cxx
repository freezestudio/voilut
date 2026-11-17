// ============================================================
// windower.cxx —— voilut 的调窗应用入口（桌面端 + Dear ImGui）
// ============================================================
// 核心功能：16 位影像的窗宽窗位（Window Level / Window Width）调节。
// 调窗是这个程序的核心交互，其他功能（缩放 / 平移 / 旋转 / 探针）都为它服务。
//
// 交互：
//   左键拖动     框选 ROI，按框内 p1/p99 实时调窗 —— 核心交互
//   Shift+左键   手动调窗（水平 = 窗宽，垂直 = 窗位）
//   右键拖动     平移
//   滚轮         缩放（Ctrl = 窗宽，Shift = 窗位）
//   ← →          窗位微调（按住 Ctrl 时步进更小）
//   ↑ ↓ 或 + -   窗宽微调（按住 Ctrl 时步进更小）
//   1..6         预设窗
//   0            重置窗（DICOM 默认 → p1/p99 自动窗）
//   F 适应窗口 / I 反色 / L 双线性 / R 旋转 / H 水平翻转 / V 垂直翻转
//   [ ] 切换图像 / Esc 退出
//   重置图像     清除窗值、反色、插值、方向、视口与 ROI
//   操作记录     保存当前 ROI 的原图坐标与窗值
//   菜单栏       文件 / 图像 / 视图；视图菜单可显隐面板
//
// 流程：
//   1) 解析命令行参数（图像 / 目录）并解码首张
//   2) 创建 GLFW 窗口 + WebGPU 环境 + 渲染管线 + ImGui
//   3) 主循环：事件 → UI（调窗）→ 图像渲染通道 + ImGui 渲染通道 → Present
// ============================================================

#if defined(_WIN32)
// min/max 宏会破坏标准库与 ImGui，必须在使用 <windows.h> 之前定义。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// 先包含 <windows.h>，避免 GLFW 先行定义 APIENTRY 后与 Windows SDK 冲突（C4005）。
#include <windows.h>
#include <shobjidl.h> // IFileOpenDialog（目录选择）
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_wgpu.h>

#include "context.h"
#include "decode/image_decode.h"
#include "image_model.h"
#include "image_renderer.h"
#include "image_source.h"
#include "orientation.h"
#include "viewport.h"
#include "window.h"

namespace
{
    // ------------------------------------------------------------
    // 状态
    // ------------------------------------------------------------

    /// @brief 已记录的 ROI（原图坐标，缩放 / 平移 / 旋转后仍有效）。
    struct RoiRecord
    {
        std::size_t id = 0;
        std::string image_name;
        std::string image_path;
        voilut::Rect rect;
        voilut::WindowSetting window;
    };

    /// @brief 应用整体状态（回调通过 GLFW 的 user pointer 获取它）。
    struct AppState
    {
        GLFWwindow *window = nullptr;         ///< GLFW 窗口。
        voilut::Context *context = nullptr;   ///< WebGPU 环境（不拥有）。
        voilut::ImageRenderer *renderer = nullptr; ///< 渲染管线（不拥有）。
        voilut::DecodedImage *image = nullptr;     ///< 已加载图像（不拥有）。

        std::string current_path;                  ///< 当前文件路径。
        voilut::WindowSetting window_setting{};    ///< 当前窗（核心状态）。
        voilut::Histogram histogram{};             ///< 当前图像的显示直方图。
        bool invert = false;                       ///< 是否反色。
        bool linear = false;                       ///< 是否双线性插值。
        voilut::Orientation orientation{};         ///< 显示方向。
        voilut::Viewport viewport{};               ///< 视口（缩放 / 平移）。

        double viewport_width = 0.0;  ///< 视口宽（像素）。
        double viewport_height = 0.0; ///< 视口高（像素）。

        // 滑块范围（由图像统计得到）。
        double level_min = 0.0;
        double level_max = 65535.0;
        double width_min = 1.0;
        double width_max = 65535.0;

        // 拖动调窗。
        bool wl_dragging = false;         ///< 是否正在拖动调窗。
        double wl_start_x = 0.0;          ///< 拖动起点（屏幕 x）。
        double wl_start_y = 0.0;          ///< 拖动起点（屏幕 y）。
        voilut::WindowSetting wl_base{};  ///< 拖动起点处的窗（按位移换算，避免累积误差）。

        // 平移。
        bool pan_dragging = false; ///< 是否正在右键平移。
        double pan_start_x = 0.0;  ///< 平移起点（屏幕 x）。
        double pan_start_y = 0.0;  ///< 平移起点（屏幕 y）。

        // ROI 框选（左键）：原图坐标，缩放 / 平移 / 旋转后仍能跟随。
        bool roi_active = false;   ///< 是否正在框选。
        bool roi_done = false;     ///< 是否已完成一次框选。
        bool roi_windowed = false; ///< 当前窗是否来自 ROI 统计。
        double roi_start_x = 0.0;  ///< 框选起点（原图坐标）。
        double roi_start_y = 0.0;
        voilut::Rect roi_image{};  ///< 当前 ROI（原图坐标）。

        // 探针。
        bool hover_valid = false;
        int hover_x = 0;
        int hover_y = 0;
        uint16_t hover_value = 0;

        // 图像列表（同目录图像）。
        std::vector<std::string> playlist;
        std::size_t playlist_index = 0;

        // 操作记录：已保存的 ROI 及其窗值。
        std::vector<RoiRecord> roi_records;
        std::size_t next_roi_id = 1;

        // 多页 / 多帧文件的当前帧（0-based）。
        uint32_t frame_index = 0;

        // 面板显隐（菜单栏可切换）。
        bool show_control_panel = true;
        bool show_history_panel = true;

        // 会话内的「每张图像记住窗值」。
        std::unordered_map<std::string, voilut::WindowSetting> remembered;
        bool remember_window = true;

        // 显示与 UI。
        voilut::VoiMode voi_mode = voilut::VoiMode::Simplified; ///< 映射公式（简化 / DICOM）。
        float ui_scale = 1.0f;                                  ///< HiDPI 缩放（按显示内容比例）。
        std::string error_message;                              ///< 最近一次错误的说明。
        bool error_pending = false;                             ///< 是否有待弹出的错误。

        bool quit_requested = false; ///< 退出请求（Esc / 关闭按钮）。
    };

    // ------------------------------------------------------------
    // 辅助函数
    // ------------------------------------------------------------

    /// @brief 把 96 DPI 下的设计尺寸换算成当前 UI 像素。
    /// @note 布局常量（面板宽、边距、固定列宽等）必须经过这里，才能在
    ///       高 DPI 下与 ImGui 样式、字体保持同一比例。
    [[nodiscard]] float ui_px(const AppState *state, float value) noexcept
    {
        return value * state->ui_scale;
    }

    /// @brief 查询窗口所在显示器的 UI 缩放。
    /// @param window 已创建的 GLFW 窗口。
    /// @return 缩放倍数（1.0 = 96 DPI）；macOS 由 framebuffer scale 处理，返回 1.0。
    [[nodiscard]] float window_ui_scale(GLFWwindow *window) noexcept
    {
#if defined(__APPLE__) || defined(__EMSCRIPTEN__) || defined(__ANDROID__)
        (void)window;
        return 1.0f;
#else
        float xscale = 1.0f;
        float yscale = 1.0f;
        glfwGetWindowContentScale(window, &xscale, &yscale);
        return std::max({1.0f, xscale, yscale});
#endif
    }

    /// @brief 显示上一个控件的工具提示（按需渲染下立即显示，不等待延迟）。
    void item_tooltip(const char *text)
    {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", text);
        }
    }

    /// @brief 菜单项：激活后补一帧，让菜单关闭和勾选状态立即刷新。
    bool menu_item(const char *label, const char *shortcut = nullptr, bool selected = false)
    {
        if (ImGui::MenuItem(label, shortcut, selected))
        {
            glfwPostEmptyEvent();
            return true;
        }
        return false;
    }

    /// @brief 可勾选菜单项：切换布尔值后补一帧刷新。
    void menu_item_toggle(const char *label, bool *value)
    {
        if (ImGui::MenuItem(label, nullptr, value))
        {
            glfwPostEmptyEvent();
        }
    }

    /// @brief 把当前窗写入渲染器（唯一的 set_window 调用入口，保证状态一致）。
    void apply_window(AppState *state)
    {
        if (state->renderer == nullptr)
        {
            return;
        }
        // 窗位限制在「数据范围 + 5% 余量」内，窗宽限制在 16 位合法区间。
        // 注意窗宽上界不限制到滑块范围：DICOM 自带的窗宽可能比实际值域宽，
        // 限制得过窄会把窗设成完全不同的对比度。
        state->window_setting.level =
            std::clamp(state->window_setting.level, state->level_min, state->level_max);
        state->window_setting.width = std::clamp(state->window_setting.width, 1.0, 65535.0);
        state->renderer->set_window(state->window_setting.level, state->window_setting.width);
    }

    /// @brief 记录并弹出一次错误（同时保留 stdout 日志）。
    void report_error(AppState *state, std::string message)
    {
        std::println("错误: {}", message);
        state->error_message = std::move(message);
        state->error_pending = true;
        if (state->window != nullptr)
        {
            glfwPostEmptyEvent();
        }
    }

    /// @brief 切换多页 / 多帧文件的当前帧（定义在 apply_image 之后）。
    void set_frame(AppState *state, uint32_t frame);

    /// @brief 适应视口（等比缩放并居中）。
    void fit_to_window(AppState *state)
    {
        if (state->image == nullptr)
        {
            return;
        }
        const auto [display_width, display_height] = voilut::display_size(
            state->image->meta.width, state->image->meta.height, state->orientation);
        state->viewport = voilut::compute_fit(display_width, display_height,
                                              state->viewport_width, state->viewport_height);
        state->renderer->set_transform(state->viewport);
    }

    /// @brief 显示坐标 → 原图坐标（与着色器的 UV 变换互逆）。
    std::pair<double, double> display_to_original(const AppState *state, double x, double y)
    {
        const auto [display_width, display_height] = voilut::display_size(
            state->image->meta.width, state->image->meta.height, state->orientation);
        if (display_width == 0 || display_height == 0)
        {
            return {0.0, 0.0};
        }
        const double u = x / static_cast<double>(display_width);
        const double v = y / static_cast<double>(display_height);
        const auto [original_u, original_v] = voilut::display_uv_to_image_uv(u, v, state->orientation);
        return {original_u * static_cast<double>(state->image->meta.width),
                original_v * static_cast<double>(state->image->meta.height)};
    }

    /// @brief 屏幕坐标 → 原图坐标（组合视口变换与方向变换）。
    std::pair<double, double> screen_to_original(const AppState *state, double x, double y)
    {
        if (state->image == nullptr)
        {
            return {0.0, 0.0};
        }
        const auto [display_x, display_y] = voilut::screen_to_image(state->viewport, x, y);
        return display_to_original(state, display_x, display_y);
    }

    /// @brief 原图坐标 → 屏幕坐标（组合方向变换与视口变换）。
    std::pair<double, double> original_to_screen(const AppState *state, double x, double y)
    {
        if (state->image == nullptr || state->image->meta.width == 0 ||
            state->image->meta.height == 0)
        {
            return {0.0, 0.0};
        }
        const auto [display_width, display_height] = voilut::display_size(
            state->image->meta.width, state->image->meta.height, state->orientation);
        const double u = x / static_cast<double>(state->image->meta.width);
        const double v = y / static_cast<double>(state->image->meta.height);
        const auto [display_u, display_v] =
            voilut::image_uv_to_display_uv(u, v, state->orientation);
        return {
            state->viewport.x +
                display_u * static_cast<double>(display_width) * state->viewport.scale,
            state->viewport.y +
                display_v * static_cast<double>(display_height) * state->viewport.scale,
        };
    }

    /// @brief 把方向应用到渲染器并重新计算适应变换。
    void apply_orientation(AppState *state)
    {
        state->renderer->set_orientation(state->orientation);
        fit_to_window(state);
    }

    /// @brief 重置窗：优先用 DICOM 自带窗，否则用 p1/p99 自动窗。
    void reset_window(AppState *state)
    {
        if (state->image == nullptr)
        {
            return;
        }
        state->window_setting = state->image->default_window
                                    ? *state->image->default_window
                                    : voilut::auto_window(state->image->meta);
        state->roi_windowed = false;
        apply_window(state);
    }

    /// @brief 重置图像：清除窗值、反色、插值、方向、视口与 ROI，恢复打开时的状态。
    void reset_image(AppState *state)
    {
        if (state->image == nullptr)
        {
            return;
        }
        // 当前图像不再保留会话内的窗值记忆。
        state->remembered.erase(state->current_path);
        // 重置图像同时回到首帧（恢复打开时的状态）。
        if (state->frame_index != 0)
        {
            set_frame(state, 0);
        }

        state->invert = state->image->initial_invert;
        state->linear = false;
        state->orientation = {};
        state->viewport = {};

        state->wl_dragging = false;
        state->pan_dragging = false;
        state->roi_active = false;
        state->roi_done = false;
        state->roi_windowed = false;
        state->roi_start_x = 0.0;
        state->roi_start_y = 0.0;
        state->roi_image = {};

        state->renderer->set_invert(state->invert);
        state->renderer->set_interpolation(state->linear);
        state->renderer->set_orientation(state->orientation);
        reset_window(state);
        fit_to_window(state);
    }

    /// @brief 应用预设窗。
    void apply_preset(AppState *state, std::size_t index)
    {
        if (state->image == nullptr || index >= voilut::kWindowPresets.size())
        {
            return;
        }
        state->window_setting = voilut::preset_window(state->image->meta,
                                                      voilut::kWindowPresets[index]);
        state->roi_windowed = false;
        apply_window(state);
    }

    /// @brief 微调窗：`level_delta` / `width_factor` 允许只提供其中一个。
    void nudge_window(AppState *state, double level_delta, double width_factor)
    {
        state->window_setting.width *= width_factor;
        state->window_setting.level += level_delta;
        state->roi_windowed = false;
        apply_window(state);
    }

    /// @brief 从框选起点和当前鼠标位置更新原图坐标 ROI。
    void update_roi_drag(AppState *state, double screen_x, double screen_y)
    {
        if (state->image == nullptr)
        {
            return;
        }
        const auto [image_x, image_y] = screen_to_original(state, screen_x, screen_y);
        const double image_width = static_cast<double>(state->image->meta.width);
        const double image_height = static_cast<double>(state->image->meta.height);
        const double x0 = std::clamp(std::min(state->roi_start_x, image_x), 0.0, image_width);
        const double y0 = std::clamp(std::min(state->roi_start_y, image_y), 0.0, image_height);
        const double x1 = std::clamp(std::max(state->roi_start_x, image_x), 0.0, image_width);
        const double y1 = std::clamp(std::max(state->roi_start_y, image_y), 0.0, image_height);
        state->roi_image = {
            .x = x0,
            .y = y0,
            .width = x1 - x0,
            .height = y1 - y0,
        };
    }

    /// @brief 按当前 ROI 统计 p1/p99 并设窗（ROI 窗）。
    /// @return ROI 有效且已应用时返回 true。
    bool apply_roi_window(AppState *state)
    {
        if (state->image == nullptr || state->roi_image.width < 1.0 ||
            state->roi_image.height < 1.0)
        {
            return false;
        }
        const auto setting = voilut::window_for_roi(
            state->image->frame_pixels(state->frame_index), state->image->meta.width,
            state->image->meta.height, state->roi_image);
        if (!setting)
        {
            return false; // 选区太小，保持当前窗
        }
        state->window_setting = *setting;
        state->roi_windowed = true;
        apply_window(state);
        return true;
    }

    /// @brief 把当前 ROI 及其窗值写入操作记录。
    void record_current_roi(AppState *state)
    {
        if (state->image == nullptr || !state->roi_done || state->roi_image.width <= 0.0 ||
            state->roi_image.height <= 0.0)
        {
            return;
        }
        state->roi_records.push_back({
            .id = state->next_roi_id++,
            .image_name = state->image->name,
            .image_path = state->current_path,
            .rect = state->roi_image,
            .window = state->window_setting,
        });
    }

    /// @brief 更新窗口标题（多帧时带帧号）。
    void update_window_title(AppState *state)
    {
        if (state->window == nullptr || state->image == nullptr)
        {
            return;
        }
        std::string title = std::format("Windower — {}", state->image->name);
        const uint32_t count = state->image->meta.frame_count;
        if (count > 1)
        {
            title += std::format("（帧 {}/{}）", state->frame_index + 1, count);
        }
        glfwSetWindowTitle(state->window, title.c_str());
    }

    /// @brief 把解码结果应用到状态与渲染器。
    void apply_image(AppState *state, voilut::DecodedImage image, const std::string &path)
    {
        // 先保存上一张的窗值。
        if (state->remember_window && state->image != nullptr && !state->current_path.empty())
        {
            state->remembered[state->current_path] = state->window_setting;
        }

        // 先上传纹理：失败时保留当前图像，并向用户报告原因（例如超过纹理上限）。
        if (state->renderer != nullptr)
        {
            if (auto uploaded = state->renderer->upload(image.meta.width, image.meta.height,
                                                        image.frame_pixels(0));
                !uploaded)
            {
                report_error(state, uploaded.error());
                return;
            }
        }

        *state->image = std::move(image);
        state->current_path = path;
        state->frame_index = 0;

        const voilut::ImageMeta &meta = state->image->meta;
        state->level_min = meta.min_value - meta.value_range * 0.05;
        state->level_max = meta.max_value + meta.value_range * 0.05;
        state->width_max = std::max(meta.value_range, 2.0);
        state->invert = state->image->initial_invert;
        state->roi_active = false;
        state->roi_done = false;
        state->roi_windowed = false;
        state->roi_start_x = 0.0;
        state->roi_start_y = 0.0;
        state->roi_image = {};

        state->renderer->set_invert(state->invert);
        state->renderer->set_interpolation(state->linear);
        state->renderer->set_orientation(state->orientation);

        // 直方图：只在新图加载时计算一次（首帧，[min, max] 上的 256 桶）。
        state->histogram = voilut::compute_histogram(
            state->image->frame_pixels(0), meta.min_value,
            std::max(meta.max_value, meta.min_value + 1.0));

        // 窗值：优先恢复该图的上次窗值，否则 DICOM 默认窗 → p1/p99 自动窗。
        const auto it = state->remembered.find(path);
        if (it != state->remembered.end())
        {
            state->window_setting = it->second;
            apply_window(state);
        }
        else
        {
            reset_window(state);
        }
        fit_to_window(state);

        if (state->window != nullptr)
        {
            update_window_title(state);
        }
        std::println("已加载 {}（{}x{}，{} bit，值域 {} ~ {}）", state->image->name, meta.width,
                     meta.height, meta.bits_per_sample, static_cast<long long>(meta.min_value),
                     static_cast<long long>(meta.max_value));
        if (meta.frame_count > 1)
        {
            std::println("多帧/多页文件：共 {} 帧，可用 , / . 或面板滑块切换", meta.frame_count);
        }
    }

    /// @brief 切换多页 / 多帧文件的当前帧（保持窗值，重算该帧统计与直方图）。
    void set_frame(AppState *state, uint32_t frame)
    {
        if (state->image == nullptr)
        {
            return;
        }
        const uint32_t count = state->image->meta.frame_count;
        if (count <= 1)
        {
            return;
        }
        frame %= count;
        if (frame == state->frame_index)
        {
            return;
        }

        const uint32_t width = state->image->meta.width;
        const uint32_t height = state->image->meta.height;
        const auto pixels = state->image->frame_pixels(frame);
        if (auto uploaded = state->renderer->upload(width, height, pixels); !uploaded)
        {
            report_error(state, uploaded.error());
            return;
        }
        state->frame_index = frame;

        // 帧统计用于直方图与滑块范围；窗值保持不变（z-stack / 时间序列对比时需要一致）。
        const voilut::ImageStats stats = voilut::compute_stats(pixels);
        voilut::ImageMeta &meta = state->image->meta;
        meta.min_value = stats.min_value;
        meta.max_value = stats.max_value;
        meta.p1 = stats.p1;
        meta.p99 = stats.p99;
        meta.value_range = stats.value_range;
        state->level_min = stats.min_value - stats.value_range * 0.05;
        state->level_max = stats.max_value + stats.value_range * 0.05;
        state->width_max = std::max(stats.value_range, 2.0);
        state->histogram = voilut::compute_histogram(
            pixels, stats.min_value, std::max(stats.max_value, stats.min_value + 1.0));

        apply_window(state);
        update_window_title(state);
        std::println("当前帧 {}/{}（值域 {} ~ {}）", frame + 1, count,
                     static_cast<long long>(stats.min_value),
                     static_cast<long long>(stats.max_value));
    }

    /// @brief 解码并显示图像列表中的第 `index` 项。
    void load_playlist_item(AppState *state, std::size_t index)
    {
        if (state->playlist.empty())
        {
            return;
        }
        const std::size_t count = state->playlist.size();
        state->playlist_index = index % count;
        const std::string &path = state->playlist[state->playlist_index];

        const auto kind = voilut::kind_from_name(path);
        if (!kind)
        {
            report_error(state, std::format("无法识别的图像格式：{}", path));
            return;
        }
        auto decoded = voilut::decode_image(path, *kind);
        if (!decoded)
        {
            report_error(state, std::format("解码失败：{}", decoded.error()));
            return;
        }
        apply_image(state, std::move(decoded.value()), path);
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
#else
    /// @brief Linux/macOS：调用 zenity 选择目录。
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
        pclose(pipe);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        {
            result.pop_back();
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
    void draw_menu_bar(AppState *state)
    {
        if (!ImGui::BeginMainMenuBar())
        {
            return;
        }

        const bool has_image = state->image != nullptr;

        if (ImGui::BeginMenu("文件"))
        {
            if (menu_item("打开文件夹...", "Ctrl+O"))
            {
                open_folder(state);
            }
            item_tooltip("选择包含 TIFF / DICOM 图像的目录");

            ImGui::Separator();
            ImGui::BeginDisabled(state->playlist.empty());
            if (menu_item("上一张", "["))
            {
                step_playlist(state, -1);
            }
            item_tooltip("切换到图像列表中的上一张");
            if (menu_item("下一张", "]"))
            {
                step_playlist(state, 1);
            }
            item_tooltip("切换到图像列表中的下一张");
            ImGui::EndDisabled();

            ImGui::Separator();
            if (menu_item("退出", "Ctrl+Q"))
            {
                state->quit_requested = true;
                glfwSetWindowShouldClose(state->window, GLFW_TRUE);
            }
            item_tooltip("关闭 Windower");
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("图像"))
        {
            ImGui::BeginDisabled(!has_image);
            if (menu_item("重置图像", "Ctrl+R"))
            {
                reset_image(state);
            }
            item_tooltip("清除窗值、反色、插值、方向、视口和 ROI");

            if (menu_item("重置窗", "0"))
            {
                reset_window(state);
            }
            item_tooltip("只恢复 DICOM 默认窗或 p1/p99 自动窗");

            if (menu_item("自动窗 p1/p99"))
            {
                state->window_setting = voilut::auto_window(state->image->meta);
                state->roi_windowed = false;
                apply_window(state);
            }
            item_tooltip("按整幅图像的 1% / 99% 分位数计算窗");

            const bool has_default_window =
                state->image != nullptr && state->image->default_window.has_value();
            ImGui::BeginDisabled(!has_default_window);
            if (menu_item("DICOM 默认窗"))
            {
                state->window_setting = *state->image->default_window;
                state->roi_windowed = false;
                apply_window(state);
            }
            item_tooltip("使用文件自带的 WindowCenter / WindowWidth");
            ImGui::EndDisabled();

            ImGui::Separator();
            if (ImGui::BeginMenu("预设窗"))
            {
                for (std::size_t i = 0; i < voilut::kWindowPresets.size(); ++i)
                {
                    const std::string shortcut = std::to_string(i + 1);
                    if (menu_item(voilut::kWindowPresets[i].name, shortcut.c_str()))
                    {
                        apply_preset(state, i);
                    }
                    item_tooltip("按图像值域比例应用预设窗");
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            if (menu_item("适应窗口", "F"))
            {
                fit_to_window(state);
            }
            item_tooltip("等比缩放并居中显示整幅图像");

            if (menu_item("旋转 90°", "R"))
            {
                state->orientation = voilut::rotate_cw(state->orientation);
                apply_orientation(state);
            }
            item_tooltip("顺时针旋转 90°");

            if (menu_item("水平翻转", "H"))
            {
                state->orientation.flip_h = !state->orientation.flip_h;
                apply_orientation(state);
            }
            item_tooltip("左右翻转显示方向");

            if (menu_item("垂直翻转", "V"))
            {
                state->orientation.flip_v = !state->orientation.flip_v;
                apply_orientation(state);
            }
            item_tooltip("上下翻转显示方向");

            ImGui::Separator();
            if (menu_item("反色", "I", state->invert))
            {
                state->invert = !state->invert;
                state->renderer->set_invert(state->invert);
            }
            item_tooltip("反转显示灰度");

            if (menu_item("双线性", "L", state->linear))
            {
                state->linear = !state->linear;
                state->renderer->set_interpolation(state->linear);
            }
            item_tooltip("放大时使用双线性插值");

            if (menu_item("DICOM 精确 VOI", nullptr, state->voi_mode == voilut::VoiMode::Dicom))
            {
                state->voi_mode = state->voi_mode == voilut::VoiMode::Dicom
                                      ? voilut::VoiMode::Simplified
                                      : voilut::VoiMode::Dicom;
                state->renderer->set_voi_mode(state->voi_mode);
            }
            item_tooltip("切换窗值→灰度公式：简化（W 分母）/ DICOM 精确（W-1 分母，C.11.2.1.2.1）");

            if (menu_item("记住窗值", nullptr, state->remember_window))
            {
                state->remember_window = !state->remember_window;
            }
            item_tooltip("在当前会话内按图像路径记住窗值");
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("视图"))
        {
            menu_item_toggle("窗宽窗位面板", &state->show_control_panel);
            item_tooltip("显示或隐藏主调窗面板");
            menu_item_toggle("操作记录面板", &state->show_history_panel);
            item_tooltip("显示或隐藏 ROI 操作记录面板");

            ImGui::Separator();
            if (menu_item("显示全部面板"))
            {
                state->show_control_panel = true;
                state->show_history_panel = true;
            }
            item_tooltip("同时显示窗宽窗位面板和操作记录面板");
            if (menu_item("隐藏全部面板"))
            {
                state->show_control_panel = false;
                state->show_history_panel = false;
            }
            item_tooltip("隐藏全部面板，仅保留图像和菜单栏");
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }

    // ------------------------------------------------------------
    // 回调
    // ------------------------------------------------------------

    void FramebufferSizeCallback(GLFWwindow *window, int width, int height)
    {
        auto *state = static_cast<AppState *>(glfwGetWindowUserPointer(window));
        if (state == nullptr || width <= 0 || height <= 0)
        {
            return;
        }
        state->context->ConfigureSurface(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        state->viewport_width = static_cast<double>(width);
        state->viewport_height = static_cast<double>(height);
        state->renderer->resize(state->viewport_width, state->viewport_height);
        fit_to_window(state);
    }

    void ScrollCallback(GLFWwindow *window, double /*xoffset*/, double yoffset)
    {
        auto *state = static_cast<AppState *>(glfwGetWindowUserPointer(window));
        if (state == nullptr || state->image == nullptr)
        {
            return;
        }
        const bool ctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                          glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
        const bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                           glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;

        if (ctrl)
        {
            // Ctrl+滚轮：窗宽（指数缩放，操作灵敏度均匀）。
            nudge_window(state, 0.0, std::pow(1.1, yoffset));
            return;
        }
        if (shift)
        {
            // Shift+滚轮：窗位（按窗宽比例步进）。
            nudge_window(state, yoffset * state->window_setting.width * 0.02, 1.0);
            return;
        }

        // 默认：以光标为锚点缩放。
        double mouse_x = 0.0;
        double mouse_y = 0.0;
        glfwGetCursorPos(window, &mouse_x, &mouse_y);
        state->viewport = voilut::zoom_at(state->viewport, mouse_x, mouse_y,
                                          std::pow(voilut::kWheelFactor, yoffset));
        state->renderer->set_transform(state->viewport);
    }

    void CursorPosCallback(GLFWwindow *window, double x, double y)
    {
        auto *state = static_cast<AppState *>(glfwGetWindowUserPointer(window));
        if (state == nullptr || state->image == nullptr)
        {
            return;
        }
        if (state->wl_dragging)
        {
            // 从拖动起点的窗按总位移换算：不会累积，松开后再次拖动不会跳变。
            voilut::WindowSetting dragged = state->wl_base;
            state->window_setting =
                voilut::drag_window(dragged, x - state->wl_start_x, y - state->wl_start_y);
            state->roi_windowed = false;
            apply_window(state);
        }
        if (state->pan_dragging)
        {
            state->viewport = voilut::pan(state->viewport, x - state->pan_start_x,
                                          y - state->pan_start_y);
            state->pan_start_x = x;
            state->pan_start_y = y;
            state->renderer->set_transform(state->viewport);
        }
        if (state->roi_active)
        {
            update_roi_drag(state, x, y);
            apply_roi_window(state);
        }
    }

    void MouseButtonCallback(GLFWwindow *window, int button, int action, int mods)
    {
        auto *state = static_cast<AppState *>(glfwGetWindowUserPointer(window));
        if (state == nullptr || state->image == nullptr)
        {
            return;
        }
        // UI 正在使用鼠标时不拦截事件（单击面板不会改变窗）。
        ImGuiIO &io = ImGui::GetIO();
        if (io.WantCaptureMouse && action == GLFW_PRESS)
        {
            return;
        }

        double x = 0.0;
        double y = 0.0;
        glfwGetCursorPos(window, &x, &y);

        if (button == GLFW_MOUSE_BUTTON_LEFT)
        {
            if (action == GLFW_PRESS)
            {
                if ((mods & GLFW_MOD_SHIFT) != 0)
                {
                    // Shift+左键：手动调窗（水平 = 窗宽，垂直 = 窗位）。
                    state->wl_dragging = true;
                    state->wl_start_x = x;
                    state->wl_start_y = y;
                    state->wl_base = state->window_setting;
                }
                else
                {
                    // 左键：框选 ROI（核心交互，实时按框内统计调窗）。
                    state->roi_active = true;
                    state->roi_done = false;
                    const auto [image_x, image_y] = screen_to_original(state, x, y);
                    const double image_width = static_cast<double>(state->image->meta.width);
                    const double image_height = static_cast<double>(state->image->meta.height);
                    state->roi_start_x = std::clamp(image_x, 0.0, image_width);
                    state->roi_start_y = std::clamp(image_y, 0.0, image_height);
                    state->roi_image = {
                        .x = state->roi_start_x,
                        .y = state->roi_start_y,
                        .width = 0.0,
                        .height = 0.0,
                    };
                }
            }
            else if (action == GLFW_RELEASE)
            {
                if (state->roi_active)
                {
                    state->roi_done = apply_roi_window(state);
                    state->roi_active = false;
                }
                state->wl_dragging = false;
            }
        }
        else if (button == GLFW_MOUSE_BUTTON_RIGHT)
        {
            if (action == GLFW_PRESS)
            {
                state->pan_dragging = true;
                state->pan_start_x = x;
                state->pan_start_y = y;
            }
            else if (action == GLFW_RELEASE)
            {
                state->pan_dragging = false;
            }
        }
    }

    void KeyCallback(GLFWwindow *window, int key, int /*scancode*/, int action, int mods)
    {
        if (action != GLFW_PRESS && action != GLFW_REPEAT)
        {
            return;
        }
        auto *state = static_cast<AppState *>(glfwGetWindowUserPointer(window));
        if (state == nullptr)
        {
            return;
        }
        if (action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL) != 0)
        {
            switch (key)
            {
            case GLFW_KEY_O:
                open_folder(state);
                return;
            case GLFW_KEY_R:
                reset_image(state);
                return;
            case GLFW_KEY_Q:
                state->quit_requested = true;
                glfwSetWindowShouldClose(window, GLFW_TRUE);
                return;
            default:
                break;
            }
        }
        // Ctrl 按下时步长更小，便于在窄窗下微调。
        const double fine = (mods & GLFW_MOD_CONTROL) != 0 ? 0.2 : 1.0;
        const double level_step = std::max(state->window_setting.width * 0.02 * fine, 1.0);

        switch (key)
        {
        case GLFW_KEY_Q:
        case GLFW_KEY_ESCAPE:
            state->quit_requested = true;
            glfwSetWindowShouldClose(window, GLFW_TRUE);
            break;
        case GLFW_KEY_F:
            fit_to_window(state);
            break;
        case GLFW_KEY_I:
            state->invert = !state->invert;
            state->renderer->set_invert(state->invert);
            break;
        case GLFW_KEY_L:
            state->linear = !state->linear;
            state->renderer->set_interpolation(state->linear);
            break;
        case GLFW_KEY_0:
            reset_window(state);
            break;
        case GLFW_KEY_1:
        case GLFW_KEY_2:
        case GLFW_KEY_3:
        case GLFW_KEY_4:
        case GLFW_KEY_5:
        case GLFW_KEY_6:
            apply_preset(state, static_cast<std::size_t>(key - GLFW_KEY_1));
            break;
        case GLFW_KEY_COMMA:
            if (state->image != nullptr && state->image->meta.frame_count > 1)
            {
                const uint32_t count = state->image->meta.frame_count;
                set_frame(state, (state->frame_index + count - 1) % count);
            }
            break;
        case GLFW_KEY_PERIOD:
            if (state->image != nullptr && state->image->meta.frame_count > 1)
            {
                set_frame(state, state->frame_index + 1);
            }
            break;
        case GLFW_KEY_LEFT:
            nudge_window(state, -level_step, 1.0);
            break;
        case GLFW_KEY_RIGHT:
            nudge_window(state, level_step, 1.0);
            break;
        case GLFW_KEY_UP:
        case GLFW_KEY_KP_ADD:
            nudge_window(state, 0.0, std::pow(1.05, 1.0 / fine));
            break;
        case GLFW_KEY_DOWN:
        case GLFW_KEY_KP_SUBTRACT:
            nudge_window(state, 0.0, std::pow(1.05, -1.0 / fine));
            break;
        case GLFW_KEY_LEFT_BRACKET:
            step_playlist(state, -1);
            break;
        case GLFW_KEY_RIGHT_BRACKET:
            step_playlist(state, 1);
            break;
        case GLFW_KEY_R:
            state->orientation = voilut::rotate_cw(state->orientation);
            apply_orientation(state);
            break;
        case GLFW_KEY_H:
            state->orientation.flip_h = !state->orientation.flip_h;
            apply_orientation(state);
            break;
        case GLFW_KEY_V:
            state->orientation.flip_v = !state->orientation.flip_v;
            apply_orientation(state);
            break;
        default:
            break;
        }
    }

    void DropCallback(GLFWwindow *window, int count, const char **paths)
    {
        auto *state = static_cast<AppState *>(glfwGetWindowUserPointer(window));
        if (state == nullptr || count < 1)
        {
            return;
        }
        std::error_code ec;
        if (std::filesystem::is_directory(paths[0], ec) && !ec)
        {
            open_directory(state, paths[0]);
            return;
        }
        open_path(state, paths[0]);
    }

    /// @brief 探针：把鼠标位置转换为原图坐标并读取像素值。
    void update_hover(AppState *state)
    {
        if (state->image == nullptr)
        {
            return;
        }
        double mouse_x = 0.0;
        double mouse_y = 0.0;
        glfwGetCursorPos(state->window, &mouse_x, &mouse_y);
        const auto [image_x, image_y] = screen_to_original(state, mouse_x, mouse_y);
        const auto value = voilut::pixel_at(
            state->image->frame_pixels(state->frame_index), state->image->meta.width,
            state->image->meta.height, image_x, image_y);
        if (value)
        {
            state->hover_valid = true;
            state->hover_x = static_cast<int>(std::floor(image_x));
            state->hover_y = static_cast<int>(std::floor(image_y));
            state->hover_value = *value;
        }
        else
        {
            state->hover_valid = false;
        }
    }

    /// @brief 为 ImGui 加载中文字体（按候选路径依次尝试）。
    void load_cjk_font()
    {
        struct FontCandidate
        {
            const char *path;
            int face;
        };
        static const FontCandidate kCandidates[] = {
            {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2},
            {"/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf", 0},
            {"/usr/share/fonts/truetype/arphic/uming.ttc", 0},
            {"C:\\Windows\\Fonts\\msyh.ttc", 0},
            {"C:\\Windows\\Fonts\\msyh.ttf", 0},
            {"C:\\Windows\\Fonts\\simhei.ttf", 0},
            {"C:\\Windows\\Fonts\\simsun.ttc", 0},
            {nullptr, 0},
        };
        ImGuiIO &io = ImGui::GetIO();
        for (int i = 0; kCandidates[i].path != nullptr; ++i)
        {
            std::ifstream probe(kCandidates[i].path, std::ios::binary);
            if (!probe.good())
            {
                continue;
            }
            ImFontConfig config;
            config.FontNo = kCandidates[i].face;
            if (io.Fonts->AddFontFromFileTTF(kCandidates[i].path, 16.0f, &config,
                                             io.Fonts->GetGlyphRangesChineseFull()) != nullptr)
            {
                return;
            }
        }
        io.Fonts->AddFontDefault();
    }

    // ------------------------------------------------------------
    // UI
    // ------------------------------------------------------------

    /// @brief 在直方图上绘制当前窗区间并叠加柱状图（单击 / 拖动可调窗位）。
    /// @param state 应用状态。
    /// @param height 控件高度。
    void draw_histogram(AppState *state, float height)
    {
        const float scale = state->ui_scale;
        const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 32.0f * scale), height);
        ImGui::InvisibleButton("##histogram", size);
        item_tooltip("在直方图上拖动可移动窗位；高亮为当前窗区间");
        const ImVec2 p0 = ImGui::GetItemRectMin();
        const ImVec2 p1 = ImGui::GetItemRectMax();

        ImDrawList *draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(p0, p1, IM_COL32(18, 18, 22, 255));

        const voilut::Histogram &hist = state->histogram;
        const double span = std::max(hist.high - hist.low, 1e-9);
        const auto value_to_x = [&](double value) {
            const double ratio = std::clamp((value - hist.low) / span, 0.0, 1.0);
            return p0.x + static_cast<float>(ratio) * (p1.x - p0.x);
        };

        if (state->image != nullptr && hist.peak > 0)
        {
            // 当前窗区间：半透明高亮，可直观看出「被映射到显示范围的是哪一段值域」。
            const double lower = state->window_setting.level - state->window_setting.width * 0.5;
            const double upper = state->window_setting.level + state->window_setting.width * 0.5;
            const float xa = value_to_x(lower);
            const float xb = value_to_x(upper);
            draw_list->AddRectFilled(ImVec2(xa, p0.y), ImVec2(xb, p1.y),
                                     IM_COL32(70, 140, 230, 45));

            // 柱状：用 sqrt 压缩动态范围，低计数区间也可显示。
            const float bar_width = (p1.x - p0.x) / static_cast<float>(voilut::kHistogramBins);
            for (uint32_t i = 0; i < voilut::kHistogramBins; ++i)
            {
                const uint32_t count = hist.bins[i];
                if (count == 0)
                {
                    continue;
                }
                const float ratio = static_cast<float>(count) / static_cast<float>(hist.peak);
                const float h = std::sqrt(ratio) * (p1.y - p0.y);
                const float x = p0.x + static_cast<float>(i) * bar_width;
                draw_list->AddRectFilled(ImVec2(x, p1.y - h), ImVec2(x + bar_width, p1.y),
                                         IM_COL32(150, 150, 160, 200));
            }

            // p1 / p99 参考线。
            const double p1_value = state->image->meta.p1;
            const double p99_value = state->image->meta.p99;
            draw_list->AddLine(ImVec2(value_to_x(p1_value), p0.y),
                               ImVec2(value_to_x(p1_value), p1.y), IM_COL32(255, 200, 80, 160),
                               1.0f * scale);
            draw_list->AddLine(ImVec2(value_to_x(p99_value), p0.y),
                               ImVec2(value_to_x(p99_value), p1.y), IM_COL32(255, 200, 80, 160),
                               1.0f * scale);

            // 窗位线。
            const float xc = value_to_x(state->window_setting.level);
            draw_list->AddLine(ImVec2(xc, p0.y), ImVec2(xc, p1.y), IM_COL32(120, 200, 255, 255),
                               1.5f * scale);
        }

        draw_list->AddRect(p0, p1, IM_COL32(80, 80, 90, 255));

        // 交互：在直方图上按住拖动 = 移动窗位（比拖动滑块更直接）。
        if (ImGui::IsItemActive() && state->image != nullptr)
        {
            const float mouse_x = ImGui::GetIO().MousePos.x;
            const float ratio =
                std::clamp((mouse_x - p0.x) / std::max(p1.x - p0.x, 1.0f), 0.0f, 1.0f);
            state->window_setting.level = hist.low + ratio * span;
            state->roi_windowed = false;
            apply_window(state);
        }
    }

    // 面板固定宽度要容纳 6 个预设按钮同排；顶部让出主菜单栏。
    // 这里是 96 DPI 下的设计值，实际布局用 ui_px() 乘以系统缩放。
    constexpr float kPanelWidth = 520.0f;
    constexpr float kPanelMargin = 12.0f;

    /// @brief 主控制面板。
    void draw_panel(AppState *state)
    {
        const float panel_top = ImGui::GetFrameHeight() + ui_px(state, 4.0f);
        const float panel_height =
            std::max(ui_px(state, 620.0f),
                     ImGui::GetIO().DisplaySize.y - panel_top - ui_px(state, 4.0f));
        ImGui::SetNextWindowPos(ImVec2(ui_px(state, kPanelMargin), panel_top), ImGuiCond_Always);
        // 面板覆盖在影像上，必须不透明，否则高亮区域背景会降低文字可读性。
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.09f, 0.11f, 1.0f));
        // 尺寸使用固定值：自动适应（高度为 0）要等到第二帧才生效，而本程序按需
        // 渲染（静止时阻塞在 glfwWaitEvents），首帧之后若无事件就不会重绘，
        // 面板会停在只有标题栏、高度异常的状态。
        // 用 Always 避免旧 imgui.ini 继续沿用旧尺寸，导致新布局被裁切。
        ImGui::SetNextWindowSize(ImVec2(ui_px(state, kPanelWidth), panel_height),
                                 ImGuiCond_Always);
        ImGui::Begin("Windower · 窗宽窗位");

        if (ImGui::Button("打开文件夹..."))
        {
            const std::string dir = choose_directory_dialog_impl();
            if (!dir.empty())
            {
                open_directory(state, dir);
            }
        }
        item_tooltip("选择包含 TIFF / DICOM 图像的目录");
        ImGui::SameLine();
        if (ImGui::Button("上一张 ["))
        {
            step_playlist(state, -1);
        }
        item_tooltip("切换到图像列表中的上一张");
        ImGui::SameLine();
        if (ImGui::Button("下一张 ]"))
        {
            step_playlist(state, 1);
        }
        item_tooltip("切换到图像列表中的下一张");
        if (!state->playlist.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%zu / %zu", state->playlist_index + 1, state->playlist.size());
        }

        if (state->image == nullptr)
        {
            ImGui::Separator();
            ImGui::TextWrapped("未加载图像。把 .tif / .tiff / .dcm 拖动到窗口，或单击上面的"
                               "「打开文件夹...」。");
            ImGui::PopStyleColor();
            ImGui::End();
            return;
        }

        const voilut::WindowSetting window_at_frame_start = state->window_setting;
        const voilut::ImageMeta &meta = state->image->meta;
        ImGui::Separator();
        ImGui::Text("%s", state->image->name.c_str());
        ImGui::TextDisabled("%u x %u  ·  %u bit  ·  值域 %.0f ~ %.0f", meta.width, meta.height,
                            meta.bits_per_sample, meta.min_value, meta.max_value);

        if (meta.frame_count > 1)
        {
            int frame = static_cast<int>(state->frame_index);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderInt("##frame", &frame, 0,
                                 static_cast<int>(meta.frame_count) - 1, "帧 %d"))
            {
                set_frame(state, static_cast<uint32_t>(frame));
            }
            item_tooltip("多页 / 多帧文件：拖动切换当前帧（也可按 , / .）");
        }

        ImGui::SeparatorText("窗宽窗位");

        // 每行：名称 / 滑块 / 数值输入。数值输入在窄窗情况下比滑块更易用。
        if (ImGui::BeginTable("##window_controls", 3,
                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX))
        {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
                                    ui_px(state, 40.0f));
            ImGui::TableSetupColumn("##slider", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##input", ImGuiTableColumnFlags_WidthFixed,
                                    ui_px(state, 86.0f));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("窗位");
            ImGui::TableSetColumnIndex(1);
            double level = state->window_setting.level;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderScalar("##level", ImGuiDataType_Double, &level, &state->level_min,
                                    &state->level_max, "%.0f"))
            {
                state->window_setting.level = level;
                state->roi_windowed = false;
                apply_window(state);
            }
            item_tooltip("拖动调整窗位；也可在右侧直接输入数值");
            ImGui::TableSetColumnIndex(2);
            double level_input = state->window_setting.level;
            ImGui::SetNextItemWidth(-1.0f);
            // InputDouble 不支持 EnterReturnsTrue（ImGui 会断言），改用
            // 「编辑结束」判定：回车或失焦均视为提交。
            ImGui::InputDouble("##level_input", &level_input, 0.0, 0.0, "%.1f");
            item_tooltip("输入窗位，回车或失焦后生效");
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                state->window_setting.level = level_input;
                state->roi_windowed = false;
                apply_window(state);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("窗宽");
            ImGui::TableSetColumnIndex(1);
            double window_width = state->window_setting.width;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderScalar("##width", ImGuiDataType_Double, &window_width,
                                    &state->width_min, &state->width_max, "%.0f"))
            {
                state->window_setting.width = window_width;
                state->roi_windowed = false;
                apply_window(state);
            }
            item_tooltip("拖动调整窗宽；窗宽越窄，对比度越高");
            ImGui::TableSetColumnIndex(2);
            double width_input = state->window_setting.width;
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputDouble("##width_input", &width_input, 0.0, 0.0, "%.1f");
            item_tooltip("输入窗宽，回车或失焦后生效；最小为 1");
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                state->window_setting.width = std::max(width_input, 1.0);
                state->roi_windowed = false;
                apply_window(state);
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        if (ImGui::Button("重置窗 (0)"))
        {
            reset_window(state);
        }
        item_tooltip("只恢复窗值，不清除反色、方向或缩放状态");
        ImGui::SameLine();
        if (ImGui::Button("自动 p1/p99"))
        {
            state->window_setting = voilut::auto_window(meta);
            state->roi_windowed = false;
            apply_window(state);
        }
        item_tooltip("按整幅图像的 1% / 99% 分位数计算窗");
        ImGui::SameLine();
        if (ImGui::Button("重置图像"))
        {
            reset_image(state);
        }
        item_tooltip("清除窗值、反色、插值、方向、缩放平移和 ROI");
        if (state->image->default_window)
        {
            ImGui::SameLine();
            if (ImGui::Button("DICOM 默认窗"))
            {
                state->window_setting = *state->image->default_window;
                state->roi_windowed = false;
                apply_window(state);
            }
            item_tooltip("使用文件自带的 WindowCenter / WindowWidth");
        }

        ImGui::TextDisabled("预设（按值域比例）");
        // 全部预设排成一行，按钮宽度按可用宽度均分。
        const float preset_spacing = ImGui::GetStyle().ItemSpacing.x;
        const float preset_width =
            (ImGui::GetContentRegionAvail().x - preset_spacing * 5.0f) /
            static_cast<float>(voilut::kWindowPresets.size());
        for (std::size_t i = 0; i < voilut::kWindowPresets.size(); ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine();
            }
            if (ImGui::Button(voilut::kWindowPresets[i].name, ImVec2(preset_width, 0.0f)))
            {
                apply_preset(state, i);
            }
            item_tooltip("按图像值域比例应用该预设窗");
        }

        draw_histogram(state, ui_px(state, 64.0f));
        ImGui::TextDisabled("直方图：高亮 = 当前窗；黄线 = p1/p99；拖动可移动窗位");

        ImGui::Separator();
        if (ImGui::Checkbox("反色 (I)", &state->invert))
        {
            state->renderer->set_invert(state->invert);
        }
        item_tooltip("反转显示灰度，不修改原始像素");
        ImGui::SameLine();
        if (ImGui::Checkbox("双线性 (L)", &state->linear))
        {
            state->renderer->set_interpolation(state->linear);
        }
        item_tooltip("放大时使用双线性插值，显示更平滑");
        ImGui::SameLine();
        ImGui::Checkbox("记住窗值", &state->remember_window);
        item_tooltip("在当前会话内按图像路径记住窗值");

        if (ImGui::Button("适应窗口 (F)"))
        {
            fit_to_window(state);
        }
        item_tooltip("等比缩放并居中显示整幅图像");
        ImGui::SameLine();
        if (ImGui::Button("旋转 (R)"))
        {
            state->orientation = voilut::rotate_cw(state->orientation);
            apply_orientation(state);
        }
        item_tooltip("顺时针旋转 90°");
        ImGui::SameLine();
        if (ImGui::Button("水平翻转 (H)"))
        {
            state->orientation.flip_h = !state->orientation.flip_h;
            apply_orientation(state);
        }
        item_tooltip("左右翻转显示方向");
        ImGui::SameLine();
        if (ImGui::Button("垂直翻转 (V)"))
        {
            state->orientation.flip_v = !state->orientation.flip_v;
            apply_orientation(state);
        }
        item_tooltip("上下翻转显示方向");

        ImGui::Separator();
        if (state->roi_windowed)
        {
            ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.0f, 1.0f), "当前窗来自 ROI 统计");
        }
        ImGui::Text("缩放 %d%%", static_cast<int>(state->viewport.scale * 100.0));
        if (state->hover_valid)
        {
            const double ratio = voilut::display_ratio(state->hover_value, state->window_setting,
                                                       state->invert, state->voi_mode);
            ImGui::Text("像素 (%d, %d) = %u → 显示 %d", state->hover_x, state->hover_y,
                        static_cast<unsigned>(state->hover_value),
                        static_cast<int>(std::lround(ratio * 255.0)));
        }
        else
        {
            ImGui::TextDisabled("像素: -");
        }
        ImGui::SeparatorText("操作说明");
        ImGui::TextDisabled("左键拖动：框选 ROI 调窗");
        ImGui::TextDisabled("Shift+左键拖动：手动调窗");
        ImGui::TextDisabled("右键拖动：平移");
        ImGui::TextDisabled("滚轮：以光标为中心缩放");
        ImGui::TextDisabled("Ctrl+滚轮：窗宽");
        ImGui::TextDisabled("Shift+滚轮：窗位");
        ImGui::TextDisabled("←/→：窗位微调，↑/↓：窗宽微调");
        ImGui::TextDisabled("F：适应窗口，I：反色，L：双线性");
        ImGui::TextDisabled("R：旋转，H/V：水平/垂直翻转");
        ImGui::TextDisabled("0：重置窗，1–6：预设，[ ]：切换图像");

        // 本帧内通过按钮 / 直方图改窗时，表格已经画完；按需渲染下补一帧刷新。
        if (state->window_setting.level != window_at_frame_start.level ||
            state->window_setting.width != window_at_frame_start.width)
        {
            glfwPostEmptyEvent();
        }

        ImGui::PopStyleColor();
        ImGui::End();
    }

    /// @brief 操作记录面板：保存当前 ROI，并列出已记录区域。
    void draw_roi_history(AppState *state)
    {
        const float history_width = ui_px(state, 380.0f);
        const float history_height = ui_px(state, 260.0f);
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float top = ImGui::GetFrameHeight() + ui_px(state, 4.0f);
        const float margin = ui_px(state, 12.0f);
        const float x = std::max(margin, display.x - history_width - margin);
        ImGui::SetNextWindowPos(ImVec2(x, top), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(history_width, history_height), ImGuiCond_Always);
        ImGui::Begin("操作记录");

        ImGui::BeginDisabled(!state->roi_done);
        if (ImGui::Button("记录当前 ROI"))
        {
            record_current_roi(state);
        }
        item_tooltip("把当前 ROI 的原图坐标和窗值加入操作记录");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(state->roi_records.empty());
        if (ImGui::Button("清空记录"))
        {
            state->roi_records.clear();
        }
        item_tooltip("删除全部 ROI 操作记录");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu 条", state->roi_records.size());

        ImGui::Separator();
        if (state->roi_records.empty())
        {
            ImGui::TextDisabled("尚未记录 ROI");
            ImGui::End();
            return;
        }

        for (auto it = state->roi_records.rbegin(); it != state->roi_records.rend(); ++it)
        {
            const RoiRecord &record = *it;
            ImGui::PushID(static_cast<int>(record.id));
            ImGui::TextWrapped("#%zu  %s", record.id, record.image_name.c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", record.image_path.c_str());
            }
            ImGui::TextDisabled("原图 ROI (%.0f, %.0f)  %.0f × %.0f", record.rect.x,
                                record.rect.y, record.rect.width, record.rect.height);
            ImGui::TextDisabled("窗位 %.0f  窗宽 %.0f", record.window.level, record.window.width);
            ImGui::PopID();
            ImGui::Separator();
        }

        ImGui::End();
    }

    /// @brief 图像上的浮层：当前窗值 + ROI 框。
    void draw_overlay(AppState *state)
    {
        if (state->image == nullptr)
        {
            return;
        }
        ImDrawList *draw_list = ImGui::GetBackgroundDrawList();

        // 左上角窗值（跟随窗口而非面板，全屏查看时也能看到）。
        const std::string text =
            std::format("L {:.0f}   W {:.0f}", state->window_setting.level,
                        state->window_setting.width);
        const ImVec2 pos(ui_px(state, kPanelMargin + kPanelWidth + 16.0f),
                         ImGui::GetFrameHeight() + ui_px(state, 8.0f));
        for (int dx = -1; dx <= 1; ++dx)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                if (dx != 0 || dy != 0)
                {
                    draw_list->AddText(
                        ImVec2(pos.x + static_cast<float>(dx) * state->ui_scale,
                               pos.y + static_cast<float>(dy) * state->ui_scale),
                        IM_COL32(0, 0, 0, 200), text.c_str());
                }
            }
        }
        draw_list->AddText(pos, IM_COL32(120, 210, 255, 255), text.c_str());

        // ROI 框选：拖动中虚线，完成后实线。
        if (!state->roi_active && !state->roi_done)
        {
            return;
        }
        if (state->roi_image.width <= 0.0 || state->roi_image.height <= 0.0)
        {
            return;
        }
        // ROI 存的是原图坐标：每帧重新投影，缩放 / 平移 / 旋转后自动跟随。
        const auto [screen_x0, screen_y0] = original_to_screen(
            state, state->roi_image.x, state->roi_image.y);
        const auto [screen_x1, screen_y1] = original_to_screen(
            state, state->roi_image.x + state->roi_image.width,
            state->roi_image.y + state->roi_image.height);
        const ImVec2 a(static_cast<float>(std::min(screen_x0, screen_x1)),
                       static_cast<float>(std::min(screen_y0, screen_y1)));
        const ImVec2 b(static_cast<float>(std::max(screen_x0, screen_x1)),
                       static_cast<float>(std::max(screen_y0, screen_y1)));
        const ImU32 color = IM_COL32(120, 200, 255, 255);
        if (state->roi_active)
        {
            const float kDash = 6.0f * state->ui_scale;
            const float kGap = 4.0f * state->ui_scale;
            const ImVec2 corners[4] = {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y)};
            for (int i = 0; i < 4; ++i)
            {
                const ImVec2 p0 = corners[i];
                const ImVec2 p1 = corners[(i + 1) % 4];
                const float length = std::hypot(p1.x - p0.x, p1.y - p0.y);
                if (length <= 0.0f)
                {
                    continue;
                }
                const float ux = (p1.x - p0.x) / length;
                const float uy = (p1.y - p0.y) / length;
                for (float d = 0.0f; d < length; d += kDash + kGap)
                {
                    const float e = std::min(d + kDash, length);
                    draw_list->AddLine(ImVec2(p0.x + ux * d, p0.y + uy * d),
                                       ImVec2(p0.x + ux * e, p0.y + uy * e), color,
                                       1.5f * state->ui_scale);
                }
            }
        }
        else
        {
            draw_list->AddRect(a, b, color, 0.0f, 0, 1.5f * state->ui_scale);
        }
    }

    /// @brief 错误弹窗：用一个不可见宿主窗口承载，菜单栏隐藏时也能弹出。
    void draw_error_popup(AppState *state)
    {
        ImGui::SetNextWindowSize(ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowPos(ImVec2(-100.0f, -100.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("##error_host", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoMove);
        if (state->error_pending)
        {
            ImGui::OpenPopup("错误##windower");
            state->error_pending = false;
        }
        ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f * state->ui_scale, 0.0f),
                                            ImVec2(760.0f * state->ui_scale, 1000.0f));
        if (ImGui::BeginPopupModal("错误##windower", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped("%s", state->error_message.c_str());
            ImGui::Separator();
            if (ImGui::Button("确定", ImVec2(120.0f * state->ui_scale, 0.0f)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    /// @brief 由启动参数构建图像列表：目录展开为其中的图像，文件按原样收集。
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
} // namespace

int main(int argc, char **argv)
{
    // 日志行缓冲：stdout 重定向到文件时也能及时看到「已加载 …」等提示。
    // 注意：MSVC 的 setvbuf 要求 size > 0（即使 buffer 为 nullptr），传 0 会
    // 触发 invalid parameter 并直接终止进程；glibc 则接受 0。
    std::setvbuf(stdout, nullptr, _IOLBF, 4096);
#if defined(_WIN32)
    // 源码与 std::println 输出均为 UTF-8；控制台默认代码页（GBK）会把中文显示成乱码。
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    const std::vector<std::string> args(argv + 1, argv + argc);
    std::vector<std::string> playlist = build_playlist(args);
    if (!args.empty() && playlist.empty())
    {
        std::println("用法: {} <图像文件或目录> [更多文件/目录 ...]", argv[0]);
        std::println("  支持 .tif / .tiff / .dcm；目录会展开为其中的图像");
        std::println("  无参数启动时，可在窗口内通过「打开文件夹...」选择图像。");
        return EXIT_FAILURE;
    }

    voilut::DecodedImage image; // 占位对象；无参数启动时保持为空
    std::string initial_path;
    if (!playlist.empty())
    {
        initial_path = playlist.front();
        const auto kind = voilut::kind_from_name(initial_path);
        if (!kind)
        {
            std::println("无法识别的图像格式：{}", initial_path);
            return EXIT_FAILURE;
        }
        auto decoded = voilut::decode_image(initial_path, *kind);
        if (!decoded)
        {
            std::println("解码失败：{}", decoded.error());
            return EXIT_FAILURE;
        }
        image = std::move(decoded.value());
    }

    if (!glfwInit())
    {
        std::println("glfwInit 失败");
        return EXIT_FAILURE;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    // 先隐藏窗口，等 UI 缩放和 Surface 尺寸都确定后再显示，避免首帧闪一下。
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    // 高 DPI：Windows / Linux 下 GLFW 的窗口尺寸是物理像素，系统缩放只通过
    // content scale 报告；不主动放大窗口和 UI，4K/200% 下会得到 1280×720 的
    // 小窗口和 16 px 的小字。macOS 的 framebuffer scale 已由 ImGui 后端处理，
    // 因此那里保持 1.0（与 ImGui 官方 GLFW 示例一致）。
    GLFWwindow *window =
        glfwCreateWindow(voilut::kWidth, voilut::kHeight, "Windower", nullptr, nullptr);
    if (!window)
    {
        std::println("创建窗口失败");
        glfwTerminate();
        return EXIT_FAILURE;
    }
    const float ui_scale = window_ui_scale(window);
    if (ui_scale > 1.0f)
    {
        glfwSetWindowSize(window, static_cast<int>(std::lround(voilut::kWidth * ui_scale)),
                          static_cast<int>(std::lround(voilut::kHeight * ui_scale)));
    }

    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    if (framebuffer_width <= 0 || framebuffer_height <= 0)
    {
        std::println("无法获取窗口 framebuffer 尺寸");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    try
    {
        {
            auto context = voilut::Context::create();
            context.CreateSurface(window);
            // 初始 Surface 必须用真实 framebuffer 尺寸：默认常量只在 100% 缩放
            // 下等于窗口大小，高 DPI 下会把 Surface 配置得比窗口小。
            context.ConfigureSurface(static_cast<uint32_t>(framebuffer_width),
                                     static_cast<uint32_t>(framebuffer_height));

            voilut::ImageRenderer renderer(context);
            if (auto result = renderer.init(); !result)
            {
                throw std::runtime_error("渲染器初始化失败：" + result.error());
            }

            AppState state;
            state.window = window;
            state.context = &context;
            state.renderer = &renderer;
            state.image = &image;
            state.playlist = std::move(playlist);
            state.playlist_index = 0;
            state.viewport_width = static_cast<double>(framebuffer_width);
            state.viewport_height = static_cast<double>(framebuffer_height);
            state.ui_scale = ui_scale;
            state.invert = image.initial_invert;
            glfwSetWindowUserPointer(window, &state);
            glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);
            glfwSetScrollCallback(window, ScrollCallback);
            glfwSetCursorPosCallback(window, CursorPosCallback);
            glfwSetMouseButtonCallback(window, MouseButtonCallback);
            glfwSetKeyCallback(window, KeyCallback);
            glfwSetDropCallback(window, DropCallback);

            renderer.resize(state.viewport_width, state.viewport_height);

            // ---- ImGui 初始化（在自定义回调注册之后，便于链式调用）----
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::StyleColorsDark();
            // 不在工作目录留下 imgui.ini：本程序所有窗口都用 ImGuiCond_Always
            // 固定位置与尺寸，ini 里没有需要保留的布局；单文件发行也不应产生
            // 额外文件（否则从不同目录启动会互相覆盖）。
            ImGui::GetIO().IniFilename = nullptr;
            // 调窗面板控件多（滑块 / 输入 / 预设 / 直方图），减小间距后
            // 在 720p 窗口中不需要滚动条。
            ImGuiStyle &style = ImGui::GetStyle();
            style.WindowPadding = ImVec2(8.0f, 6.0f);
            style.FramePadding = ImVec2(5.0f, 3.0f);
            style.ItemSpacing = ImVec2(6.0f, 4.0f);
            style.ItemInnerSpacing = ImVec2(4.0f, 3.0f);
            // 上面的数值是 96 DPI 设计值：先按系统缩放放大样式，再让字体
            // 跟随同一比例。布局常量由 ui_px() 换算，二者必须使用同一个 scale。
            style.ScaleAllSizes(ui_scale);
            style.FontScaleDpi = ui_scale;
            load_cjk_font();
            renderer.set_voi_mode(state.voi_mode);
            ImGui_ImplGlfw_InitForOther(window, true);
            ImGui_ImplWGPU_InitInfo init_info;
            init_info.Device = context.device.Get();
            init_info.RenderTargetFormat = static_cast<WGPUTextureFormat>(context.format);
            init_info.DepthStencilFormat = WGPUTextureFormat_Undefined;
            if (!ImGui_ImplWGPU_Init(&init_info))
            {
                std::println("ImGui WGPU 后端初始化失败");
            }

            if (!initial_path.empty())
            {
                apply_image(&state, std::move(image), initial_path);
            }
            else
            {
                std::println("未加载图像：拖放图像到窗口，或单击「打开文件夹...」选择目录。");
            }

            // 按需渲染：静止时阻塞在 glfwWaitEvents，避免持续占用 GPU/CPU。
            // 前几帧用 PollEvents 预热：ImGui 的字体图集上传与窗口 auto-resize
            // 需要连续几帧才能稳定，否则首帧之后一阻塞就会停在中间状态。
            // 窗口此前保持隐藏，至此尺寸、字体和首幅图像都已准备好。
            glfwShowWindow(window);
            int warmup_frames = 3;
            while (!glfwWindowShouldClose(window))
            {
                if (warmup_frames > 0)
                {
                    --warmup_frames;
                    glfwPollEvents();
                }
                else
                {
                    glfwWaitEvents();
                }

                ImGui_ImplWGPU_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
                draw_menu_bar(&state);
                update_hover(&state);
                if (state.show_control_panel)
                {
                    draw_panel(&state);
                }
                if (state.show_history_panel)
                {
                    draw_roi_history(&state);
                }
                draw_overlay(&state);
                draw_error_popup(&state);
                ImGui::Render();

                wgpu::SurfaceTexture surfaceTexture;
                context.surface.GetCurrentTexture(&surfaceTexture);
                wgpu::RenderPassColorAttachment attachment{
                    .view = surfaceTexture.texture.CreateView(),
                    .loadOp = wgpu::LoadOp::Clear,
                    .storeOp = wgpu::StoreOp::Store,
                    .clearValue = {0.0, 0.0, 0.0, 1.0},
                };
                wgpu::RenderPassDescriptor renderPass{
                    .colorAttachmentCount = 1,
                    .colorAttachments = &attachment,
                };

                wgpu::CommandEncoder encoder = context.device.CreateCommandEncoder();
                wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&renderPass);
                renderer.record(pass); // 先绘制图像
                ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass.Get()); // 再绘制 UI
                pass.End();

                wgpu::CommandBuffer commands = encoder.Finish();
                context.device.GetQueue().Submit(1, &commands);
                context.surface.Present();
                context.instance.ProcessEvents();
            }

            ImGui_ImplWGPU_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            context.UnconfigureSurface();
        } // GPU 资源先析构，之后才关闭 GLFW / X11。

        glfwDestroyWindow(window);
        glfwTerminate();
        std::println("程序正常退出");
        return EXIT_SUCCESS;
    }
    catch (const std::exception &e)
    {
        std::println("致命错误: {}", e.what());
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
}
