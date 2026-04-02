/**
 * input_injector.cpp — 远程输入注入模块实现
 *
 * 通过 Win32 SendInput() API 将远端键鼠事件注入到本地系统。
 *
 * 参考实现：
 *   - Sunshine input.cpp:     SendInput + 桌面切换兜底 + 扫描码模式
 *   - RustDesk win_impl.rs:   dwExtraInfo 标记 + 虚拟桌面坐标
 *   - noVNC xtscancodes.js:   HTML event.code → AT Set 1 扫描码
 */
// 需要在 windows.h 之前定义，启用 OCR_NORMAL/OCR_IBEAM 等光标常量
#ifndef OEMRESOURCE
#define OEMRESOURCE
#endif
#include "input_injector.h"
#include <thread>
#include <map>
#include <vector>
#include <chrono>

namespace MDShare {

// ── 析构 ──

InputInjector::~InputInjector() {
    StopCursorHideEnforcement();
    StopCursorPolling();
}

// ── 启用/禁用 ──

void InputInjector::Enable() {
    m_enabled.store(true);
}

void InputInjector::Disable() {
    m_enabled.store(false);
}

bool InputInjector::IsEnabled() const {
    return m_enabled.load();
}

// ── 安全发送 (参考 Sunshine send_input()) ──

void InputInjector::SendInputSafe(INPUT& input) {
    // 标记合成输入，让 InputLocker 钩子可识别并放行
    if (input.type == INPUT_KEYBOARD) {
        input.ki.dwExtraInfo = INJECTED_INPUT_EXTRA;
    } else if (input.type == INPUT_MOUSE) {
        input.mi.dwExtraInfo = INJECTED_INPUT_EXTRA;
    }

    if (::SendInput(1, &input, sizeof(INPUT)) == 0) {
        // Sunshine 模式：失败时切换到活动桌面后重试
        HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
        if (hDesk) {
            SetThreadDesktop(hDesk);
            ::SendInput(1, &input, sizeof(INPUT));
            CloseDesktop(hDesk);
        }
    }
}

void InputInjector::SendInputsSafe(INPUT* inputs, UINT count) {
    // 标记所有输入
    for (UINT i = 0; i < count; ++i) {
        if (inputs[i].type == INPUT_KEYBOARD) {
            inputs[i].ki.dwExtraInfo = INJECTED_INPUT_EXTRA;
        } else if (inputs[i].type == INPUT_MOUSE) {
            inputs[i].mi.dwExtraInfo = INJECTED_INPUT_EXTRA;
        }
    }

    if (::SendInput(count, inputs, sizeof(INPUT)) == 0) {
        HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
        if (hDesk) {
            SetThreadDesktop(hDesk);
            ::SendInput(count, inputs, sizeof(INPUT));
            CloseDesktop(hDesk);
        }
    }
}

// ── 鼠标注入 ──

void InputInjector::InjectMouseMoveAbsolute(double normX, double normY) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    // MOUSEEVENTF_VIRTUALDESK: 映射到整个虚拟桌面（多显示器）
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    // 归一化坐标 0.0~1.0 → 0~65535
    input.mi.dx = static_cast<LONG>(normX * 65535.0);
    input.mi.dy = static_cast<LONG>(normY * 65535.0);
    // 限制范围
    if (input.mi.dx < 0) input.mi.dx = 0;
    if (input.mi.dx > 65535) input.mi.dx = 65535;
    if (input.mi.dy < 0) input.mi.dy = 0;
    if (input.mi.dy > 65535) input.mi.dy = 65535;
    SendInputSafe(input);
}

void InputInjector::InjectMouseMoveRelative(int dx, int dy) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    SendInputSafe(input);
}

void InputInjector::InjectMouseButton(int button, bool down) {
    INPUT input = {};
    input.type = INPUT_MOUSE;

    switch (button) {
    case 0: // 左键
        input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case 1: // 中键
        input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case 2: // 右键
        input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case 3: // X1
        input.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        input.mi.mouseData = XBUTTON1;
        break;
    case 4: // X2
        input.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        input.mi.mouseData = XBUTTON2;
        break;
    default:
        return;
    }

    SendInputSafe(input);
}

void InputInjector::InjectMouseWheel(int deltaX, int deltaY) {
    // 垂直滚轮
    if (deltaY != 0) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(deltaY);
        SendInputSafe(input);
    }
    // 水平滚轮
    if (deltaX != 0) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = static_cast<DWORD>(deltaX);
        SendInputSafe(input);
    }
}

