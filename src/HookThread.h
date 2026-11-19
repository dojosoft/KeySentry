#pragma once
#include <Windows.h>
#include <atomic>
#include <functional>
#include <memory>

// ============================================================
// HookThread - 低级钩子专用线程
// 低级键盘/鼠标钩子（WH_KEYBOARD_LL / WH_MOUSE_LL）的回调在
// “安装钩子的线程”上执行，并受系统 LowLevelHooksTimeout 限制。
// 若与 UI 线程共用，UI 中的跨进程等待（隐藏窗口、读取标题、
// 音频 COM 调用等）会让回调超时，Windows 7+ 会静默移除钩子，
// 表现为按键屏蔽/映射/老板键全部失效且无任何提示。
//
// 本类把 SetWindowsHookEx 与消息泵放到独立线程：
//   - 钩子回调始终在该线程执行，不受 UI 线程阻塞影响；
//   - UI 线程只负责接收钩子投递的消息并处理后续业务。
//
// 生命周期约定（R8-H5）：
//   线程所需的全部状态放在堆上共享的 Context 中，由钩子线程自己持有；
//   UI 侧只保留 shared_ptr。即使启动超时/停止失败后本对象被复用或析构，
//   钩子线程也不会访问已释放的 HookThread 成员，避免 use-after-free。
//   就绪通知使用原子标志轮询，不跨线程共享事件句柄，消除“超时后关闭
//   句柄、线程随后 SetEvent”的句柄竞态。
// ============================================================
class HookThread {
public:
    HookThread() = default;
    // 析构时尽力停止线程；即使 Stop 超时，线程上下文仍由线程自身持有并释放，
    // 本对象可以直接销毁（与旧实现不同，不再依赖“析构前必须成功 Stop”）。
    // 使用较短的有界等待：退出阶段不能为异常线程长时间挂起（R8-H5）
    ~HookThread() {
        Stop(1500);
        // 放弃的线程可能仍在运行：关闭句柄即可，线程本身由系统回收
        if (m_abandonedThread) {
            CloseHandle(m_abandonedThread);
            m_abandonedThread = nullptr;
        }
    }
    HookThread(const HookThread&) = delete;
    HookThread& operator=(const HookThread&) = delete;

    // 启动线程并在线程内安装钩子；install 返回钩子句柄（失败返回 nullptr）。
    // abandonCleanup 在线程被 UI 放弃所有权后、卸载钩子前执行：它只能捕获
    // 与宿主解耦的共享状态，用于补发按键释放等必要收尾（R15-H02）。
    // 返回 true 表示安装成功。
    bool Start(std::function<HHOOK()> install, std::function<void(HHOOK)> uninstall,
               std::function<void()> abandonCleanup = {}) {
        if (!install || !uninstall) return false;
        // 上一个线程未能确认退出时先放弃它：旧线程的上下文独立，
        // 放弃后其后续执行不会影响新线程
        Stop(1000);
        // 旧线程仍未退出时拒绝创建新线程：两个低级钩子同时工作会重复
        // 拦截/注入按键。保留句柄，等下一轮 Start 前回收（R9-A2）
        if (!ReapAbandonedThread()) {
            OutputDebugStringW(L"[KeySentry] 旧钩子线程尚未退出，暂不创建新线程\n");
            return false;
        }

        auto ctx = std::make_shared<Context>();
        ctx->install = std::move(install);
        ctx->uninstall = std::move(uninstall);
        ctx->abandonCleanup = std::move(abandonCleanup);

        // 线程参数持有 shared_ptr 的副本：线程退出前上下文始终有效
        auto* holder = new std::shared_ptr<Context>(ctx);
        m_thread = CreateThread(nullptr, 0, ThreadProc, holder, 0, &m_tid);
        if (!m_thread) {
            delete holder;
            m_tid = 0;
            return false;
        }
        m_ctx = std::move(ctx);

        // 有界等待安装结果（轮询原子标志，避免事件句柄跨线程生命周期问题）
        const ULONGLONG deadline = GetTickCount64() + 3000;
        while (!m_ctx->ready.load(std::memory_order_acquire)) {
            // 线程提前退出（异常/安装失败）时无需再等满超时
            if (WaitForSingleObject(m_thread, 10) != WAIT_TIMEOUT) break;
            if (GetTickCount64() >= deadline) break;
        }
        if (!m_ctx->ready.load(std::memory_order_acquire) ||
            !m_ctx->installed.load(std::memory_order_acquire)) {
            // 启动失败或超时：请求线程退出并放弃所有权（不关闭任何线程仍在
            // 访问的句柄/对象），上下文由线程自身回收
            Abandon();
            return false;
        }
        return true;
    }

    // 停止线程：线程收到 WM_QUIT 后先卸载钩子再退出。
    // 返回 false 表示无法在 timeoutMs 内确认线程退出：句柄会被保留用于
    // 后续探活（Start 前确认旧线程已退出），线程退出时自行释放上下文。
    bool Stop(DWORD timeoutMs = 5000) {
        if (!m_ctx && !m_thread) return true;

        // 先置停止标志再投递 WM_QUIT：线程消息队列尚未建立时投递会失败，
        // 线程会在建立队列后检查该标志并立即退出，避免漏掉停止请求
        if (m_ctx) m_ctx->stopRequested.store(true, std::memory_order_release);

        bool exited = true;
        if (m_thread) {
            if (WaitForSingleObject(m_thread, 0) == WAIT_TIMEOUT) {
                exited = false;
                if (m_tid) PostThreadMessageW(m_tid, WM_QUIT, 0, 0);
                exited = WaitForSingleObject(m_thread, timeoutMs) == WAIT_OBJECT_0;
            }
            if (!exited) {
                // 线程仍可能访问其 Context：保留句柄用于后续探活，
                // 避免下一次 Start 在旧线程未退出时创建第二个钩子线程（R9-A2）。
                // 同时标记放弃，线程退出时不再回调捕获宿主 this 的卸载闭包（R13-M2）
                if (m_ctx) m_ctx->abandoned.store(true, std::memory_order_release);
                // 此处槽位里的旧句柄必然已退出（Start 已回收），可直接关闭
                if (m_abandonedThread) CloseHandle(m_abandonedThread);
                m_abandonedThread = m_thread;
            } else {
                CloseHandle(m_thread);
            }
            m_thread = nullptr;
        }
        m_tid = 0;
        m_ctx.reset();
        if (!exited) {
            OutputDebugStringW(L"[KeySentry] HookThread 停止超时，放弃线程所有权\n");
        }
        return exited;
    }

