/// @file ui.cxx
/// @brief 菜单 / 调窗面板 / 直方图 / 叠层 / 错误弹窗。

#include "ui.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include "app_ops.h"
#include "image_io.h"
#include "image_model.h"
#include "image_renderer.h"

namespace voilut::app
{
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

            ImGui::BeginDisabled(state->image != nullptr && state->image->meta.is_float);
            if (menu_item("DICOM 精确 VOI", nullptr, state->voi_mode == voilut::VoiMode::Dicom))
            {
                state->voi_mode = state->voi_mode == voilut::VoiMode::Dicom
                                      ? voilut::VoiMode::Simplified
                                      : voilut::VoiMode::Dicom;
                state->renderer->set_voi_mode(state->voi_mode);
            }
            item_tooltip("切换窗值→灰度公式：简化（W 分母）/ DICOM 精确（W-1 分母，C.11.2.1.2.1）");
            ImGui::EndDisabled();

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
            menu_item_toggle("高分辨率细节", &state->detail_enabled);
            item_tooltip("虚拟切片图像放大时按需加载当前视口的 1:1 细节");

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

    void load_cjk_font()
    {
        struct FontCandidate
        {
            const char *path;
            int face;
        };
        static const FontCandidate kCandidates[] = {
            // macOS：系统自带的中文黑体。PingFang 在部分系统版本 / 精简安装中
            // 不存在，因此优先 STHeiti（face 1 = Heiti SC），再退到冬青黑体 /
            // 宋体 / Arial Unicode MS。
            {"/System/Library/Fonts/STHeiti Medium.ttc", 1},
            {"/System/Library/Fonts/STHeiti Light.ttc", 1},
            {"/System/Library/Fonts/Hiragino Sans GB.ttc", 0},
            {"/System/Library/Fonts/Supplemental/Songti.ttc", 0},
            {"/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 0},
            // Linux
            {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2},
            {"/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf", 0},
            {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 2},
            {"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", 2},
            {"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 0},
            {"/usr/share/fonts/truetype/arphic/uming.ttc", 0},
            // Windows
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
        // 默认字体只有拉丁字形，中文会显示成 "?"；日志里明确提示便于排查。
        std::println("警告: 未找到中文字体，界面中文将显示为 \"?\"");
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
            open_folder(state);
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
        if (meta.is_float)
        {
            ImGui::TextDisabled("%u x %u  ·  %u bit float  ·  值域 %.6g ~ %.6g", meta.width,
                                meta.height, meta.bits_per_sample, meta.min_value,
                                meta.max_value);
        }
        else
        {
            ImGui::TextDisabled("%u x %u  ·  %u bit  ·  值域 %.0f ~ %.0f", meta.width,
                                meta.height, meta.bits_per_sample, meta.min_value,
                                meta.max_value);
        }
        const voilut::ValueScale &scale = state->image->scale;
        const bool fractional = meta.is_float || std::abs(scale.slope) != 1.0;
        const char *value_format = fractional ? "%.4g" : "%.0f";
        const double level_low =
            std::min(scale.to_modality(state->level_min), scale.to_modality(state->level_max));
        const double level_high =
            std::max(scale.to_modality(state->level_min), scale.to_modality(state->level_max));
        const double width_low = std::min(scale.width_to_modality(state->width_min),
                                          scale.width_to_modality(state->width_max));
        const double width_high = std::max(scale.width_to_modality(state->width_min),
                                           scale.width_to_modality(state->width_max));
        if (state->image->padding.enabled)
        {
            ImGui::TextDisabled("已排除填充值 %.0f ~ %.0f（不参与统计 / 直方图 / ROI）",
                                state->image->padding.low, state->image->padding.high);
        }
        if (state->renderer->preview_scale() > 1)
        {
            ImGui::TextDisabled("预览：纹理降采样 1/%u", state->renderer->preview_scale());
        }
        if (state->image->preview_width != 0 && state->detail_loaded.width > 0.0)
        {
            ImGui::TextDisabled("细节：%.0f×%.0f @ (%.0f, %.0f)", state->detail_loaded.width,
                                state->detail_loaded.height, state->detail_loaded.x,
                                state->detail_loaded.y);
        }

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
            double level = scale.to_modality(state->window_setting.level);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderScalar("##level", ImGuiDataType_Double, &level, &level_low,
                                    &level_high, value_format))
            {
                state->window_setting.level = scale.to_storage(level);
                state->roi_windowed = false;
                apply_window(state);
            }
            item_tooltip("拖动调整窗位；也可在右侧直接输入数值");
            ImGui::TableSetColumnIndex(2);
            double level_input = scale.to_modality(state->window_setting.level);
            ImGui::SetNextItemWidth(-1.0f);
            // InputDouble 不支持 EnterReturnsTrue（ImGui 会断言），改用
            // 「编辑结束」判定：回车或失焦均视为提交。
            ImGui::InputDouble("##level_input", &level_input, 0.0, 0.0,
                               fractional ? "%.6g" : "%.1f");
            item_tooltip("输入窗位，回车或失焦后生效");
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                state->window_setting.level = scale.to_storage(level_input);
                state->roi_windowed = false;
                apply_window(state);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("窗宽");
            ImGui::TableSetColumnIndex(1);
            double window_width = scale.width_to_modality(state->window_setting.width);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderScalar("##width", ImGuiDataType_Double, &window_width,
                                    &width_low, &width_high, value_format))
            {
                state->window_setting.width = scale.width_to_storage(window_width);
                state->roi_windowed = false;
                apply_window(state);
            }
            item_tooltip("拖动调整窗宽；窗宽越窄，对比度越高");
            ImGui::TableSetColumnIndex(2);
            double width_input = scale.width_to_modality(state->window_setting.width);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputDouble("##width_input", &width_input, 0.0, 0.0,
                               fractional ? "%.6g" : "%.1f");
            item_tooltip("输入窗宽，回车或失焦后生效；不能小于最小窗宽");
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                const double min_modal =
                    scale.width_to_modality(meta.is_float ? state->width_min : 1.0);
                state->window_setting.width =
                    scale.width_to_storage(std::max(width_input, min_modal));
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

        if (state->image->window_presets.size() > 1)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("DICOM 窗值（共 %zu 组）", state->image->window_presets.size());
            for (std::size_t i = 0; i < state->image->window_presets.size(); ++i)
            {
                if (i > 0)
                {
                    ImGui::SameLine();
                }
                const std::string label = std::format("DICOM {}", i + 1);
                if (ImGui::Button(label.c_str()))
                {
                    state->window_setting = state->image->window_presets[i];
                    state->roi_windowed = false;
                    apply_window(state);
                }
                item_tooltip("使用文件自带的第 N 组 WindowCenter / WindowWidth");
            }
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
            const voilut::ValueScale &scale = state->image->scale;
            const double ratio = voilut::display_ratio(state->hover_value, state->window_setting,
                                                       state->invert, state->voi_mode);
            const bool has_modality = !state->image->modality_unit.empty() ||
                                      scale.slope != 1.0 || scale.intercept != 0.0;
            if (state->image->meta.is_float)
            {
                ImGui::Text("像素 (%d, %d) = %.6g → 显示 %d", state->hover_x, state->hover_y,
                            state->hover_value, static_cast<int>(std::lround(ratio * 255.0)));
            }
            else if (has_modality)
            {
                const char *label =
                    state->image->modality_unit.empty() ? "模态值"
                                                        : state->image->modality_unit.c_str();
                ImGui::Text("像素 (%d, %d) = %.0f（%s %.0f）→ 显示 %d", state->hover_x,
                            state->hover_y, state->hover_value, label,
                            scale.to_modality(state->hover_value),
                            static_cast<int>(std::lround(ratio * 255.0)));
            }
            else
            {
                ImGui::Text("像素 (%d, %d) = %.0f → 显示 %d", state->hover_x, state->hover_y,
                            state->hover_value, static_cast<int>(std::lround(ratio * 255.0)));
            }
            if (state->image->preview_width != 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("（预览值）");
            }
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
            const double level = record.scale.to_modality(record.window.level);
            const double width = record.scale.width_to_modality(record.window.width);
            ImGui::TextDisabled("窗位 %.6g  窗宽 %.6g%s%s", level, width,
                                record.modality_unit.empty() ? "" : " ",
                                record.modality_unit.c_str());
            ImGui::PopID();
            ImGui::Separator();
        }

        ImGui::End();
    }

    /// @brief 图像上的浮层：当前窗值 + ROI 框。
    void draw_overlay(AppState *state)
    {
        ImDrawList *draw_list = ImGui::GetBackgroundDrawList();
        if (state->loading)
        {
            const char *text = "正在加载…";
            const ImVec2 size = ImGui::CalcTextSize(text);
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            draw_list->AddText(ImVec2((display.x - size.x) * 0.5f,
                                      ImGui::GetFrameHeight() + ui_px(state, 12.0f)),
                               IM_COL32(255, 220, 120, 255), text);
        }
        if (state->image == nullptr)
        {
            return;
        }

        // 左上角窗值（跟随窗口而非面板，全屏查看时也能看到）。
        const voilut::ValueScale &scale = state->image->scale;
        const double level = scale.to_modality(state->window_setting.level);
        const double width = scale.width_to_modality(state->window_setting.width);
        const bool fractional =
            state->image->meta.is_float || std::abs(scale.slope) != 1.0;
        std::string text = fractional
                               ? std::format("L {:.6g}   W {:.6g}", level, width)
                               : std::format("L {:.0f}   W {:.0f}", level, width);
        if (!state->image->modality_unit.empty())
        {
            text += " " + state->image->modality_unit;
        }
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

} // namespace voilut::app