// ── 键盘注入 ──

void InputInjector::InjectKeyScancode(uint16_t scancode, bool extended, bool up) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    // 使用扫描码模式（最可靠，游戏和全屏应用都响应）
    input.ki.wScan = scancode;
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (up) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInputSafe(input);
}

void InputInjector::InjectUnicodeText(const std::wstring& text) {
    // 参考 Sunshine unicode() — 先全部按下，再全部释放
    std::vector<INPUT> inputs;
    inputs.reserve(text.size() * 2);

    for (wchar_t ch : text) {
        INPUT down = {};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = ch;
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(down);
    }
    for (wchar_t ch : text) {
        INPUT up = {};
        up.type = INPUT_KEYBOARD;
        up.ki.wScan = ch;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(up);
    }

    if (!inputs.empty()) {
        SendInputsSafe(inputs.data(), static_cast<UINT>(inputs.size()));
    }
}

void InputInjector::InjectSpecialCombo(const std::string& combo) {
    if (combo == "ctrl+alt+del") {
        // Ctrl+Alt+Del 无法通过 SendInput 注入（安全桌面限制）
        // 使用 SAS (Secure Attention Sequence) DLL
        HMODULE sas = LoadLibraryW(L"sas.dll");
        if (sas) {
            typedef void (WINAPI *SendSAS_t)(BOOL);
            auto pSendSAS = (SendSAS_t)GetProcAddress(sas, "SendSAS");
            if (pSendSAS) pSendSAS(FALSE);
            FreeLibrary(sas);
        }
    }
    else if (combo == "alt+tab") {
        INPUT inputs[4] = {};
        // Alt down
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x38; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;
        // Tab down
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x0F; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        // Tab up
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x0F; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        // Alt up
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x38; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "alt+f4") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x38; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x3E; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x3E; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x38; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "alt+esc") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x38; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x01; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x01; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x38; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "win") {
        INPUT inputs[2] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x5B; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x5B; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 2);
    }
    else if (combo == "win+d") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x5B; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x20; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x20; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x5B; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "win+e") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x5B; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x12; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x12; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x5B; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "win+r") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x5B; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x13; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x13; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x5B; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "win+l") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x5B; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x26; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x26; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x5B; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "win+tab") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x5B; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x0F; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x0F; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x5B; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "win+s") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x5B; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x1F; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x1F; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x5B; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "win+i") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x5B; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x17; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x17; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x5B; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "win+x") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x5B; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x2D; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x2D; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x5B; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "win+v") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x5B; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x2F; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x2F; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x5B; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "win+a") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x5B; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x1E; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x1E; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x5B; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "win+g") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x5B; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x22; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x22; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x5B; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "win+p") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x5B; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x19; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x19; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x5B; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "ctrl+esc") {
        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x1D; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x01; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x01; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x1D; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 4);
    }
    else if (combo == "ctrl+shift+esc") {
        INPUT inputs[6] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = 0x1D; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = 0x2A; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = 0x01; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wScan = 0x01; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[4].type = INPUT_KEYBOARD;
        inputs[4].ki.wScan = 0x2A; inputs[4].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        inputs[5].type = INPUT_KEYBOARD;
        inputs[5].ki.wScan = 0x1D; inputs[5].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        SendInputsSafe(inputs, 6);
    }
}

// ── 释放所有修饰键 ──

void InputInjector::ReleaseAllModifiers() {
    INPUT inputs[8] = {};
    // LeftCtrl (sc=0x1d)
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wScan = 0x1D; inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    // RightCtrl (sc=0x1d, extended)
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wScan = 0x1D; inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    // LeftShift (sc=0x2a)
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wScan = 0x2A; inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    // RightShift (sc=0x36)
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wScan = 0x36; inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    // LeftAlt (sc=0x38)
    inputs[4].type = INPUT_KEYBOARD;
    inputs[4].ki.wScan = 0x38; inputs[4].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    // RightAlt (sc=0x38, extended)
    inputs[5].type = INPUT_KEYBOARD;
    inputs[5].ki.wScan = 0x38; inputs[5].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    // LeftWin (sc=0x5b, extended)
    inputs[6].type = INPUT_KEYBOARD;
    inputs[6].ki.wScan = 0x5B; inputs[6].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    // RightWin (sc=0x5c, extended)
    inputs[7].type = INPUT_KEYBOARD;
    inputs[7].ki.wScan = 0x5C; inputs[7].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;

    SendInputsSafe(inputs, 8);
}

