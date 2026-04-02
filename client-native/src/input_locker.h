#pragma once
/**
 * input_locker.h — 键鼠锁定模块（用户态低级钩子实现）
 *
 * 功能：通过 WH_KEYBOARD_LL + WH_MOUSE_LL 全局钩子拦截键盘和鼠标输入。
 * 使用可配置的复杂热键组合（如 Ctrl+Alt+Shift+F12）来切换锁定/解锁。
 *
 * 限制：
 *   - 无法拦截 Ctrl+Alt+Del（Windows 安全机制）
 *   - 钩子必须在有消息循环的线程中安装
 *
 * 线程安全：Install/Uninstall 必须在 UI 线程调用。
 *          SetHotkey/Toggle/IsLocked 可从任意线程调用。
 */

#include <windows.h>
#include <string>
#include <functional>
#include <atomic>
#include <cstdint>

namespace MDShare {

/**
 * 热键修饰符（可组合）
 */
enum HotkeyModifier : uint32_t {
    MOD_NONE    = 0,
    HMOD_CTRL   = 1 << 0,
    HMOD_ALT    = 1 << 1,
    HMOD_SHIFT  = 1 << 2,
    HMOD_WIN    = 1 << 3,
};

/**
 * 热键定义
 */
struct HotkeyCombo {
    uint32_t modifiers = HMOD_CTRL | HMOD_ALT | HMOD_SHIFT;  // 默认: Ctrl+Alt+Shift
    DWORD    vkCode    = VK_F12;                               // 默认: F12

    /** 从字符串解析，格式: "Ctrl+Alt+Shift+F12" */
    static HotkeyCombo FromString(const std::string& str);

    /** 序列化为字符串 */
    std::string ToString() const;
};

class InputLocker {
public:
    using StateCallback = std::function<void(bool locked)>;
    using ViewerComboCallback = std::function<void(const std::string& combo)>;

    InputLocker();
    ~InputLocker();

    // 禁止拷贝
    InputLocker(const InputLocker&) = delete;
    InputLocker& operator=(const InputLocker&) = delete;

    /**
     * 安装低级钩子。必须在有消息循环的线程（UI 线程）调用。
     * @return true 安装成功
     */
    bool Install();

    /**
     * 卸载钩子并解除锁定。
     */
    void Uninstall();

    /**
     * 设置解锁/锁定的热键组合。
     */
    void SetHotkey(const HotkeyCombo& combo);

    /**
     * 获取当前热键。
     */
    HotkeyCombo GetHotkey() const;

    /**
     * 切换锁定状态。
     */
    void Toggle();

    /**
     * 锁定键鼠。
     */
    void Lock();

    /**
     * 解锁键鼠。
     */
    void Unlock();

    /**
     * 是否已锁定。
     */
    bool IsLocked() const;

    /**
     * 钩子是否已安装。
     */
    bool IsInstalled() const;

    /**
     * 注册状态变化回调（锁定/解锁时触发）。
     */
    void OnStateChanged(StateCallback cb);

    /**
     * 启用 Viewer 端系统键拦截并转发模式。
     */
    void EnableViewerForward();

    /**
     * 禁用 Viewer 端系统键拦截并转发模式。
     */
    void DisableViewerForward();

    /**
     * Viewer 转发模式是否启用。
     */
    bool IsViewerForwardEnabled() const;

    /**
     * 注册 Viewer 端组合键转发回调。
     */
    void OnViewerCombo(ViewerComboCallback cb);

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);

    /** 检查当前按键事件是否匹配热键组合 */
    bool CheckHotkeyMatch(DWORD vkCode, bool isKeyDown);
    bool ShouldForwardViewerCombo(DWORD vkCode, bool isKeyDown, bool isKeyUp, std::string& comboOut);

    /** 锁定/解锁时释放所有 OS 级别修饰键，防止粘键 */
    void ReleaseModifiersForLock();

    HHOOK m_keyboardHook = nullptr;
    HHOOK m_mouseHook    = nullptr;

    std::atomic<bool> m_locked{false};
    std::atomic<bool> m_installed{false};

    HotkeyCombo   m_hotkey;
    StateCallback m_callback;
    ViewerComboCallback m_viewerComboCallback;

    std::atomic<bool> m_viewerForwardEnabled{false};
    std::atomic<bool> m_lwinDown{false};
    std::atomic<bool> m_rwinDown{false};
    /** Win 键延迟发送标记：为 true 代表 Win 已经参与了组合键，keyup 时不再发送单独 "win" */
    std::atomic<bool> m_winUsedInCombo{false};

    // 修饰键跟踪
    std::atomic<bool> m_ctrlDown{false};
    std::atomic<bool> m_altDown{false};
    std::atomic<bool> m_shiftDown{false};
    std::atomic<bool> m_winDown{false};

    // 用于钩子的 static 访问
    static InputLocker* s_instance;
};

} // namespace MDShare
