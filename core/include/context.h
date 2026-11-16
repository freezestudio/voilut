/// @file context.h
/// @brief WebGPU 环境（Instance / Adapter / Device / Surface）。
/// @details
/// WebGPU 的初始化是一条逐层创建对象的流程，对象关系如下：
///
/// @code{.unparsed}
/// Instance（入口，对应整个 GPU 实现）
///    └─ Adapter（适配器，对应某块物理/逻辑 GPU）
///         └─ Device（设备，负责创建资源、提交命令）
///
/// Surface（显示目标）负责把渲染结果显示到屏幕上。
/// @endcode
///
/// 这里把"请求 Adapter / 请求 Device"写成协程，用同步的写法
/// （co_await）描述异步流程，避免层层嵌套的回调。

#pragma once

#include <cstdint>

#include <webgpu/webgpu_cpp.h> // Dawn 提供的 WebGPU C++ 封装

#include "coroutine.h" // task<T> / async_value<T> / sync_wait

// 只用到 GLFWwindow*，不需要包含整个 GLFW 头文件（前向声明即可）
struct GLFWwindow;

namespace voilut
{
    /// @brief WebGPU 环境：持有 Instance / Adapter / Device / Surface。
    class Context
    {
    public:
        /// @brief 协程版初始化：创建 Instance → 请求 Adapter → 请求 Device。
        /// @return 可被 co_await 的 `task<void>`。
        /// @note 调用者必须 co_await 它，或者用 `sync_wait()` 驱动它执行。
        /// @note `[[nodiscard("原因")]]` 是 C++23 特性：忽略返回值会给出带原因警告。
        [[nodiscard("Context::InitAsync 必须被 co_await 或以其他方式驱动")]]
        task<void> InitAsync();

        /// @brief 把 GLFW 窗口包装成 WebGPU 的 Surface（显示目标）。
        /// @param window 目标窗口。
        void CreateSurface(GLFWwindow *window);

        /// @brief 给 Surface 选择格式 / 尺寸，并和 Device 绑定（使用默认窗口尺寸）。
        /// @pre 必须在获取 Device 之后调用。
        void ConfigureSurface();

        /// @brief 功能相同，但显式指定尺寸；窗口缩放后用它重新配置 Surface。
        /// @param width 目标宽度（像素）。
        /// @param height 目标高度（像素）。
        void ConfigureSurface(uint32_t width, uint32_t height);

        /// @brief 程序退出前解绑 Surface，避免资源泄漏。
        void UnconfigureSurface() const;

        /// @brief 静态工厂（推荐入口）。
        /// @details 内部用 `sync_wait` 将 InitAsync 协程一次性执行完毕，
        ///          这样 main() 中不需要写协程代码，也能获取初始化完成的 Context。
        /// @return 初始化完成的 `Context`。
        /// @note `sync_wait` 会阻塞到初始化完成；若失败会抛异常，由 main 捕获。
        [[nodiscard]] static Context create()
        {
            Context ctx;
            // sync_wait 会阻塞到初始化完成；若失败会抛异常，由 main 捕获
            sync_wait(ctx.InitAsync());
            return ctx;
        }

        /// @brief 取 Surface（deducing this，C++23）。
        /// @details 模板用 `this Self&&` 同时覆盖 const / 非 const 两种调用，
        ///          无需重复编写重载。
        /// @return 成员 `surface` 的转发引用。
        template <typename Self>
        auto &&get_surface(this Self &&self)
        {
            return std::forward<Self>(self).surface;
        }

        /// @brief 取纹理格式（deducing this，C++23）。
        /// @return 成员 `format` 的值。
        template <typename Self>
        auto get_format(this Self &&self)
        {
            return std::forward<Self>(self).format;
        }

    public:
        // ---- 核心对象：顺序即生命周期依赖，device 必须在 adapter 之后 ----
        wgpu::Instance instance;                                     ///< WebGPU 入口。
        wgpu::Adapter adapter;                                       ///< 适配器（某块 GPU）。
        wgpu::Device device;                                         ///< 设备（创建资源、提交命令）。
        wgpu::Surface surface;                                       ///< 显示表面。
        wgpu::TextureFormat format = wgpu::TextureFormat::Undefined; ///< 表面纹理格式。

    private:
        /// @name 内部异步步骤
        /// @details 供 InitAsync 依次 co_await。
        /// @{
        [[nodiscard("Context::RequestAdapterAsync 必须被 co_await")]]
        task<void> RequestAdapterAsync();
        [[nodiscard("Context::RequestDeviceAsync 必须被 co_await")]]
        task<void> RequestDeviceAsync();
        /// @}

    private:
        async_value<wgpu::Adapter> adapter_result_; ///< 异步 Adapter 结果容器。
        async_value<wgpu::Device> device_result_;   ///< 异步 Device 结果容器。
    };

} // namespace voilut