// ── 屏幕信息 ──

nlohmann::json InputInjector::GetScreenInfo() {
    nlohmann::json info;

    // 主显示器
    info["primaryWidth"] = GetSystemMetrics(SM_CXSCREEN);
    info["primaryHeight"] = GetSystemMetrics(SM_CYSCREEN);

    // 虚拟桌面（所有显示器的包围盒）
    info["virtualLeft"] = GetSystemMetrics(SM_XVIRTUALSCREEN);
    info["virtualTop"] = GetSystemMetrics(SM_YVIRTUALSCREEN);
    info["virtualWidth"] = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    info["virtualHeight"] = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    info["monitorCount"] = GetSystemMetrics(SM_CMONITORS);

    // DPI（需要以 DPI 感知模式运行才准确）
    HDC hdc = GetDC(nullptr);
    if (hdc) {
        info["dpiX"] = GetDeviceCaps(hdc, LOGPIXELSX);
        info["dpiY"] = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(nullptr, hdc);
    }

    return info;
}

// ── 输入事件 JSON 分发 ──

void InputInjector::HandleInputEvent(const nlohmann::json& msg) {
    if (!m_enabled.load()) return;

    std::string t = msg.value("t", "");

    if (t == "ma") {
        // 鼠标绝对移动
        double x = msg.value("x", 0.0);
        double y = msg.value("y", 0.0);
        InjectMouseMoveAbsolute(x, y);
    }
    else if (t == "mm") {
        // 鼠标相对移动 (Pointer Lock 模式)
        int dx = msg.value("dx", 0);
        int dy = msg.value("dy", 0);
        InjectMouseMoveRelative(dx, dy);
    }
    else if (t == "md") {
        // 鼠标按下
        int b = msg.value("b", 0);
        InjectMouseButton(b, true);
    }
    else if (t == "mu") {
        // 鼠标释放
        int b = msg.value("b", 0);
        InjectMouseButton(b, false);
    }
    else if (t == "mw") {
        // 鼠标滚轮
        int dx = msg.value("dx", 0);
        int dy = msg.value("dy", 0);
        InjectMouseWheel(dx, dy);
    }
    else if (t == "kd") {
        // 键盘按下
        uint16_t sc = msg.value("sc", 0);
        bool ext = msg.value("ext", false);
        InjectKeyScancode(sc, ext, false);
    }
    else if (t == "ku") {
        // 键盘释放
        uint16_t sc = msg.value("sc", 0);
        bool ext = msg.value("ext", false);
        InjectKeyScancode(sc, ext, true);
    }
    else if (t == "text") {
        // Unicode 文本
        std::string utf8 = msg.value("text", "");
        if (!utf8.empty()) {
            int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
            if (len > 0) {
                std::wstring wide(len - 1, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), len);
                InjectUnicodeText(wide);
            }
        }
    }
    else if (t == "combo") {
        // 特殊组合键
        std::string combo = msg.value("combo", "");
        if (!combo.empty()) {
            InjectSpecialCombo(combo);
        }
    }
}

// ── 光标形状同步 ──

// 系统光标句柄 → CSS cursor 名称映射（进程生命周期内缓存）
static std::map<HCURSOR, std::string> s_systemCursors;
static bool s_cursorsInitialized = false;

static void _InitSystemCursors() {
    if (s_cursorsInitialized) return;
    s_systemCursors[LoadCursorW(nullptr, IDC_ARROW)]     = "default";
    s_systemCursors[LoadCursorW(nullptr, IDC_IBEAM)]     = "text";
    s_systemCursors[LoadCursorW(nullptr, IDC_WAIT)]      = "wait";
    s_systemCursors[LoadCursorW(nullptr, IDC_CROSS)]     = "crosshair";
    s_systemCursors[LoadCursorW(nullptr, IDC_UPARROW)]   = "n-resize";
    s_systemCursors[LoadCursorW(nullptr, IDC_SIZENWSE)]  = "nwse-resize";
    s_systemCursors[LoadCursorW(nullptr, IDC_SIZENESW)]  = "nesw-resize";
    s_systemCursors[LoadCursorW(nullptr, IDC_SIZEWE)]    = "ew-resize";
    s_systemCursors[LoadCursorW(nullptr, IDC_SIZENS)]    = "ns-resize";
    s_systemCursors[LoadCursorW(nullptr, IDC_SIZEALL)]   = "move";
    s_systemCursors[LoadCursorW(nullptr, IDC_NO)]        = "not-allowed";
    s_systemCursors[LoadCursorW(nullptr, IDC_HAND)]      = "pointer";
    s_systemCursors[LoadCursorW(nullptr, IDC_APPSTARTING)] = "progress";
    s_systemCursors[LoadCursorW(nullptr, IDC_HELP)]      = "help";
    s_cursorsInitialized = true;
}

