/**
 * input_locker.cpp — 键鼠锁定模块实现
 *
 * 通过 SetWindowsHookEx(WH_KEYBOARD_LL / WH_MOUSE_LL) 全局拦截输入。
 * 锁定期间仅放行解锁热键组合，其余所有键盘鼠标事件被吞掉。
 */
#include "input_locker.h"
#include "input_injector.h"  // for INJECTED_INPUT_EXTRA
#include <algorithm>
#include <sstream>
#include <cctype>
#include <unordered_map>

namespace MDShare {

// ── 静态实例指针（钩子回调需要） ──

InputLocker* InputLocker::s_instance = nullptr;

// ── VK 名称映射 ──

static const std::unordered_map<std::string, DWORD>& GetNameToVK() {
    static const std::unordered_map<std::string, DWORD> map = {
        {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4},
        {"f5", VK_F5}, {"f6", VK_F6}, {"f7", VK_F7}, {"f8", VK_F8},
        {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
        {"f13", VK_F13}, {"f14", VK_F14}, {"f15", VK_F15}, {"f16", VK_F16},
        {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE},
        {"tab", VK_TAB}, {"space", VK_SPACE},
        {"insert", VK_INSERT}, {"delete", VK_DELETE},
        {"home", VK_HOME}, {"end", VK_END},
        {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT},
        {"pause", VK_PAUSE}, {"break", VK_PAUSE},
        {"scrolllock", VK_SCROLL}, {"numlock", VK_NUMLOCK},
        {"capslock", VK_CAPITAL},
        {"printscreen", VK_SNAPSHOT},
        // 数字键
        {"0", 0x30}, {"1", 0x31}, {"2", 0x32}, {"3", 0x33}, {"4", 0x34},
        {"5", 0x35}, {"6", 0x36}, {"7", 0x37}, {"8", 0x38}, {"9", 0x39},
        // 字母键
        {"a", 0x41}, {"b", 0x42}, {"c", 0x43}, {"d", 0x44}, {"e", 0x45},
        {"f", 0x46}, {"g", 0x47}, {"h", 0x48}, {"i", 0x49}, {"j", 0x4A},
        {"k", 0x4B}, {"l", 0x4C}, {"m", 0x4D}, {"n", 0x4E}, {"o", 0x4F},
        {"p", 0x50}, {"q", 0x51}, {"r", 0x52}, {"s", 0x53}, {"t", 0x54},
        {"u", 0x55}, {"v", 0x56}, {"w", 0x57}, {"x", 0x58}, {"y", 0x59},
        {"z", 0x5A},
        // 小键盘
        {"numpad0", VK_NUMPAD0}, {"numpad1", VK_NUMPAD1},
        {"numpad2", VK_NUMPAD2}, {"numpad3", VK_NUMPAD3},
        {"numpad4", VK_NUMPAD4}, {"numpad5", VK_NUMPAD5},
        {"numpad6", VK_NUMPAD6}, {"numpad7", VK_NUMPAD7},
        {"numpad8", VK_NUMPAD8}, {"numpad9", VK_NUMPAD9},
    };
    return map;
}

static const std::unordered_map<DWORD, std::string>& GetVKToName() {
    static std::unordered_map<DWORD, std::string> map;
    static bool init = false;
    if (!init) {
        // 仅保留首选名称
        for (auto& [name, vk] : GetNameToVK()) {
            if (map.find(vk) == map.end() || name.size() > map[vk].size()) {
                // 优先短名称，但避免 "esc" 覆盖 "escape" 等
                if (map.find(vk) == map.end()) map[vk] = name;
            }
        }
        // 强制首选名称
        map[VK_ESCAPE] = "Escape";
        map[VK_F1]  = "F1";  map[VK_F2]  = "F2";  map[VK_F3]  = "F3";
        map[VK_F4]  = "F4";  map[VK_F5]  = "F5";  map[VK_F6]  = "F6";
        map[VK_F7]  = "F7";  map[VK_F8]  = "F8";  map[VK_F9]  = "F9";
        map[VK_F10] = "F10"; map[VK_F11] = "F11"; map[VK_F12] = "F12";
        init = true;
    }
    return map;
}

// ── HotkeyCombo 辅助 ──

static std::string ToLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return r;
}

static std::string Trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    auto end   = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

HotkeyCombo HotkeyCombo::FromString(const std::string& str) {
    HotkeyCombo combo;
    combo.modifiers = MOD_NONE;
    combo.vkCode    = 0;

    std::istringstream ss(str);
    std::string token;
    while (std::getline(ss, token, '+')) {
        token = Trim(token);
        std::string lower = ToLower(token);

        if (lower == "ctrl" || lower == "control") {
            combo.modifiers |= HMOD_CTRL;
        } else if (lower == "alt") {
            combo.modifiers |= HMOD_ALT;
        } else if (lower == "shift") {
            combo.modifiers |= HMOD_SHIFT;
        } else if (lower == "win" || lower == "windows" || lower == "super" || lower == "meta") {
            combo.modifiers |= HMOD_WIN;
        } else {
            // 主键
            auto& nameMap = GetNameToVK();
            auto it = nameMap.find(lower);
            if (it != nameMap.end()) {
                combo.vkCode = it->second;
            }
        }
    }

    // 回退：如果解析失败则使用默认值
    if (combo.vkCode == 0) {
        combo.modifiers = HMOD_CTRL | HMOD_ALT | HMOD_SHIFT;
        combo.vkCode    = VK_F12;
    }

    return combo;
}

std::string HotkeyCombo::ToString() const {
    std::string result;
    if (modifiers & HMOD_CTRL)  result += "Ctrl+";
    if (modifiers & HMOD_ALT)   result += "Alt+";
    if (modifiers & HMOD_SHIFT) result += "Shift+";
    if (modifiers & HMOD_WIN)   result += "Win+";

    auto& vkMap = GetVKToName();
    auto it = vkMap.find(vkCode);
    if (it != vkMap.end()) {
        result += it->second;
    } else {
        // 未知键码，直接用数字
        result += "VK_" + std::to_string(vkCode);
    }
    return result;
}

// ── InputLocker 实现 ──

InputLocker::InputLocker() {
    // 默认热键: Ctrl+Alt+Shift+F12
    m_hotkey.modifiers = HMOD_CTRL | HMOD_ALT | HMOD_SHIFT;
    m_hotkey.vkCode    = VK_F12;
}

InputLocker::~InputLocker() {
    Uninstall();
}

bool InputLocker::Install() {
    if (m_installed.load()) return true;

    s_instance = this;

    m_keyboardHook = SetWindowsHookExW(
        WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
    if (!m_keyboardHook) {
        s_instance = nullptr;
        return false;
    }

    m_mouseHook = SetWindowsHookExW(
        WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
    if (!m_mouseHook) {
        UnhookWindowsHookEx(m_keyboardHook);
        m_keyboardHook = nullptr;
        s_instance = nullptr;
        return false;
    }

    m_installed.store(true);
    return true;
}

void InputLocker::Uninstall() {
    if (!m_installed.load()) return;

    // 确保解锁
    if (m_locked.load()) {
        m_locked.store(false);
        if (m_callback) m_callback(false);
    }

    if (m_keyboardHook) {
        UnhookWindowsHookEx(m_keyboardHook);
        m_keyboardHook = nullptr;
    }
    if (m_mouseHook) {
        UnhookWindowsHookEx(m_mouseHook);
        m_mouseHook = nullptr;
    }

    m_installed.store(false);
    if (s_instance == this) s_instance = nullptr;
}

void InputLocker::SetHotkey(const HotkeyCombo& combo) {
    m_hotkey = combo;
}

HotkeyCombo InputLocker::GetHotkey() const {
    return m_hotkey;
}

void InputLocker::Toggle() {
    if (m_locked.load()) {
        Unlock();
    } else {
        Lock();
    }
}

void InputLocker::Lock() {
    if (m_locked.exchange(true)) return; // 已经锁定
    // 重置 ShouldForwardViewerCombo 专用的 Win 键追踪
    // （锁定期间该函数被跳过，这些状态会变陈旧）
    m_lwinDown.store(false);
    m_rwinDown.store(false);
    m_winUsedInCombo.store(false);
    if (m_callback) m_callback(true);
    // 锁定时释放所有 OS 级别修饰键，防止锁定前按下的修饰键粘滞
    ReleaseModifiersForLock();
}

void InputLocker::Unlock() {
    if (!m_locked.exchange(false)) return; // 已经解锁
    // 重置修饰键内部追踪状态（解锁组合键的 keydown 在锁定期间被 CheckHotkeyMatch
    // 记录但未到达 ShouldForwardViewerCombo；若不清除，解锁后释放键时
    // ShouldForwardViewerCombo 会因 altDown/ctrlDown 残留而吞掉 keyup，
    // 导致一个孤立的 Alt keyup 到达 WebView2 触发 Chromium 菜单激活模式，
    // 使后续 JS 快捷键全部失效）
    m_ctrlDown.store(false);
    m_altDown.store(false);
    m_shiftDown.store(false);
    m_winDown.store(false);
    m_lwinDown.store(false);
    m_rwinDown.store(false);
    m_winUsedInCombo.store(false);
    // 发送合成 keyup 清理 OS / WebView2 级别的修饰键状态
    ReleaseModifiersForLock();
    if (m_callback) m_callback(false);
}

bool InputLocker::IsLocked() const {
    return m_locked.load();
}

bool InputLocker::IsInstalled() const {
    return m_installed.load();
}

void InputLocker::OnStateChanged(StateCallback cb) {
    m_callback = std::move(cb);
}

void InputLocker::ReleaseModifiersForLock() {
    // 发送 OS 级别 keyup 释放所有修饰键（使用 INJECTED_INPUT_EXTRA 让 hook 放行）
    // 防止锁定/解锁后修饰键残留在 OS "按下" 状态
    INPUT inputs[5] = {};
    for (int i = 0; i < 5; i++) {
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.dwFlags = KEYEVENTF_KEYUP;
        inputs[i].ki.dwExtraInfo = INJECTED_INPUT_EXTRA;
    }
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].ki.wVk = VK_MENU;
    inputs[2].ki.wVk = VK_SHIFT;
    inputs[3].ki.wVk = VK_LWIN;
    inputs[4].ki.wVk = VK_RWIN;
    SendInput(5, inputs, sizeof(INPUT));
}

void InputLocker::EnableViewerForward() {
    m_viewerForwardEnabled.store(true);
}

void InputLocker::DisableViewerForward() {
    m_viewerForwardEnabled.store(false);
    // 如果有修饰键处于 "hook 记录的按下" 状态，发送 OS 级别的 keyup，
    // 防止 hook 拦截了 keydown 但用户未走 keyup 路径导致系统粘键
    bool needCtrlUp  = m_ctrlDown.load();
    bool needAltUp   = m_altDown.load();
    bool needShiftUp = m_shiftDown.load();
    bool needLWinUp  = m_lwinDown.load();
    bool needRWinUp  = m_rwinDown.load();

    m_lwinDown.store(false);
    m_rwinDown.store(false);
    m_winUsedInCombo.store(false);
    // 重置修饰键状态，防止残留粘键
    m_ctrlDown.store(false);
    m_altDown.store(false);
    m_shiftDown.store(false);

    // 发送 OS 级别 keyup 释放残留修饰键（使用 INJECTED_INPUT_EXTRA 让 hook 放行）
    INPUT inputs[5] = {};
    int count = 0;
    auto addKeyUp = [&](WORD vk) {
        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = vk;
        inputs[count].ki.dwFlags = KEYEVENTF_KEYUP;
        inputs[count].ki.dwExtraInfo = INJECTED_INPUT_EXTRA;
        count++;
    };
    if (needCtrlUp)  addKeyUp(VK_CONTROL);
    if (needAltUp)   addKeyUp(VK_MENU);
    if (needShiftUp) addKeyUp(VK_SHIFT);
    if (needLWinUp)  addKeyUp(VK_LWIN);
    if (needRWinUp)  addKeyUp(VK_RWIN);

    if (count > 0) {
        SendInput(count, inputs, sizeof(INPUT));
    }
}

bool InputLocker::IsViewerForwardEnabled() const {
    return m_viewerForwardEnabled.load();
}

void InputLocker::OnViewerCombo(ViewerComboCallback cb) {
    m_viewerComboCallback = std::move(cb);
}

// ── 热键匹配 ──

bool InputLocker::CheckHotkeyMatch(DWORD vkCode, bool isKeyDown) {
    // 更新修饰键状态
    switch (vkCode) {
    case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL:
        m_ctrlDown.store(isKeyDown);
        break;
    case VK_LMENU: case VK_RMENU: case VK_MENU:
        m_altDown.store(isKeyDown);
        break;
    case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT:
        m_shiftDown.store(isKeyDown);
        break;
    case VK_LWIN: case VK_RWIN:
        m_winDown.store(isKeyDown);
        break;
    }

    // 仅在主键按下时检测匹配
    if (!isKeyDown) return false;
    if (vkCode != m_hotkey.vkCode) return false;

    // 检查修饰符
    bool needCtrl  = (m_hotkey.modifiers & HMOD_CTRL)  != 0;
    bool needAlt   = (m_hotkey.modifiers & HMOD_ALT)   != 0;
    bool needShift = (m_hotkey.modifiers & HMOD_SHIFT)  != 0;
    bool needWin   = (m_hotkey.modifiers & HMOD_WIN)   != 0;

    if (needCtrl  != m_ctrlDown.load())  return false;
    if (needAlt   != m_altDown.load())   return false;
    if (needShift != m_shiftDown.load()) return false;
    if (needWin   != m_winDown.load())   return false;

    return true;
}

bool InputLocker::ShouldForwardViewerCombo(DWORD vkCode, bool isKeyDown, bool isKeyUp, std::string& comboOut) {
    // ── Win 键延迟发送：keydown 时仅记录，keyup 时如果没有参与组合才发送 "win" ──
    if (vkCode == VK_LWIN) {
        if (isKeyDown) {
            m_lwinDown.store(true);
            m_winUsedInCombo.store(false);
            // 不立即发送 "win"，等待后续按键或 keyup 再决定
            return true;
        }
        if (isKeyUp) {
            m_lwinDown.store(false);
            if (!m_winUsedInCombo.load()) {
                comboOut = "win";  // 单独 Win 按下-释放 → 发送 "win"
            }
            return true;
        }
    }
    if (vkCode == VK_RWIN) {
        if (isKeyDown) {
            m_rwinDown.store(true);
            m_winUsedInCombo.store(false);
            return true;
        }
        if (isKeyUp) {
            m_rwinDown.store(false);
            if (!m_winUsedInCombo.load()) {
                comboOut = "win";
            }
            return true;
        }
    }

    const bool anyWinDown = m_lwinDown.load() || m_rwinDown.load();
    const bool altDown = m_altDown.load();
    const bool ctrlDown = m_ctrlDown.load();

    if (!isKeyDown && !isKeyUp) return false;

    if (isKeyDown) {
        if (anyWinDown) {
            m_winUsedInCombo.store(true);  // Win 参与组合，keyup 时不再发送单独 "win"
            switch (vkCode) {
            case 'D': comboOut = "win+d"; return true;
            case 'E': comboOut = "win+e"; return true;
            case 'R': comboOut = "win+r"; return true;
            case 'L': comboOut = "win+l"; return true;
            case VK_TAB: comboOut = "win+tab"; return true;
            case 'S': comboOut = "win+s"; return true;
            case 'I': comboOut = "win+i"; return true;
            case 'X': comboOut = "win+x"; return true;
            case 'V': comboOut = "win+v"; return true;
            case 'A': comboOut = "win+a"; return true;
            case 'G': comboOut = "win+g"; return true;
            case 'P': comboOut = "win+p"; return true;
            default:
                // Win+任意键：本机吞掉，避免系统快捷键触发
                return true;
            }
        }

        // ESC 键：viewer forward 模式下拦截，防止退出 Pointer Lock
        if (vkCode == VK_ESCAPE) {
            return true;  // 吞掉 ESC，不传递给浏览器
        }

        if (altDown && vkCode == VK_TAB) {
            comboOut = "alt+tab";
            return true;
        }
        if (altDown && vkCode == VK_F4) {
            comboOut = "alt+f4";
            return true;
        }
        if (altDown && vkCode == VK_ESCAPE) {
            comboOut = "alt+esc";
            return true;
        }
        if (ctrlDown && m_shiftDown.load() && vkCode == VK_ESCAPE) {
            comboOut = "ctrl+shift+esc";
            return true;
        }
        if (ctrlDown && vkCode == VK_ESCAPE) {
            comboOut = "ctrl+esc";
            return true;
        }

        // Shift+F1~F12：转发给 JS 处理（如 Shift+F9 切换远控）
        if (m_shiftDown.load() && !altDown && !ctrlDown && !anyWinDown
            && vkCode >= VK_F1 && vkCode <= VK_F12) {
            int fNum = vkCode - VK_F1 + 1;
            comboOut = "shift+f" + std::to_string(fNum);
            return true;
        }
    } else {
        // keyup: 如果仍处于组合状态，继续吞掉，避免本机补触发
        if (anyWinDown || altDown || ctrlDown) {
            return true;
        }
        // ESC keyup 也要吞掉（与 keydown 对应）
        if (vkCode == VK_ESCAPE) {
            return true;
        }
        // Shift+F-key keyup 也要吞掉（与 keydown 对应）
        if (m_shiftDown.load() && vkCode >= VK_F1 && vkCode <= VK_F12) {
            return true;
        }
    }

    return false;
}

// ── 低级钩子回调 ──

LRESULT CALLBACK InputLocker::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_instance) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        // 放行远程注入的事件（通过 dwExtraInfo 标记识别）
        if (kb->dwExtraInfo == INJECTED_INPUT_EXTRA) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool isKeyUp   = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