    bool Installed() const {
        return m_ctx && m_ctx->installed.load(std::memory_order_acquire);
    }

private:
    // 钩子线程的全部共享状态：由线程与 UI 共同持有（shared_ptr），
    // 线程退出后最后一个引用释放，不需要 UI 线程参与回收
    struct Context {
        std::atomic<bool> ready{false};
        std::atomic<bool> installed{false};
        std::atomic<bool> stopRequested{false};
        // UI 侧已放弃线程所有权（停止超时/启动失败）。
        // 此时宿主对象随时可能析构，线程退出时不得回调捕获宿主 this 的
        // uninstall 闭包，只能直接卸载钩子（R13-M2）
        std::atomic<bool> abandoned{false};
        std::function<HHOOK()> install;
        std::function<void(HHOOK)> uninstall;
        // UI 侧放弃所有权后执行的清理：不得捕获宿主对象，只能使用共享状态
        std::function<void()> abandonCleanup;
        HHOOK hook = nullptr;  // 仅钩子线程读写
    };

    // 放弃当前线程：通知退出后立即释放 UI 侧引用，不等待线程结束
    void Abandon() {
        if (m_ctx) {
            m_ctx->stopRequested.store(true, std::memory_order_release);
            // 宿主对象可能在返回后立即析构：线程退出时禁止回调其卸载闭包（R13-M2）
            m_ctx->abandoned.store(true, std::memory_order_release);
        }
        if (m_thread) {
            // 仅在确认线程仍在运行时投递，避免把 WM_QUIT 发给被系统复用的线程 ID
            if (WaitForSingleObject(m_thread, 0) == WAIT_TIMEOUT && m_tid) {
                PostThreadMessageW(m_tid, WM_QUIT, 0, 0);
            }
            // 给线程一个很短的收尾机会；仍在运行则保留句柄，
            // 后续 Start 会拒绝在旧线程未退出时创建第二个钩子线程（R9-A2）
            if (WaitForSingleObject(m_thread, 200) == WAIT_TIMEOUT) {
                if (m_abandonedThread) CloseHandle(m_abandonedThread);
                m_abandonedThread = m_thread;
            } else {
                CloseHandle(m_thread);
            }
            m_thread = nullptr;
        }
        m_tid = 0;
        m_ctx.reset();
    }

    // 回收已退出的旧线程句柄；旧线程仍在运行则返回 false（调用方不得创建新线程）
    bool ReapAbandonedThread() {
        if (!m_abandonedThread) return true;
        if (WaitForSingleObject(m_abandonedThread, 0) != WAIT_OBJECT_0) return false;
        CloseHandle(m_abandonedThread);
        m_abandonedThread = nullptr;
        return true;
    }

    static DWORD WINAPI ThreadProc(LPVOID param) {
        // 线程独占持有上下文引用：UI 侧放弃/析构 HookThread 后，
        // 这里访问的仍是有效内存
        std::unique_ptr<std::shared_ptr<Context>> holder(
            static_cast<std::shared_ptr<Context>*>(param));
        const std::shared_ptr<Context> ctx = *holder;
        holder.reset();

        // 先建立线程消息队列：低级钩子回调依赖它，也保证 PostThreadMessage 可用
        MSG msg = {};
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        if (!ctx->stopRequested.load(std::memory_order_acquire)) {
            ctx->hook = ctx->install ? ctx->install() : nullptr;
            ctx->installed.store(ctx->hook != nullptr, std::memory_order_release);
        }
        ctx->ready.store(true, std::memory_order_release);
        if (!ctx->hook) return 0;

        // 再次检查停止请求：Stop 可能在消息队列建立前投递失败且已置位标志
        while (!ctx->stopRequested.load(std::memory_order_acquire) &&
               GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (ctx->hook) {
            if (!ctx->abandoned.load(std::memory_order_acquire) && ctx->uninstall) {
                ctx->uninstall(ctx->hook);
            } else {
                // UI 侧已放弃所有权：卸载闭包仍捕获宿主对象，不能回调
                // （宿主可能在等待超时后已析构）。但按键释放等收尾只依赖
                // 共享状态，必须在这里执行，否则目标键会一直保持按下（R15-H02）
                if (ctx->abandonCleanup) ctx->abandonCleanup();
                UnhookWindowsHookEx(ctx->hook);
            }
            ctx->hook = nullptr;
        }
        ctx->installed.store(false, std::memory_order_release);
        return 0;
    }

    std::shared_ptr<Context> m_ctx;
    HANDLE m_thread = nullptr;
    DWORD m_tid = 0;
    // 已放弃但可能仍在运行的旧线程句柄：仅用于下次 Start 前确认其已退出，
    // 防止两个低级钩子线程同时工作（R9-A2）
    HANDLE m_abandonedThread = nullptr;
};