std::string InputInjector::_GetCurrentCursorType() {
    _InitSystemCursors();
    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return "default";
    if (!(ci.flags & CURSOR_SHOWING)) return "none";

    auto it = s_systemCursors.find(ci.hCursor);
    return (it != s_systemCursors.end()) ? it->second : "default";
}

void InputInjector::StartCursorPolling(CursorCallback callback) {
    StopCursorPolling();
    m_cursorCallback = callback;
    m_lastCursorType.clear();
    m_cursorPolling.store(true);
    m_cursorThread = std::thread(&InputInjector::_CursorPollLoop, this);
}

void InputInjector::StopCursorPolling() {
    m_cursorPolling.store(false);
    if (m_cursorThread.joinable()) m_cursorThread.join();
    m_cursorCallback = nullptr;
    m_lastCursorType.clear();
}

void InputInjector::_CursorPollLoop() {
    while (m_cursorPolling.load()) {
        std::string cur = _GetCurrentCursorType();
        if (cur != m_lastCursorType) {
            m_lastCursorType = cur;
            if (m_cursorCallback) {
                try { m_cursorCallback(cur); } catch (...) {}
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ── 被控端光标隐藏控制 ──

HCURSOR InputInjector::_CreateBlankCursor() {
    int cx = GetSystemMetrics(SM_CXCURSOR);
    int cy = GetSystemMetrics(SM_CYCURSOR);
    // AND mask: all 0xFF = transparent; XOR mask: all 0x00 = no inversion → invisible
    int maskSize = ((cx + 31) / 32) * 4 * cy;
    std::vector<BYTE> andMask(maskSize, 0xFF);
    std::vector<BYTE> xorMask(maskSize, 0x00);
    return CreateCursor(nullptr, 0, 0, cx, cy, andMask.data(), xorMask.data());
}

void InputInjector::_ApplyBlankCursors() {
    if (!m_blankCursor) m_blankCursor = _CreateBlankCursor();
    if (!m_blankCursor) return;

    // 替换所有标准系统光标类型为透明光标
    static const DWORD cursorIds[] = {
        OCR_NORMAL, OCR_IBEAM, OCR_WAIT, OCR_CROSS, OCR_UP,
        OCR_SIZENWSE, OCR_SIZENESW, OCR_SIZEWE, OCR_SIZENS, OCR_SIZEALL,
        OCR_NO, OCR_HAND, OCR_APPSTARTING
    };
    for (auto id : cursorIds) {
        HCURSOR copy = CopyCursor(m_blankCursor);
        if (copy) SetSystemCursor(copy, id);
    }
}

void InputInjector::_RestoreCursors() {
    // 从注册表恢复所有系统光标为默认
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
}

void InputInjector::SetCursorHidden(bool hidden) {
    bool was = m_cursorShouldBeHidden.exchange(hidden);
    if (was != hidden) {
        if (hidden) {
            _ApplyBlankCursors();
        } else {
            _RestoreCursors();
        }
    }
}

void InputInjector::StartCursorHideEnforcement() {
    StopCursorHideEnforcement();
    m_cursorShouldBeHidden.store(true); // 默认隐藏
    _ApplyBlankCursors();
    m_cursorHideActive.store(true);
    m_cursorHideThread = std::thread(&InputInjector::_CursorHideLoop, this);
}

void InputInjector::StopCursorHideEnforcement() {
    m_cursorHideActive.store(false);
    if (m_cursorHideThread.joinable()) m_cursorHideThread.join();
    m_cursorShouldBeHidden.store(false);
    _RestoreCursors();
    if (m_blankCursor) {
        DestroyCursor(m_blankCursor);
        m_blankCursor = nullptr;
    }
}

void InputInjector::_CursorHideLoop() {
    while (m_cursorHideActive.load()) {
        if (m_cursorShouldBeHidden.load()) {
            // 某些应用可能重置光标，定期重新应用透明光标
            _ApplyBlankCursors();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

} // namespace MDShare
