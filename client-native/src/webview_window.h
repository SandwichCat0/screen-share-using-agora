#pragma once
#include <string>
#include <functional>
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include "agora_manager.h"
#include "perf_migration.h"
#include "process_mgr.h"
#include "input_locker.h"
#include "input_injector.h"

namespace MDShare {

class WebViewWindow {
public:
    explicit WebViewWindow(HINSTANCE hInstance);
    ~WebViewWindow();

    void Run(int nCmdShow, bool stealth = false);

private:
    // ── Win32 窗口 ──
    HINSTANCE m_hInstance = nullptr;
    HWND      m_hwnd     = nullptr;
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    bool CreateMainWindow(int nCmdShow);

    // ── WebView2 ──
    Microsoft::WRL::ComPtr<ICoreWebView2>            m_webview;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller>  m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_env;
    bool m_webReady = false;

    void InitWebView2();
    void OnWebViewCreated(HRESULT hr, ICoreWebView2Controller* controller);
    void OnWebMessageReceived(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args);
    void OnWebResourceRequested(ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args);
    void OnNavigationCompleted(ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args);
    void ResizeWebView();

    // ── 消息处理 ──
    void DispatchMessage(const std::string& json);
    void SendToWeb(const std::string& type, const std::string& dataJson = "null");
    // 从任意线程安全发送（PostMessage 到 UI 线程）
    void PostToWeb(const std::string& type, const std::string& dataJson);
    // 直接发送已构建好的 JSON 字符串（跳过 nlohmann 序列化，用于大数据如预览帧）
    void SendRawJsonToWeb(const std::string& jsonMessage);

    // ── 全屏 ──
    bool        m_isFullscreen = false;
    bool        m_isHidden = false;
    bool        m_isStealth = false;
    void SetStealth(bool enable);
    WINDOWPLACEMENT m_prevPlacement{};
    LONG        m_prevStyle = 0;
    LONG        m_prevExStyle = 0;
    void EnterFullscreen();
    void ExitFullscreen();

    // ── Agora Native ──
    AgoraManager m_agora;

    // ── 键鼠锁定 ──
    InputLocker m_inputLocker;

    // ── 远程输入注入 ──
    InputInjector m_inputInjector;

    // ── 日志迁移 ──
    PerfMigration::MigrationManager m_migrationMgr;

    // ── Web 资源 ──
    static std::string GetMimeType(const std::string& path);
    static std::pair<const unsigned char*, size_t> FindEmbeddedResource(const std::string& path);
};

} // namespace MDShare
