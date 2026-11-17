/// @file image_io.h
/// @brief 图像列表、后台解码与打开目录。

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "app_state.h"

namespace voilut::app
{
    [[nodiscard]] std::vector<std::string> build_playlist(const std::vector<std::string> &args);
    void start_load(AppState *state, const std::string &path);
    void poll_load(AppState *state);
    void load_playlist_item(AppState *state, std::size_t index);
    void open_path(AppState *state, const std::string &path);
    void open_directory(AppState *state, const std::string &dir);
    void step_playlist(AppState *state, int delta);
    void open_folder(AppState *state);
} // namespace voilut::app