        // 检查是否匹配切换热键（始终切换锁定状态，无论 viewer forward 是否启用）
        if (s_instance->CheckHotkeyMatch(kb->vkCode, isKeyDown)) {
            s_instance->Toggle();
            return 1; // 吞掉热键本身
        }

        // Viewer 端系统键转发模式（本机吞掉，转发组合键给 JS）
        // 锁定时不转发：锁定状态下一切输入都应被拦截
        if (s_instance->m_viewerForwardEnabled.load() && !s_instance->m_locked.load()) {
            std::string combo;
            if (s_instance->ShouldForwardViewerCombo(kb->vkCode, isKeyDown, isKeyUp, combo)) {
                // combo 可能在 keydown（如 win+r）或 keyup（单独 win 延迟发送）时产生
                if (!combo.empty() && s_instance->m_viewerComboCallback) {
                    s_instance->m_viewerComboCallback(combo);
                }
                return 1;
            }
        }

        // 锁定时：吞掉所有键盘事件（包括修饰键、Win 键）
        // CheckHotkeyMatch 已在上方执行过修饰键状态跟踪，解锁热键仍可触发
        // Lock() 时已通过 ReleaseModifiersForLock() 释放 OS 级别修饰键
        if (s_instance->m_locked.load()) {
            return 1;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK InputLocker::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_instance) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        // 放行远程注入的事件（通过 dwExtraInfo 标记识别）
        if (ms->dwExtraInfo == INJECTED_INPUT_EXTRA) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        if (s_instance->m_locked.load()) {
            // 锁定时吞掉所有本地鼠标事件
            return 1;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace MDShare
