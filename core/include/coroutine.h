/// @file coroutine.h
/// @brief C++20/26 协程工具（轻量协程库）。
/// @details
/// 本文件实现协程原语，共包含三个组件：
///
///   1) `task<T>`        可被 co_await 的"任务"（惰性启动：创建时不执行）
///   2) `async_value<T>` 由回调驱动的"异步值"（外部 set 后恢复等待者）
///   3) `sync_wait()`    在 main 等普通函数中驱动 task 直至结束
///
/// 【阅读顺序】
///   a. 先看 `task<void>::promise_type`：理解编译器要求协程提供哪些钩子函数；
///   b. 再看 `task<void>::awaitable` / `operator co_await`：理解 co_await 三步
///      （await_ready → await_suspend → await_resume）；
///   c. 然后看 `async_value<T>`：它是连接回调与协程的适配层；
///   d. 最后看 `sync_wait()`：驱动整条调用链。
///
/// 【关键概念：协程句柄 coroutine_handle】
///   协程被调用时并不立即执行函数体，而是先分配一块"协程帧"（保存局部变量、
///   参数、当前暂停位置），然后返回一个对象。`std::coroutine_handle` 就是
///   指向这块帧的操作入口：`resume()` 继续执行、`done()` 判断是否结束、
///   `destroy()` 释放帧内存、`promise()` 取回 promise 对象。

#pragma once

// ------------------------------------------------------------
// C++26 的 "= delete(原因)"（P2573）用于在编译错误里写明删除原因，
// 但 MSVC 19.51 尚未实现（会报 C2059 语法错误）。这里用宏包裹：
//   - MSVC      ：退化为普通 "= delete"（不保留原因文本）
//   - GCC/Clang ：保留 "= delete(原因)"
// ------------------------------------------------------------
#if defined(_MSC_VER)
#define APP_DELETE_REASON(reason) = delete
#else
#define APP_DELETE_REASON(reason) = delete (reason)
#endif

#include <coroutine>   // std::coroutine_handle / suspend_always
#include <exception>   // std::exception_ptr / std::current_exception
#include <utility>     // std::move / std::exchange / std::forward
#include <type_traits> // std::is_void_v（sync_wait 用）

namespace voilut
{

    /// @brief 有返回值的惰性协程任务。
    /// @details
    /// 用法：
    /// @code{.cpp}
    /// task<int> f() { co_return 42; }
    /// task<void> g() { co_await f(); }
    /// @endcode
    ///
    /// "惰性"的含义：调用 `f()` 仅创建协程帧并停在 `initial_suspend`，
    /// 必须有调用方 co_await 它（或由 `sync_wait` 驱动）才会实际执行。
    template <typename T>
    class task
    {
    public:
        /// @brief 协程的"控制块"。
        /// @details 编译器会在协程帧里放一个 `promise_type` 对象，并通过它：
        ///   - 获取返回给调用者的对象（`get_return_object`）
        ///   - 决定开始 / 结束时是否暂停（`initial_suspend` / `final_suspend`）
        ///   - 接收 `co_return` 的值（`return_value`）
        ///   - 接收未捕获异常（`unhandled_exception`）
        struct promise_type
        {
            T value{};              ///< `co_return` 的值最终存这里。
            std::exception_ptr exc; ///< 协程体内抛出的异常暂存于此。

