/// @file app_ops.cxx
/// @brief 窗值 / 视口 / ROI / 帧切换实现。

#include "app_ops.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <print>
#include <utility>

#include <GLFW/glfw3.h>

#include "image_model.h"
#include "image_renderer.h"
#include "orientation.h"
#include "viewport.h"

namespace voilut::app
{
    void apply_window(AppState *state)
    {
        if (state->renderer == nullptr)
        {
            return;
        }
        const bool is_float = state->image != nullptr && state->image->meta.is_float;
        const double min_width = is_float ? state->width_min : 1.0;
        const double max_width = is_float ? std::max(state->width_max, min_width * 10.0) : 65535.0;
        // 窗位限制在「数据范围 + 5% 余量」内；整数窗宽允许 1..65535，
        // 浮点窗宽按值域比例（否则 0..1 的数据会被扩成几千）。
        state->window_setting.level =
            std::clamp(state->window_setting.level, state->level_min, state->level_max);
        state->window_setting.width =
            std::clamp(state->window_setting.width, min_width, max_width);
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

    /// @brief 按像素类型上传指定帧。
    std::expected<void, std::string> upload_frame(voilut::ImageRenderer &renderer,
                                                  const voilut::DecodedImage &image,
                                                  uint32_t frame)
    {
        std::optional<std::pair<uint32_t, uint32_t>> data_size;
        if (image.preview_width != 0)
        {
            data_size = std::pair<uint32_t, uint32_t>{image.preview_width, image.preview_height};
        }
        if (image.pixel_type == voilut::DecodedImage::PixelType::Float32)
        {
            return renderer.upload_float(image.meta.width, image.meta.height,
                                         image.frame_floats(frame), data_size);
        }
        return renderer.upload(image.meta.width, image.meta.height, image.frame_pixels(frame),
                               data_size);
    }

    /// @brief 统计指定帧（整数 / 浮点分派；整数会排除填充值）。
    voilut::ImageStats frame_stats(const voilut::DecodedImage &image, uint32_t frame,
                                   voilut::ValueExclusion exclusion)
    {
        return image.pixel_type == voilut::DecodedImage::PixelType::Float32
                   ? voilut::compute_stats_float(image.frame_floats(frame))
                   : voilut::compute_stats(image.frame_pixels(frame), exclusion);
    }

    /// @brief 指定帧的 256 桶直方图（整数 / 浮点分派；整数会排除填充值）。
    voilut::Histogram frame_histogram(const voilut::DecodedImage &image, uint32_t frame, double low,
                                      double high, voilut::ValueExclusion exclusion)
    {
        return image.pixel_type == voilut::DecodedImage::PixelType::Float32
                   ? voilut::compute_histogram_float(image.frame_floats(frame), low, high)
                   : voilut::compute_histogram(image.frame_pixels(frame), low, high, exclusion);
    }

    /// @brief 设置窗宽滑块范围与拖拽盘限（浮点按值域比例）。
    void set_width_limits(AppState *state, const voilut::ImageMeta &meta)
    {
        if (meta.is_float)
        {
            state->width_min = std::max(meta.value_range * 1e-4, 1e-12);
            state->width_max = std::max(meta.value_range * 1.2, state->width_min * 10.0);
        }
        else
        {
            state->width_min = 1.0;
            state->width_max = std::max(meta.value_range, 2.0);
        }
    }

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
        const auto &meta = state->image->meta;
        const uint32_t texture_width = state->image->texture_width();
        const uint32_t texture_height = state->image->texture_height();
        const double scale_x = static_cast<double>(texture_width) / meta.width;
        const double scale_y = static_cast<double>(texture_height) / meta.height;
        const voilut::Rect roi_scaled{
            .x = state->roi_image.x * scale_x,
            .y = state->roi_image.y * scale_y,
            .width = state->roi_image.width * scale_x,
            .height = state->roi_image.height * scale_y,
        };
        const auto setting =
            state->image->pixel_type == voilut::DecodedImage::PixelType::Float32
                ? voilut::window_for_roi_float(state->image->frame_floats(state->frame_index),
                                               texture_width, texture_height, roi_scaled)
                : voilut::window_for_roi(state->image->frame_pixels(state->frame_index),
                                         texture_width, texture_height, roi_scaled,
                                         state->image->padding);
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
            .scale = state->image->scale,
            .modality_unit = state->image->modality_unit,
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
        const uint32_t preview =
            state->renderer != nullptr ? state->renderer->preview_scale() : 1;
        if (preview > 1)
        {
            title += std::format("（预览 1/{}）", preview);
        }
        glfwSetWindowTitle(state->window, title.c_str());
    }

