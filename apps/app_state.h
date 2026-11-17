/// @file app_state.h
/// @brief Windower 的共享应用状态（UI / 输入 / 图像 I/O 共用）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

#include "image_model.h"
#include "orientation.h"
#include "viewport.h"

struct GLFWwindow;

namespace voilut
{
    class Context;
    class ImageRenderer;
}

namespace voilut::app
{
    /// @brief 已记录的 ROI（原图坐标，缩放 / 平移 / 旋转后仍有效）。
    struct RoiRecord
    {
        std::size_t id = 0;
        std::string image_name;
        std::string image_path;
        voilut::Rect rect;
        voilut::WindowSetting window;
        voilut::ValueScale scale;
        std::string modality_unit;
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
        double hover_value = 0.0; ///< 当前像素值（整数图像也为 double，便于统一格式化）。

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

        // 异步解码：避免大图加载时冻结 UI。
        bool loading = false; ///< 是否正在后台解码。
        std::future<std::expected<voilut::DecodedImage, std::string>> load_future; ///< 后台结果。
        std::string load_path;                 ///< 正在加载的路径。
        std::size_t pending_index = 0;         ///< 加载期间用户请求的下一张。
        bool has_pending = false;              ///< 是否有待处理的翻页请求。
    };

} // namespace voilut::app
