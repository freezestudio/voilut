/// @file ui.h
/// @brief 菜单 / 调窗面板 / 直方图 / 叠层 / 错误弹窗。

#pragma once

#include "app_state.h"

namespace voilut::app
{
    void load_cjk_font();
    void draw_menu_bar(AppState *state);
    void draw_panel(AppState *state);
    void draw_roi_history(AppState *state);
    void draw_overlay(AppState *state);
    void draw_error_popup(AppState *state);
} // namespace voilut::app
