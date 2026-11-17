/// @file app_ops.h
/// @brief 窗值 / 视口 / ROI / 帧切换等显示状态操作。

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "app_state.h"

namespace voilut::app
{
    void apply_window(AppState *state);
    void report_error(AppState *state, std::string message);
    void fit_to_window(AppState *state);
    [[nodiscard]] std::pair<double, double> screen_to_original(const AppState *state, double x,
                                                               double y);
    [[nodiscard]] std::pair<double, double> original_to_screen(const AppState *state, double x,
                                                               double y);
    void apply_orientation(AppState *state);
    void reset_window(AppState *state);
    void reset_image(AppState *state);
    void apply_preset(AppState *state, std::size_t index);
    void nudge_window(AppState *state, double level_delta, double width_factor);
    void update_roi_drag(AppState *state, double screen_x, double screen_y);
    bool apply_roi_window(AppState *state);
    void record_current_roi(AppState *state);
    void update_window_title(AppState *state);
    void apply_image(AppState *state, voilut::DecodedImage image, const std::string &path,
                     std::shared_ptr<voilut::RegionSource> source = {});
    void set_frame(AppState *state, uint32_t frame);
    void update_hover(AppState *state);
    void update_detail(AppState *state);
} // namespace voilut::app
