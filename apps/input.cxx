/// @file input.cxx
/// @brief GLFW 输入回调。

#include "input.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include "app_ops.h"
#include "context.h"
#include "image_io.h"
#include "image_renderer.h"

namespace voilut::app
{
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
            const bool is_float = state->image->meta.is_float;
            state->window_setting = voilut::drag_window(
                dragged, x - state->wl_start_x, y - state->wl_start_y,
                is_float ? state->width_min : 1.0,
                is_float ? state->width_max : 65535.0);
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
        // 整数图像的窗位步进下限为 1；浮点图像按窗宽比例，否则 1.0 会把窗位
        // 直接跳到值域之外（浮点值域可能只有 0..1）。
        const bool is_float = state->image != nullptr && state->image->meta.is_float;
        const double level_step = std::max(state->window_setting.width * 0.02 * fine,
                                           is_float ? 0.0 : 1.0);

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
} // namespace voilut::app