            /// @brief 协程创建后立刻调用。
            /// @return 调用 `f()` 得到的 `task` 对象。
            task get_return_object() noexcept
            {
                return task{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            /// @brief 开始即暂停：保证 task 具有惰性，创建时不执行任何代码。
            std::suspend_always initial_suspend() noexcept { return {}; }
            /// @brief 结束也暂停：把销毁时机交给 task 的析构函数，避免帧被提前释放。
            std::suspend_always final_suspend() noexcept { return {}; }

            /// @brief `co_return v;` 时调用。
            void return_value(T v) noexcept { value = std::move(v); }
            /// @brief 协程体内任何未捕获异常都会到这里，先存起来，稍后由 `await_resume` 重新抛出。
            void unhandled_exception() noexcept { exc = std::current_exception(); }
        };

    private:
        /// @brief co_await 一个 task 时实际参与"三步协议"的对象。
        struct awaitable
        {
            std::coroutine_handle<promise_type> handle; ///< 被等待任务的句柄。
            T &ref;                                     ///< 任务结果引用。
            std::exception_ptr &exc_ref;                ///< 任务异常引用。

            awaitable(std::coroutine_handle<promise_type> h, T &r, std::exception_ptr &e)
                : handle(h), ref(r), exc_ref(e) {}

            /// @brief 第 1 步：任务是否已经完成？完成就直接取结果，不挂起。
            bool await_ready() const noexcept { return handle.done(); }

            /// @brief 第 2 步：需要挂起时调用，直接 resume 被等待的任务。
            /// @details 于是"当前协程挂起、另一个协程开始运行"；参数是"当前协程"
            ///          的句柄，此处未使用（丢弃）。
            void await_suspend(std::coroutine_handle<>) noexcept { handle.resume(); }

            /// @brief 第 3 步：任务结束后恢复执行时调用。
            /// @return `co_await` 表达式的结果。
            T await_resume()
            {
                if (exc_ref)
                    std::rethrow_exception(exc_ref);
                return std::move(ref);
            }
        };

    public:
        explicit task(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}

        /// @brief 析构时销毁协程帧，释放内存（RAII 思想）。
        ~task()
        {
            if (handle_)
                handle_.destroy();
        }

        // 协程帧是独占资源：拷贝会导致两次 destroy，必须禁止。
        // 这里用 C++26 的 "= delete(说明)" 直接写明删除原因（编译报错更明确）。
        task(const task &) APP_DELETE_REASON("copy is ill-formed for coroutine handles");
        task &operator=(const task &) APP_DELETE_REASON("copy assignment is ill-formed");

        /// @brief 允许移动（转移所有权），源对象句柄置空避免重复销毁。
        task(task &&o) noexcept : handle_(std::exchange(o.handle_, {})) {}
        task &operator=(task &&o) noexcept
        {
            if (this != &o)
            {
                if (handle_)
                    handle_.destroy();
                handle_ = std::exchange(o.handle_, {});
            }
            return *this;
        }

        /// @brief co_await task 时编译器会调用它得到 `awaitable`。
        awaitable operator co_await() noexcept
        {
            return awaitable{handle_, handle_.promise().value, handle_.promise().exc};
        }

        /// @brief 任务是否已结束。
        bool done() const noexcept { return handle_ && handle_.done(); }
        /// @brief 取任务结果。
        T &result() { return handle_.promise().value; }

        /// @brief 取内部协程句柄（deducing this，C++23）。
        /// @details 模板用 `this Self&&` 同时覆盖 const / 非 const 对象，
        ///          无需重复编写重载；供 `sync_wait` 驱动。
        /// @return 成员 `handle_` 的转发引用。
        template <typename Self>
        auto &&get(this Self &&self)
        {
            return std::forward<Self>(self).handle_;
        }

    private:
        std::coroutine_handle<promise_type> handle_;
    };

    /// @brief `task<void>` 特化：没有返回值的协程。
    /// @details 与非 void 版本基本相同，只有两点差别：
    ///   - 用 `return_void()` 代替 `return_value()`
    ///   - `await_resume()` / `result()` 不返回值
    template <>
    class task<void>
    {
    public:
        struct promise_type
        {
            std::exception_ptr exc; ///< 未捕获异常暂存位置。

            task get_return_object() noexcept
            {
                return task{std::coroutine_handle<promise_type>::from_promise(*this)};
            }
            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_always final_suspend() noexcept { return {}; }
            void return_void() noexcept {} ///< `co_return;` 时调用。
            void unhandled_exception() noexcept { exc = std::current_exception(); }
        };

        struct awaitable
        {
            std::coroutine_handle<promise_type> handle; ///< 被等待任务的句柄。
            std::exception_ptr &exc_ref;                ///< 任务异常引用。

            awaitable(std::coroutine_handle<promise_type> h, std::exception_ptr &e)
                : handle(h), exc_ref(e) {}

            bool await_ready() const noexcept { return handle.done(); }
            void await_suspend(std::coroutine_handle<>) noexcept { handle.resume(); }
            void await_resume()
            {
                if (exc_ref)
                    std::rethrow_exception(exc_ref);
            }
        };

        explicit task(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}
        ~task()
        {
            if (handle_)
                handle_.destroy();
        }

        task(const task &) APP_DELETE_REASON("task<void> is not copyable");
        task &operator=(const task &) APP_DELETE_REASON("task<void> is not copyable");
        task(task &&o) noexcept : handle_(std::exchange(o.handle_, {})) {}
        task &operator=(task &&o) noexcept
        {
            if (this != &o)
            {
                if (handle_)
                    handle_.destroy();
                handle_ = std::exchange(o.handle_, {});
            }
            return *this;
        }

        awaitable operator co_await() noexcept
        {
            return awaitable{handle_, handle_.promise().exc};
        }
        bool done() const noexcept { return handle_ && handle_.done(); }
        void result() { /* void 任务无返回值 */ }

        /// @brief 取内部协程句柄（deducing this，同上）。
        template <typename Self>
        auto &&get(this Self &&self)
        {
            return std::forward<Self>(self).handle_;
        }

    private:
        std::coroutine_handle<promise_type> handle_;
    };

    /// @brief `task<void>` 的短别名。
    using unit = task<void>;

    /// @brief 可由外部 set 的异步值。
    /// @details
    /// 场景：WebGPU 的 RequestAdapter 是异步的，结果通过回调返回。
    /// 为了用同步写法 co_await 它，采用以下机制：
    ///   1) 协程 co_await `async_value`，若值未就绪，就将自己的句柄存入 `caller_`；
    ///   2) 回调触发时调用 `set(value)`，唤醒等待的协程。
    ///
    /// 这是回调与协程之间的适配层，也是本实现采用协程的关键。
    template <typename T>
    class async_value
    {
    public:
        /// @brief co_await 协议三步实现。
        struct awaiter
        {
            async_value *ptr = nullptr; ///< 指向所属的 `async_value`。