    /// @brief 把解码结果应用到状态与渲染器。
    void apply_image(AppState *state, voilut::DecodedImage image, const std::string &path,
                     std::shared_ptr<voilut::RegionSource> source)
    {
        // 先保存上一张的窗值。
        if (state->remember_window && state->image != nullptr && !state->current_path.empty())
        {
            state->remembered[state->current_path] = state->window_setting;
        }

        // 先上传纹理：失败时保留当前图像，并向用户报告原因（例如超过纹理上限）。
        if (state->renderer != nullptr)
        {
            if (auto uploaded = upload_frame(*state->renderer, image, 0); !uploaded)
            {
                report_error(state, uploaded.error());
                return;
            }
        }

        *state->image = std::move(image);
        state->current_path = path;
        state->frame_index = 0;
        state->region_source = std::move(source);
        ++state->detail_generation;
        state->detail_loaded = {};
        state->detail_loaded_frame = 0;
        state->detail_request = {};
        state->renderer->clear_detail();

        const voilut::ImageMeta &meta = state->image->meta;
        // DICOM 精确 VOI 只对整数像素有意义（W-1 分母/阈值）；浮点图像回到简化公式。
        if (meta.is_float && state->voi_mode == voilut::VoiMode::Dicom)
        {
            state->voi_mode = voilut::VoiMode::Simplified;
            state->renderer->set_voi_mode(state->voi_mode);
        }
        state->level_min = meta.min_value - meta.value_range * 0.05;
        state->level_max = meta.max_value + meta.value_range * 0.05;
        set_width_limits(state, meta);
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
        state->histogram = frame_histogram(*state->image, 0, meta.min_value,
                                           std::max(meta.max_value, meta.min_value + 1.0),
                                           state->image->padding);

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
        state->detail_last_viewport = state->viewport;
        state->detail_last_orientation = state->orientation;
        state->detail_last_frame = state->frame_index;
        state->detail_dirty = true;
        state->detail_changed_at = std::chrono::steady_clock::now();

        if (state->window != nullptr)
        {
            update_window_title(state);
        }
        if (meta.is_float)
        {
            std::println("已加载 {}（{}x{}，32 位浮点，值域 {:.6g} ~ {:.6g}）",
                         state->image->name, meta.width, meta.height, meta.min_value,
                         meta.max_value);
        }
        else
        {
            std::println("已加载 {}（{}x{}，{} bit，值域 {} ~ {}）", state->image->name,
                         meta.width, meta.height, meta.bits_per_sample,
                         static_cast<long long>(meta.min_value),
                         static_cast<long long>(meta.max_value));
        }
        if (!state->image->modality_unit.empty())
        {
            std::println("模态值（{}）：{:.6g} ~ {:.6g}", state->image->modality_unit,
                         state->image->scale.to_modality(meta.min_value),
                         state->image->scale.to_modality(meta.max_value));
        }
        if (meta.frame_count > 1)
        {
            std::println("多帧/多页文件：共 {} 帧，可用 , / . 或面板滑块切换", meta.frame_count);
        }
        if (state->renderer->preview_scale() > 1)
        {
            if (state->region_source)
            {
                std::println("大图预览：按 1/{} 显示，放大后按需加载 1:1 细节",
                             state->renderer->preview_scale());
            }
            else
            {
                std::println("大图预览：按 1/{} 降采样显示（放大不会恢复原始像素）",
                             state->renderer->preview_scale());
            }
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

        if (auto uploaded = upload_frame(*state->renderer, *state->image, frame); !uploaded)
        {
            report_error(state, uploaded.error());
            return;
        }
        state->frame_index = frame;

        // 帧统计用于直方图与滑块范围；窗值保持不变（z-stack / 时间序列对比时需要一致）。
        const voilut::ImageStats stats = frame_stats(*state->image, frame, state->image->padding);
        voilut::ImageMeta &meta = state->image->meta;
        meta.min_value = stats.min_value;
        meta.max_value = stats.max_value;
        meta.p1 = stats.p1;
        meta.p99 = stats.p99;
        meta.value_range = stats.value_range;
        state->level_min = stats.min_value - stats.value_range * 0.05;
        state->level_max = stats.max_value + stats.value_range * 0.05;
        set_width_limits(state, meta);
        state->histogram = frame_histogram(*state->image, frame, stats.min_value,
                                           std::max(stats.max_value, stats.min_value + 1.0),
                                           state->image->padding);

        apply_window(state);
        update_window_title(state);
        if (meta.is_float)
        {
            std::println("当前帧 {}/{}（值域 {:.6g} ~ {:.6g}）", frame + 1, count,
                         stats.min_value, stats.max_value);
        }
        else
        {
            std::println("当前帧 {}/{}（值域 {} ~ {}）", frame + 1, count,
                         static_cast<long long>(stats.min_value),
                         static_cast<long long>(stats.max_value));
        }
    }

    /// @brief 启动后台解码（完成后通过 glfwPostEmptyEvent 唤醒主循环）。
    /// @param state 应用状态（窗口指针用于唤醒）。

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
        const uint32_t frame = state->frame_index;
        const uint32_t texture_width = state->image->texture_width();
        const uint32_t texture_height = state->image->texture_height();
        const double sample_x =
            image_x * static_cast<double>(texture_width) / state->image->meta.width;
        const double sample_y =
            image_y * static_cast<double>(texture_height) / state->image->meta.height;
        bool valid = false;
        if (state->image->pixel_type == voilut::DecodedImage::PixelType::Float32)
        {
            const auto value = voilut::pixel_at_float(state->image->frame_floats(frame),
                                                      texture_width, texture_height, sample_x,
                                                      sample_y);
            if (value)
            {
                state->hover_value = static_cast<double>(*value);
                valid = true;
            }
        }
        else
        {
            const auto value = voilut::pixel_at(state->image->frame_pixels(frame),
                                                texture_width, texture_height, sample_x,
                                                sample_y);
            if (value)
            {
                state->hover_value = static_cast<double>(*value);
                valid = true;
            }
        }
        if (valid)
        {
            state->hover_valid = true;
            state->hover_x = static_cast<int>(std::floor(image_x));
            state->hover_y = static_cast<int>(std::floor(image_y));
        }
        else
        {
            state->hover_valid = false;
        }
    }

