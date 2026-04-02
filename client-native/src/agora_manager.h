#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <windows.h>

#include <IAgoraRtcEngine.h>

namespace MDShare {

/**
 * AgoraManager — Agora C++ RTC 引擎封装
 *
 * 负责：引擎初始化、频道管理、屏幕捕获、事件回调转 JSON 发回 WebView2
 */
class AgoraManager : public agora::rtc::IRtcEngineEventHandler {
public:
    AgoraManager();
    ~AgoraManager();

    // 初始化引擎（appId 从 JS 传入）
    bool Init(const std::string& appId);
    // 释放引擎
    void Release();

    // 频道
    bool JoinChannel(const std::string& token,
                     const std::string& channelId,
                     agora::rtc::uid_t uid);
    void LeaveChannel();

    // 屏幕/窗口捕获
    bool StartCaptureByDisplay(int displayId,
                               int frameRate, int width, int height, int bitrate,
                               bool shareAudio = false);
    bool StartCaptureByWindow(int64_t windowId,
                              int frameRate, int width, int height, int bitrate,
                              bool shareAudio = false);
    void StopCapture();

    // 动态更新编码参数（已开始捕获后调用）
    void UpdateCaptureParams(int frameRate, int bitrate);

    // 枚举屏幕/窗口源，返回 JSON 数组字符串
    std::string GetSourcesJson();

    // C++ → JS 事件回调（由 WebViewWindow 绑定）
    // 参数为完整 JSON 字符串，格式: {"type":"xxx","data":{...}}
    std::function<void(std::string)> onEvent;

    // C++ → JS 预览帧回调（base64 BMP 数据，由 WebViewWindow 绑定）
    std::function<void(std::string)> onPreviewFrame;

    // 预览截屏
    void StartPreviewLoop(const std::string& sourceType, int64_t sourceId);
    void StopPreviewLoop();

    bool IsInitialized() const { return m_initialized; }
    bool IsJoined()      const { return m_joined; }

private:
    // ── IRtcEngineEventHandler ──
    void onJoinChannelSuccess(const char* channel,
                              agora::rtc::uid_t uid, int elapsed) override;
    void onLeaveChannel(const agora::rtc::RtcStats& stats) override;
    void onError(int err, const char* msg) override;
    void onNetworkQuality(agora::rtc::uid_t uid,
                          int txQuality, int rxQuality) override;
    void onLocalVideoStats(agora::rtc::VIDEO_SOURCE_TYPE source,
                           const agora::rtc::LocalVideoStats& stats) override;

    // 向 WebView2 发送事件（线程安全，可从回调线程调用）
    void EmitEvent(const std::string& type, const std::string& dataJson);

    // 构造 ScreenCaptureParameters
    agora::rtc::ScreenCaptureParameters MakeParams(int frameRate, int width,
                                                    int height, int bitrate);
    // 应用游戏优化参数（引擎初始化后调用一次）
    void ApplyGamingParams();
    // 设置编码器配置（每次 startCapture 前调用，携带实际分辨率）
    void ApplyEncoderConfig(int width, int height, int frameRate, int bitrate);

    agora::rtc::IRtcEngine* m_engine = nullptr;
    std::atomic<bool> m_initialized{ false };
    std::atomic<bool> m_joined{ false };
    std::atomic<bool> m_capturing{ false };
    std::atomic<bool> m_shareAudio{ false };
    std::atomic<bool> m_hwRetried{ false };  // 防止软编回退重试死循环

    // 预览截屏线程
    std::thread m_previewThread;
    std::atomic<bool> m_previewRunning{ false };
    std::string m_previewSourceType;
    int64_t     m_previewSourceId = 0;
    void PreviewThreadProc();

    // 统计定时器
    HWND    m_timerHwnd  = nullptr;
    UINT_PTR m_timerId   = 0;
    void StartStatsTimer();
    void StopStatsTimer();
    static VOID CALLBACK StatsTimerProc(HWND, UINT, UINT_PTR, DWORD);
    static AgoraManager* s_instance; // 用于 StatsTimerProc 回调
};

} // namespace MDShare
