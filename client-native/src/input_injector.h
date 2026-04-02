#pragma once
/**
 * input_injector.h — 远程输入注入模块
 *
 * 功能：接收远端（观看端）发来的输入事件，通过 SendInput() 注入到本地系统。
 *
 * 参考：
 *   - Sunshine src/platform/windows/input.cpp（SendInput + 桌面切换）
 *   - RustDesk enigo win_impl.rs（dwExtraInfo 标记合成输入）
 *   - noVNC xtscancodes.js（HTML event.code → AT Set 1 扫描码映射）
 *
 * 坐标约定：鼠标归一化坐标 0.0~1.0 → 内部映射到 0~65535 绝对坐标
 *
 * 线程安全：InjectXxx 系列函数可从任意线程调用。
 *          Enable/Disable 在逻辑上由消息分发线程控制。
 */

#include <windows.h>
#include <cstdint>
#include <string>
#include <atomic>
#include <functional>
#include <thread>
#include <nlohmann/json.hpp>

namespace MDShare {

/**
 * 合成输入的 dwExtraInfo 标记值
 * InputLocker 的钩子通过此值识别并放行注入的事件
 */
constexpr ULONG_PTR INJECTED_INPUT_EXTRA = 0x4D445348; // "MDSH" 的十六进制

class InputInjector {
public:
    InputInjector() = default;
    ~InputInjector();

    // 禁止拷贝
    InputInjector(const InputInjector&) = delete;
    InputInjector& operator=(const InputInjector&) = delete;

    /** 启用远程控制（开始接受注入） */
    void Enable();

    /** 禁用远程控制（停止接受注入） */
    void Disable();

    /** 是否已启用 */
    bool IsEnabled() const;

    /**
     * 处理前端发来的输入事件 JSON
     * 消息格式参见 remote-control.js 中的定义
     */
    void HandleInputEvent(const nlohmann::json& msg);

    /**
     * 获取当前屏幕信息（分辨率、DPI、多显示器布局）
     */
    static nlohmann::json GetScreenInfo();

    // ── 底层注入 API ──

    /** 鼠标绝对移动（归一化坐标 0.0~1.0） */
    static void InjectMouseMoveAbsolute(double normX, double normY);

    /** 鼠标相对移动（像素增量） */
    static void InjectMouseMoveRelative(int dx, int dy);

    /** 鼠标按钮 (button: 0=左, 1=中, 2=右, 3=X1, 4=X2) */
    static void InjectMouseButton(int button, bool down);

    /** 鼠标滚轮（delta: 正=向上, 负=向下） */
    static void InjectMouseWheel(int deltaX, int deltaY);

    /** 键盘按键（使用扫描码模式，最可靠） */
    static void InjectKeyScancode(uint16_t scancode, bool extended, bool up);

    /** Unicode 文本输入 */
    static void InjectUnicodeText(const std::wstring& text);

    /** 特殊组合键（如 Ctrl+Alt+Del），通过 SAS 或模拟 */
    static void InjectSpecialCombo(const std::string& combo);

    /** 释放所有修饰键（防止远控开始/结束时粘键） */
    static void ReleaseAllModifiers();

    // ── 光标形状同步 ──

    using CursorCallback = std::function<void(const std::string& cursorType)>;

    /** 开始光标轮询（100ms间隔），形状变化时回调 */
    void StartCursorPolling(CursorCallback callback);

    /** 停止光标轮询 */
    void StopCursorPolling();

    // ── 被控端光标隐藏控制 ──

    /**
     * 设置被控端光标是否隐藏
     * @param hidden true=隐藏系统光标, false=恢复系统光标
     */
    void SetCursorHidden(bool hidden);

    /** 启动光标隐藏强制执行线程（RC 启用时调用） */
    void StartCursorHideEnforcement();

    /** 停止光标隐藏强制执行线程并恢复系统光标（RC 禁用时调用） */
    void StopCursorHideEnforcement();

private:
    std::atomic<bool> m_enabled{false};

    // 光标轮询
    std::atomic<bool> m_cursorPolling{false};
    std::thread m_cursorThread;
    std::string m_lastCursorType;
    CursorCallback m_cursorCallback;
    void _CursorPollLoop();
    static std::string _GetCurrentCursorType();

    // 光标隐藏
    std::atomic<bool> m_cursorShouldBeHidden{false};
    std::atomic<bool> m_cursorHideActive{false};
    std::thread m_cursorHideThread;
    HCURSOR m_blankCursor = nullptr;
    void _CursorHideLoop();
    static HCURSOR _CreateBlankCursor();
    void _ApplyBlankCursors();
    static void _RestoreCursors();

    /** 安全发送 INPUT，失败时尝试切换桌面后重试 (参考 Sunshine) */
    static void SendInputSafe(INPUT& input);

    /** 批量安全发送 */
    static void SendInputsSafe(INPUT* inputs, UINT count);
};

} // namespace MDShare
