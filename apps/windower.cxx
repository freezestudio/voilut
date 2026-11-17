/// @file windower.cxx
/// @brief Windower 应用入口：窗口 / WebGPU / ImGui 初始化与主循环。
/// @details 具体功能拆分在：
///          - app_state.h   共享状态
///          - app_ops.cxx   窗值 / 视口 / ROI / 帧切换
///          - image_io.cxx  图像列表与后台解码
///          - input.cxx     GLFW 输入回调
///          - ui.cxx        菜单 / 面板 / 直方图 / 叠层

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_wgpu.h>

#include "app_ops.h"
#include "app_state.h"
#include "context.h"
#include "image_io.h"
#include "image_model.h"
#include "image_renderer.h"
#include "input.h"
#include "ui.h"
#include "window.h"

#ifndef VOILUT_VERSION
#define VOILUT_VERSION "0.0.0"
#endif

using namespace voilut::app;

namespace
{
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

    void print_usage(const char *program)
    {
        std::println("用法: {} <图像文件或目录> [更多文件/目录 ...]", program);
        std::println("  支持 .tif / .tiff / .dcm；目录会展开为其中的图像");
        std::println("  无参数启动时，可在窗口内通过「打开文件夹...」选择图像");
        std::println("  --help, -h   显示本帮助");
        std::println("  --version    显示版本");
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
    for (const std::string &arg : args)
    {
        if (arg == "--help" || arg == "-h")
        {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (arg == "--version")
        {
            std::println("Windower {}", VOILUT_VERSION);
            return EXIT_SUCCESS;
        }
    }
    std::vector<std::string> playlist = build_playlist(args);
    if (!args.empty() && playlist.empty())
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    voilut::DecodedImage image; // 占位对象；无参数启动时保持为空
    std::string initial_path;
    if (!playlist.empty())
    {
        initial_path = playlist.front();
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
                start_load(&state, initial_path);
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
                else if (state.detail_dirty)
                {
                    // 细节请求有 120 ms 防抖：需要一个超时唤醒，否则按需渲染会
                    // 一直阻塞在 WaitEvents，防抖到期后没有事件就不再触发。
                    glfwWaitEventsTimeout(0.15);
                }
                else
                {
                    glfwWaitEvents();
                }

                ImGui_ImplWGPU_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                poll_load(&state);
                update_detail(&state);
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