    /// @brief 视口停止变化后按需加载 1:1 细节纹理（虚拟切片 Phase 1b）。
    void update_detail(AppState *state)
    {
        if (state->image == nullptr || state->renderer == nullptr)
        {
            return;
        }
        const auto now = std::chrono::steady_clock::now();

        if (state->detail_inflight && state->detail_future.valid() &&
            state->detail_future.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready)
        {
            auto result = state->detail_future.get();
            state->detail_inflight = false;
            const bool current = state->detail_request_generation == state->detail_generation;
            if (result && current && state->region_source != nullptr)
            {
                const voilut::PixelRegion &region = *result;
                const bool is_float =
                    state->image->pixel_type == voilut::DecodedImage::PixelType::Float32;
                const auto uploaded =
                    is_float ? state->renderer->upload_detail_float(
                                   region.x, region.y, region.width, region.height, region.f32)
                             : state->renderer->upload_detail(
                                   region.x, region.y, region.width, region.height, region.u16);
                if (!uploaded)
                {
                    std::println("细节上传失败：{}", uploaded.error());
                    state->renderer->clear_detail();
                    state->detail_loaded = {};
                }
                else
                {
                    if (state->detail_loaded.width == 0.0)
                    {
                        std::println("细节纹理已启用：{}×{}", region.width, region.height);
                    }
                    state->detail_loaded = {
                        .x = static_cast<double>(region.x),
                        .y = static_cast<double>(region.y),
                        .width = static_cast<double>(region.width),
                        .height = static_cast<double>(region.height),
                    };
                    state->detail_loaded_frame = state->frame_index;
                }
            }
            else if (!result)
            {
                std::println("细节读取失败：{}", result.error());
                state->renderer->clear_detail();
                state->detail_loaded = {};
            }
            glfwPostEmptyEvent();
        }

        if (state->region_source == nullptr || !state->detail_enabled || state->loading)
        {
            state->renderer->clear_detail();
            state->detail_loaded = {};
            state->detail_dirty = false;
            return;
        }

        const auto &meta = state->image->meta;
        const auto &vp = state->viewport;
        const auto &last_vp = state->detail_last_viewport;
        const auto &orientation = state->orientation;
        const auto &last_orientation = state->detail_last_orientation;
        const bool changed = vp.scale != last_vp.scale || vp.x != last_vp.x ||
                             vp.y != last_vp.y ||
                             orientation.rotation != last_orientation.rotation ||
                             orientation.flip_h != last_orientation.flip_h ||
                             orientation.flip_v != last_orientation.flip_v ||
                             state->frame_index != state->detail_last_frame;
        if (changed)
        {
            state->detail_last_viewport = vp;
            state->detail_last_orientation = orientation;
            state->detail_last_frame = state->frame_index;
            state->detail_dirty = true;
            state->detail_changed_at = now;
        }
        if (!state->detail_dirty)
        {
            return;
        }
        if (now - state->detail_changed_at < std::chrono::milliseconds(120))
        {
            return;
        }
        if (state->detail_inflight)
        {
            return;
        }
        if (vp.scale < 0.75)
        {
            state->renderer->clear_detail();
            state->detail_loaded = {};
            state->detail_dirty = false;
            return;
        }

        const std::array<std::pair<double, double>, 4> corners = {
            screen_to_original(state, 0.0, 0.0),
            screen_to_original(state, state->viewport_width, 0.0),
            screen_to_original(state, 0.0, state->viewport_height),
            screen_to_original(state, state->viewport_width, state->viewport_height),
        };
        double min_x = corners[0].first;
        double max_x = corners[0].first;
        double min_y = corners[0].second;
        double max_y = corners[0].second;
        for (const auto &corner : corners)
        {
            min_x = std::min(min_x, corner.first);
            max_x = std::max(max_x, corner.first);
            min_y = std::min(min_y, corner.second);
            max_y = std::max(max_y, corner.second);
        }
        min_x = std::clamp(min_x, 0.0, static_cast<double>(meta.width));
        max_x = std::clamp(max_x, 0.0, static_cast<double>(meta.width));
        min_y = std::clamp(min_y, 0.0, static_cast<double>(meta.height));
        max_y = std::clamp(max_y, 0.0, static_cast<double>(meta.height));
        if (max_x - min_x < 1.0 || max_y - min_y < 1.0)
        {
            state->detail_dirty = false;
            return;
        }

        constexpr uint32_t kDetailMax = 4096;
        const uint32_t max_width = std::min(kDetailMax, meta.width);
        const uint32_t max_height = std::min(kDetailMax, meta.height);
        const uint32_t min_detail_width = std::min<uint32_t>(256, max_width);
        const uint32_t min_detail_height = std::min<uint32_t>(256, max_height);
        const uint32_t wanted_width = std::clamp<uint32_t>(
            static_cast<uint32_t>(std::ceil(max_x - min_x)), min_detail_width, max_width);
        const uint32_t wanted_height = std::clamp<uint32_t>(
            static_cast<uint32_t>(std::ceil(max_y - min_y)), min_detail_height, max_height);
        if (wanted_width == 0 || wanted_height == 0)
        {
            state->detail_dirty = false;
            return;
        }
        const double center_x = (min_x + max_x) * 0.5;
        const double center_y = (min_y + max_y) * 0.5;
        auto align = [](double value) {
            return static_cast<uint32_t>(std::max(0.0, std::floor(value / 64.0) * 64.0));
        };
        uint32_t rx = align(center_x - wanted_width * 0.5);
        uint32_t ry = align(center_y - wanted_height * 0.5);
        rx = std::min(rx, meta.width - wanted_width);
        ry = std::min(ry, meta.height - wanted_height);

        const voilut::Rect request{
            .x = static_cast<double>(rx),
            .y = static_cast<double>(ry),
            .width = static_cast<double>(wanted_width),
            .height = static_cast<double>(wanted_height),
        };
        if (request.x == state->detail_loaded.x && request.y == state->detail_loaded.y &&
            request.width == state->detail_loaded.width &&
            request.height == state->detail_loaded.height &&
            state->detail_loaded_frame == state->frame_index)
        {
            state->detail_dirty = false;
            return;
        }

        state->detail_dirty = false;
        ++state->detail_generation;
        state->detail_request_generation = state->detail_generation;
        state->detail_request = request;
        state->detail_inflight = true;
        auto source = state->region_source;
        GLFWwindow *window = state->window;
        const uint32_t frame = state->frame_index;
        state->detail_future = std::async(std::launch::async, [source, request, frame, window]() {
            auto result = source->read_region(static_cast<uint32_t>(request.x),
                                              static_cast<uint32_t>(request.y),
                                              static_cast<uint32_t>(request.width),
                                              static_cast<uint32_t>(request.height), frame);
            if (window != nullptr)
            {
                glfwPostEmptyEvent();
            }
            return result;
        });
    }
} // namespace voilut::app
