// ============================================================
// context.cxx —— WebGPU 环境搭建（协程版）
// ============================================================
// 初始化顺序（每一步都依赖前一步的结果）：
//   CreateInstance → RequestAdapter → RequestDevice
//
// WebGPU 的 RequestXxx 都是异步回调式 API。本文件把它包装成
// 两个协程（RequestAdapterAsync / RequestDeviceAsync），
// 因此 InitAsync 内可以用同步写法逐步 co_await。
//
// 注意：为简化实现，当前在协程内部用 instance.WaitAny() 阻塞等待回调完成。
// 完整的异步渲染流程（多帧并行）不应阻塞，而应让回调 resume 协程；
// 上文介绍的 async_value 已具备该能力，此处尚未使用。
// ============================================================

#include "context.h"

#include <algorithm>
#include <format>
#include <iostream>
#include <stdexcept>

#include <GLFW/glfw3.h>
#include <dawn/webgpu_cpp_print.h> // 让 wgpu 类型可以直接 std::print
#include <webgpu/webgpu_glfw.h>    // wgpu::glfw::CreateSurfaceForWindow
#include <print>

#include "window.h"

namespace voilut
{

    // ------------------------------------------------------------
    // InitAsync：三步初始化总入口
    // ------------------------------------------------------------
    task<void> Context::InitAsync()
    {
        // 1) 创建 Instance。Instance 是 WebGPU 的入口对象。
        //    这里显式声明需要 TimedWaitAny 特性，供带超时的 WaitAny 使用。
        static const auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
        wgpu::InstanceDescriptor instanceDesc{
            .requiredFeatureCount = 1,
            .requiredFeatures = &kTimedWaitAny
        };
        instance = wgpu::CreateInstance(&instanceDesc);

        // 2) 请求 Adapter（获取 GPU 适配器）
        co_await RequestAdapterAsync();
        // 3) 请求 Device（获取用于创建资源的设备对象）
        co_await RequestDeviceAsync();
    }

    // ------------------------------------------------------------
    // CreateSurface：把 GLFW 窗口转换为 WebGPU 显示目标
    // ------------------------------------------------------------
    void Context::CreateSurface(GLFWwindow *window)
    {
        // Dawn 提供了 GLFW 适配层，内部会处理各平台（X11/Wayland/Win32/Cocoa）差异
        surface = wgpu::glfw::CreateSurfaceForWindow(instance, window);
    }

    // ------------------------------------------------------------
    // ConfigureSurface：选择格式 + 尺寸，并绑定 Device
    // ------------------------------------------------------------
    void Context::ConfigureSurface()
    {
        ConfigureSurface(kWidth, kHeight);
    }

    void Context::ConfigureSurface(uint32_t width, uint32_t height)
    {
        if (!surface)
        {
            throw std::runtime_error("Surface 尚未创建，无法配置");
        }

        // 查询当前 Adapter/Surface 组合支持的能力（可用格式、呈现模式等）
        wgpu::SurfaceCapabilities capabilities;
        surface.GetCapabilities(adapter, &capabilities);
        if (capabilities.formatCount == 0)
        {
            throw std::runtime_error("Surface 不支持任何可用的纹理格式");
        }
        // 取第一种推荐格式（通常是 BGRA8Unorm 或 RGBA8Unorm）
        format = capabilities.formats[0];

        wgpu::SurfaceConfiguration config{
            .device = device,
            .format = format,
            .width = width,
            .height = height
        };
        surface.Configure(&config);
    }

    // ------------------------------------------------------------
    // UnconfigureSurface：退出前解绑，避免资源/驱动层告警
    // ------------------------------------------------------------
    void Context::UnconfigureSurface() const
    {
        if (surface)
        {
            surface.Unconfigure();
        }
    }

    // ------------------------------------------------------------
    // RequestAdapterAsync：请求 GPU 适配器
    // ------------------------------------------------------------
    task<void> Context::RequestAdapterAsync()
    {
        // RequestAdapter 传入一个回调：成功把 adapter 放进 adapter_result_，
        // 失败则放入异常。
        wgpu::Future future = instance.RequestAdapter(
            nullptr, wgpu::CallbackMode::WaitAnyOnly,
            [this](wgpu::RequestAdapterStatus status, wgpu::Adapter a,
                   wgpu::StringView message)
            {
                if (status != wgpu::RequestAdapterStatus::Success)
                {
                    adapter_result_.set_exception(std::make_exception_ptr(
                        std::runtime_error(std::format("RequestAdapter 失败: {}",
                                                       std::string(message)))));
                    return;
                }
                adapter_result_.set(std::move(a));
            });

        // 阻塞等待回调执行完毕（回调会在 WaitAny 内部被调用）
        instance.WaitAny(future, UINT64_MAX);
        // 回调已将结果写入 async_value，此处 co_await 会立即就绪并取回 Adapter
        adapter = co_await adapter_result_;
    }

    // ------------------------------------------------------------
    // RequestDeviceAsync：请求逻辑设备
    // ------------------------------------------------------------
    task<void> Context::RequestDeviceAsync()
    {
        // 科研图像（显微镜、工业 CT、全切片）常超过 WebGPU 默认的 8192 边长。
        // 先查 adapter 上限，再请求一个不超过它的值；超过部分由渲染器明确报错。
        wgpu::Limits adapter_limits{};
        if (adapter.GetLimits(&adapter_limits) != wgpu::Status::Success ||
            adapter_limits.maxTextureDimension2D == wgpu::kLimitU32Undefined)
        {
            throw std::runtime_error("无法查询 adapter 的纹理尺寸上限");
        }
        constexpr uint32_t kPreferredTextureDimension = 16384;
        wgpu::Limits required{};
        required.maxTextureDimension2D =
            std::min(adapter_limits.maxTextureDimension2D, kPreferredTextureDimension);
        max_texture_dimension = required.maxTextureDimension2D;

        wgpu::DeviceDescriptor desc{};
        desc.requiredLimits = &required;
        // 未捕获错误回调：GPU 校验失败、内存错误等都会输出到这里，便于调试
        desc.SetUncapturedErrorCallback(
            [](const wgpu::Device &, wgpu::ErrorType errorType, wgpu::StringView message)
            {
                std::cout << "Error: " << int(errorType) << " - " << std::string(message) << "\n";
            });
        // 设备丢失回调：显式设置以消除 Dawn 的启动告警。
        // 正常退出时设备会被销毁（reason == Destroyed），无需记录。
        desc.SetDeviceLostCallback(
            wgpu::CallbackMode::AllowSpontaneous,
            [](const wgpu::Device &, wgpu::DeviceLostReason reason, wgpu::StringView message)
            {
                if (reason != wgpu::DeviceLostReason::Destroyed)
                {
                    std::cout << "Device lost: reason=" << int(reason) << " - "
                              << std::string(message) << "\n";
                }
            });

        wgpu::Future future = adapter.RequestDevice(
            &desc, wgpu::CallbackMode::WaitAnyOnly,
            [this](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView message)
            {
                if (status != wgpu::RequestDeviceStatus::Success)
                {
                    device_result_.set_exception(std::make_exception_ptr(
                        std::runtime_error(std::format("RequestDevice 失败: {}",
                                                       std::string(message)))));
                    return;
                }
                device_result_.set(std::move(d));
            });
        instance.WaitAny(future, UINT64_MAX);
        device = co_await device_result_;
    }

} // namespace voilut