            /// @brief 值已就绪则直接继续，无需挂起。
            bool await_ready() const noexcept { return ptr->ready_; }

            /// @brief 未就绪：记录等待者，然后挂起（不 resume，等待 set 唤醒）。
            /// @param h 等待者协程句柄。
            void await_suspend(std::coroutine_handle<> h) noexcept { ptr->caller_ = h; }

            /// @brief 被 `set()` 唤醒后取结果；若之前存的是异常则重新抛出。
            /// @return 结果值（移动语义）。
            T await_resume()
            {
                if (ptr->exc_)
                    std::rethrow_exception(ptr->exc_);
                return std::move(ptr->value_);
            }
        };

        async_value() = default;
        async_value(const async_value &) APP_DELETE_REASON("async_value is not copyable");
        async_value &operator=(const async_value &) APP_DELETE_REASON("async_value is not copyable");
        async_value(async_value &&) = default;
        async_value &operator=(async_value &&) = default;

        /// @brief 外部（通常是 WebGPU 回调）调用：设置成功值并唤醒等待者。
        /// @param v 结果值。
        void set(T v)
        {
            value_ = std::move(v);
            ready_ = true;
            if (caller_)
                caller_.resume();
        }

        /// @brief 外部调用：设置异常并唤醒等待者。
        /// @param exc 异常指针。
        void set_exception(std::exception_ptr exc)
        {
            exc_ = exc;
            ready_ = true;
            if (caller_)
                caller_.resume();
        }

        /// @brief 得到 `awaiter`，使 `async_value` 可被 co_await。
        awaiter operator co_await() noexcept { return awaiter{this}; }

    private:
        T value_{};                      ///< 结果值。
        bool ready_ = false;             ///< 是否已就绪。
        std::exception_ptr exc_;         ///< 若失败，错误信息。
        std::coroutine_handle<> caller_; ///< 正在等待的协程（未就绪时才有值）。
    };

    /// @brief `async_value<void>` 特化：只表示"完成 / 失败"，没有值。
    template <>
    class async_value<void>
    {
    public:
        struct awaiter
        {
            async_value *ptr = nullptr; ///< 指向所属的 `async_value`。
            bool await_ready() const noexcept { return ptr->ready_; }
            void await_suspend(std::coroutine_handle<> h) noexcept { ptr->caller_ = h; }
            void await_resume()
            {
                if (ptr->exc_)
                    std::rethrow_exception(ptr->exc_);
            }
        };

        async_value() = default;
        async_value(const async_value &) APP_DELETE_REASON("async_value is not copyable");
        async_value &operator=(const async_value &) APP_DELETE_REASON("async_value is not copyable");
        async_value(async_value &&) = default;
        async_value &operator=(async_value &&) = default;

        /// @brief 标记完成并唤醒等待者。
        void set()
        {
            ready_ = true;
            if (caller_)
                caller_.resume();
        }

        /// @brief 设置异常并唤醒等待者。
        /// @param exc 异常指针。
        void set_exception(std::exception_ptr exc)
        {
            exc_ = exc;
            ready_ = true;
            if (caller_)
                caller_.resume();
        }

        /// @brief 得到 `awaiter`，使 `async_value` 可被 co_await。
        awaiter operator co_await() noexcept { return awaiter{this}; }

    private:
        bool ready_ = false;             ///< 是否已就绪。
        std::exception_ptr exc_;         ///< 若失败，错误信息。
        std::coroutine_handle<> caller_; ///< 正在等待的协程。
    };

    /// @brief 在同步上下文里等待 task 结束。
    /// @details
    /// 由于 main() 不是协程，无法 co_await，因此需要手动"驱动"任务：
    /// 只要任务尚未完成，就调用一次 `resume()`。
    ///
    /// 为什么需要循环？因为 `awaitable::await_suspend()` 仅移交控制权，
    /// 当前协程会停在挂起点；再次 resume 它才会调用 `await_resume` 继续执行。
    /// 本实现中的异步操作实际上都在 `WaitAny()` 中同步完成，所以循环次数很少。
    ///
    /// 结束后若 promise 中存有异常，就地重新抛出，让调用者（main）可以捕获。
    ///
    /// @param t 要驱动的任务。
    /// @return 任务结果（`T` 为 void 时无返回值）。
    /// @throw 协程体内未捕获的异常。
    template <typename T>
    [[nodiscard("sync_wait 的结果/异常必须处理")]]
    T sync_wait(task<T> t)
    {
        auto handle = t.get(); // 获取协程句柄
        while (!handle.done())
        {
            handle.resume(); // 反复恢复，直到协程执行完毕
        }
        // 协程体内未捕获的异常被 promise 暂存，这里统一抛出
        if (handle.promise().exc)
        {
            std::rethrow_exception(handle.promise().exc);
        }
        if constexpr (!std::is_void_v<T>)
        {
            return std::move(t.result()); // 有返回值时将其返回
        }
    }

} // namespace voilut
