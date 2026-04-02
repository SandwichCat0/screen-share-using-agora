/**
 * agora_manager.cpp — Agora C++ RTC 引擎封装实现
 *
 * 参考：APIExample/APIExample/Advanced/ScreenShare/AgoraScreenCapture.cpp
 */
#include "agora_manager.h"
#include "json_helpers.h"

#include <nlohmann/json.hpp>
#include <vector>
#include <chrono>

// GDI+ for JPEG encoding — must include objidl.h before gdiplus.h
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

using json = nlohmann::json;

namespace MDShare {

AgoraManager* AgoraManager::s_instance = nullptr;

AgoraManager::AgoraManager()
{
    s_instance = this;
}

AgoraManager::~AgoraManager()
{
    Release();
    s_instance = nullptr;
}

// ── Init ─────────────────────────────────────────────

bool AgoraManager::Init(const std::string& appId)
{
    if (m_initialized) return true;

    m_engine = createAgoraRtcEngine();
    if (!m_engine) {
        EmitEvent("agora-error", R"({"code":-1,"msg":"createAgoraRtcEngine failed"})");
        return false;
    }

    agora::rtc::RtcEngineContext ctx;
    ctx.appId        = appId.c_str();
    ctx.eventHandler = this;
    ctx.channelProfile = agora::CHANNEL_PROFILE_LIVE_BROADCASTING;

    int ret = m_engine->initialize(ctx);
    if (ret != 0) {
        json err;
        err["code"] = ret;
        err["msg"]  = "initialize failed";
        EmitEvent("agora-error", err.dump());
        m_engine->release(nullptr);
        m_engine = nullptr;
        return false;
    }

    m_engine->setClientRole(agora::rtc::CLIENT_ROLE_BROADCASTER);
    ApplyGamingParams();

    m_initialized = true;
    EmitEvent("agora-ready", "{}");
    return true;
}

// ── Release ──────────────────────────────────────────

void AgoraManager::Release()
{
    StopPreviewLoop();
    StopStatsTimer();
    if (!m_engine) return;

    if (m_capturing) {
        m_engine->stopScreenCapture();
        m_capturing = false;
    }
    if (m_joined) {
        m_engine->leaveChannel();
        m_joined = false;
    }
    m_engine->stopPreview();
    m_engine->disableVideo();
    m_engine->release(nullptr);
    m_engine      = nullptr;
    m_initialized = false;
}

// ── JoinChannel ──────────────────────────────────────

bool AgoraManager::JoinChannel(const std::string& token,
                                const std::string& channelId,
                                agora::rtc::uid_t uid)
{
    if (!m_initialized || !m_engine) return false;

    agora::rtc::ChannelMediaOptions opt;
    opt.channelProfile     = agora::CHANNEL_PROFILE_LIVE_BROADCASTING;
    opt.clientRoleType     = agora::rtc::CLIENT_ROLE_BROADCASTER;
    opt.autoSubscribeAudio = true;
    opt.autoSubscribeVideo = true;
    opt.publishScreenTrack = true;
    opt.publishCameraTrack = false;
    opt.publishMicrophoneTrack = false;

    const char* tokenPtr = token.empty() ? nullptr : token.c_str();
    int ret = m_engine->joinChannel(tokenPtr, channelId.c_str(), uid, opt);
    return ret == 0;
}

// ── LeaveChannel ─────────────────────────────────────

void AgoraManager::LeaveChannel()
{
    if (m_engine && m_joined) {
        m_engine->leaveChannel();
    }
}

// ── MakeParams ───────────────────────────────────────

agora::rtc::ScreenCaptureParameters AgoraManager::MakeParams(
    int frameRate, int width, int height, int bitrate)
{
    agora::rtc::ScreenCaptureParameters p;
    p.dimensions.width  = width;
    p.dimensions.height = height;
    p.frameRate         = frameRate;
    p.bitrate           = bitrate;        // kbps, 0 = SDK auto
    p.captureMouseCursor = true;
    p.windowFocus        = false;
    p.excludeWindowList  = nullptr;
    p.excludeWindowCount = 0;
    return p;
}

// ── StartCaptureByDisplay ────────────────────────────

bool AgoraManager::StartCaptureByDisplay(int displayId,
    int frameRate, int width, int height, int bitrate, bool shareAudio)
{
    if (!m_initialized || !m_engine) return false;

    if (m_capturing) {
        m_engine->stopScreenCapture();
        m_capturing = false;
    }

    m_shareAudio = shareAudio;
    if (shareAudio) {
        // 采集默认播放设备的音频（loopback）
        m_engine->enableLoopbackRecording(true);
        // 静音实际麦克风录制，防止 CABLE Output 的声音泄漏到观看端
        m_engine->adjustRecordingSignalVolume(0);
    }

    // 设置编码器配置（分辨率 + 硬编 + H.265），必须在 startCapture 之前
    ApplyEncoderConfig(width, height, frameRate, bitrate);
    m_hwRetried = false;

    agora::rtc::Rectangle region{}; // 全屏
    auto params = MakeParams(frameRate, width, height, bitrate);
    int ret = m_engine->startScreenCaptureByDisplayId(
        (uint32_t)displayId, region, params);

    if (ret != 0) {
        json err;
        err["code"] = ret;
        err["msg"]  = "startScreenCaptureByDisplayId failed";
        EmitEvent("agora-error", err.dump());
        if (shareAudio) {
            m_engine->enableLoopbackRecording(false);
            m_engine->adjustRecordingSignalVolume(100);
        }
        m_shareAudio = false;
        return false;
    }

    m_capturing = true;
    m_engine->startPreview(agora::rtc::VIDEO_SOURCE_SCREEN);

    if (m_joined) {
        agora::rtc::ChannelMediaOptions opt;
        opt.publishScreenTrack = true;
        opt.publishCameraTrack = false;
        if (shareAudio) opt.publishMicrophoneTrack = true;
        m_engine->updateChannelMediaOptions(opt);
    }
    return true;
}

// ── StartCaptureByWindow ─────────────────────────────

bool AgoraManager::StartCaptureByWindow(int64_t windowId,
    int frameRate, int width, int height, int bitrate, bool shareAudio)
{
    if (!m_initialized || !m_engine) return false;

    if (m_capturing) {
        m_engine->stopScreenCapture();
        m_capturing = false;
    }

    m_shareAudio = shareAudio;
    if (shareAudio) {
        m_engine->enableLoopbackRecording(true);
        m_engine->adjustRecordingSignalVolume(0);
    }

    // 设置编码器配置（分辨率 + 硬编 + H.265），必须在 startCapture 之前
    ApplyEncoderConfig(width, height, frameRate, bitrate);
    m_hwRetried = false;

    agora::rtc::Rectangle region{};
    auto params = MakeParams(frameRate, width, height, bitrate);
    params.windowFocus = true; // 捕获时自动聚焦窗口
    int ret = m_engine->startScreenCaptureByWindowId(
        (int64_t)(intptr_t)windowId, region, params);

    if (ret != 0) {
        json err;
        err["code"] = ret;
        err["msg"]  = "startScreenCaptureByWindowId failed";
        EmitEvent("agora-error", err.dump());
        if (shareAudio) {
            m_engine->enableLoopbackRecording(false);
            m_engine->adjustRecordingSignalVolume(100);
        }
        m_shareAudio = false;
        return false;
    }

    m_capturing = true;
    m_engine->startPreview(agora::rtc::VIDEO_SOURCE_SCREEN);

    if (m_joined) {
        agora::rtc::ChannelMediaOptions opt;
        opt.publishScreenTrack = true;
        opt.publishCameraTrack = false;
        if (shareAudio) opt.publishMicrophoneTrack = true;
        m_engine->updateChannelMediaOptions(opt);
    }
    return true;
}

// ── StopCapture ──────────────────────────────────────

void AgoraManager::StopCapture()
{
    if (m_engine && m_capturing) {
        m_engine->stopScreenCapture();
        m_engine->stopPreview();
        m_capturing = false;
    }
    if (m_engine && m_shareAudio) {
        m_engine->enableLoopbackRecording(false);
        m_engine->adjustRecordingSignalVolume(100);
        m_shareAudio = false;
    }
}

// ── UpdateCaptureParams ──────────────────────────────

void AgoraManager::UpdateCaptureParams(int frameRate, int bitrate)
{
    if (!m_engine || !m_capturing) return;
    agora::rtc::ScreenCaptureParameters p;
    p.frameRate = frameRate;
    p.bitrate   = bitrate;
    m_engine->updateScreenCaptureParameters(p);
}

// ── GetSourcesJson ───────────────────────────────────

std::string AgoraManager::GetSourcesJson()
{
    if (!m_initialized || !m_engine) return "[]";

    SIZE thumbSz = { 160, 90 };
    SIZE iconSz  = { 32, 32 };
    agora::rtc::IScreenCaptureSourceList* list =
        m_engine->getScreenCaptureSources(thumbSz, iconSz, true);

    json arr = json::array();
    if (!list) return arr.dump();

    for (int i = 0; i < list->getCount(); i++) {
        agora::rtc::ScreenCaptureSourceInfo info = list->getSourceInfo(i);

        json item;
        item["id"]       = (int64_t)(intptr_t)info.sourceId;
        item["name"]     = info.sourceTitle ? info.sourceTitle : "";
        item["type"]     = (info.type == agora::rtc::ScreenCaptureSourceType_Screen)
                           ? "display" : "window";
        item["minimized"] = info.minimizeWindow;
        arr.push_back(item);
    }

    list->release();
    return arr.dump();
}

// ── ApplyGamingParams ────────────────────────────────

void AgoraManager::ApplyGamingParams()
{
    if (!m_engine) return;

    // 游戏场景（最重要，优先帧率）
    m_engine->setScreenCaptureScenario(
        agora::rtc::SCREEN_SCENARIO_GAMING);

    // 注意：不再禁用 DirectX 捕获。禁用 DirectX 会断开 GPU 零拷贝链路，
    // 导致帧数据必须从系统内存上传到 GPU 再编码，大幅增加延迟，
    // SDK 可能因此判定硬编效率不够而回退到软编。
    // 若遇到特定游戏黑屏/卡顿，应针对该游戏使用窗口捕获而非全屏捕获。

    // 防止 H265 回退时码率抖动
    m_engine->setParameters(
        R"({"rtc.video.saveBitrateParams":{"H265Fallback_save":false}})");

    // 不跳过系统/全屏窗口
    m_engine->setParameters(
        R"({"che.video.do_not_skip_system_window":true})");

    // IPC 捕获模式（更低延迟）
    m_engine->setParameters(
        R"({"che.video.using_ipc_capturer":true})");

    // 硬件编码器
    m_engine->setParameters(
        R"({"engine.video.enable_hw_encoder":true})");

    // 允许 DirectX 捕获，保持 GPU 零拷贝路径畅通（NVENC 硬编的前提）
    m_engine->setParameters(
        R"({"rtc.win_allow_directx":true})");
}

// ── ApplyEncoderConfig ───────────────────────────────

void AgoraManager::ApplyEncoderConfig(int width, int height,
                                       int frameRate, int bitrate)
{
    if (!m_engine) return;

    // 关键修复：必须把实际分辨率传给 VideoEncoderConfiguration，
    // 否则 SDK 会用默认值（640×480）钳制编码分辨率，
    // 导致即使 ScreenCaptureParameters 指定了 2560×1440，
    // 实际编码输出仍被限制到 1920×1080 甚至更低。
    agora::rtc::VideoEncoderConfiguration encCfg;
    encCfg.dimensions.width  = width;
    encCfg.dimensions.height = height;
    encCfg.frameRate         = frameRate;
    encCfg.bitrate           = bitrate;  // kbps, 0 = SDK auto
    encCfg.codecType         = agora::rtc::VIDEO_CODEC_H265;
    encCfg.advanceOptions.encodingPreference    = agora::rtc::PREFER_HARDWARE;
    encCfg.advanceOptions.compressionPreference = agora::rtc::PREFER_LOW_LATENCY;
    m_engine->setVideoEncoderConfiguration(encCfg);
}

// ── EmitEvent ────────────────────────────────────────

void AgoraManager::EmitEvent(const std::string& type, const std::string& dataJson)
{
    if (!onEvent) return;
    // 构造: {"type":"xxx","data":{...}}
    std::string msg = R"({"type":")" + type + R"(","data":)" + dataJson + "}";
    onEvent(msg);
}

// ── IRtcEngineEventHandler callbacks ─────────────────

void AgoraManager::onJoinChannelSuccess(const char* channel,
    agora::rtc::uid_t uid, int /*elapsed*/)
{
    m_joined = true;

    // 修复竞态：若 startCapture 在 join 完成前已被调用（m_capturing==true），
    // 此时 publishScreenTrack 尚未生效，需在此补发 updateChannelMediaOptions。
    if (m_capturing && m_engine) {
        agora::rtc::ChannelMediaOptions opt;
        opt.publishScreenTrack = true;
        opt.publishCameraTrack = false;
        if (m_shareAudio) opt.publishMicrophoneTrack = true;
        m_engine->updateChannelMediaOptions(opt);
    }

    StartStatsTimer();
    json d;
    d["channel"] = channel ? channel : "";
    d["uid"]     = (uint32_t)uid;
    EmitEvent("agora-joined", d.dump());
}

void AgoraManager::onLeaveChannel(const agora::rtc::RtcStats& /*stats*/)
{
    m_joined = false;
    StopStatsTimer();
    EmitEvent("agora-left", "{}");
}

void AgoraManager::onError(int err, const char* msg)
{
    json d;
    d["code"] = err;
    d["msg"]  = msg ? msg : "";
    EmitEvent("agora-error", d.dump());
}

void AgoraManager::onNetworkQuality(agora::rtc::uid_t /*uid*/,
    int txQuality, int /*rxQuality*/)
{
    json d;
    d["txQuality"] = txQuality;
    EmitEvent("agora-network", d.dump());
}

void AgoraManager::onLocalVideoStats(agora::rtc::VIDEO_SOURCE_TYPE /*source*/,
    const agora::rtc::LocalVideoStats& stats)
{
    bool isHw = (stats.hwEncoderAccelerating == 1);
    bool isH265 = (stats.codecType == agora::rtc::VIDEO_CODEC_H265);

    // 软编回退自动重试：如果 SDK 回退到软编（SW），尝试一次显式重设硬编配置。
    // H.265 不可用时（远端不支持），至少确保 H.264 也走硬件编码（NVENC）。
    if (!isHw && m_capturing && !m_hwRetried && m_engine) {
        m_hwRetried = true;
        m_engine->setParameters(R"({"engine.video.enable_hw_encoder":true})");

        agora::rtc::VideoEncoderConfiguration encCfg;
        encCfg.dimensions.width  = stats.encodedFrameWidth  > 0 ? stats.encodedFrameWidth  : 1920;
        encCfg.dimensions.height = stats.encodedFrameHeight > 0 ? stats.encodedFrameHeight : 1080;
        // 如果 H.265 已被拒绝，退而求其次用 H.264 硬编
        encCfg.codecType = isH265 ? agora::rtc::VIDEO_CODEC_H265
                                  : agora::rtc::VIDEO_CODEC_H264;
        encCfg.advanceOptions.encodingPreference    = agora::rtc::PREFER_HARDWARE;
        encCfg.advanceOptions.compressionPreference = agora::rtc::PREFER_LOW_LATENCY;
        m_engine->setVideoEncoderConfiguration(encCfg);
    }

    json d;
    d["frameRate"]  = stats.sentFrameRate;
    d["bitrate"]    = stats.sentBitrate;
    d["width"]      = stats.encodedFrameWidth;
    d["height"]     = stats.encodedFrameHeight;
    d["hwEncoder"]  = isHw;
    // codecType: 2 = H.264, 3 = H.265
    d["codec"]      = isH265 ? "H.265" : "H.264";
    EmitEvent("agora-stats", d.dump());
}

// ── 统计定时器（每 3 秒）────────────────────────────

void AgoraManager::StartStatsTimer()
{
    // 使用消息窗口定时器，在调用线程上触发
    // 简单实现：直接使用 SetTimer 在主线程（依赖 WM_TIMER 在消息循环）
    // 注意：agora 的 onLocalVideoStats 已经周期性回调，不需要额外定时器
    // 此处留空，统计已通过 SDK 回调推送
}

void AgoraManager::StopStatsTimer()
{
    // 留空
}

VOID CALLBACK AgoraManager::StatsTimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    // 留空
}

// ── 预览截屏实现（GDI 截取 + GDI+ JPEG 编码）──────────

namespace {

// Base64 编码
std::string Base64Encode(const uint8_t* data, size_t len)
{
    static const char* t =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(4 * ((len + 2) / 3));
    unsigned val = 0;
    int bits = -6;
    for (size_t i = 0; i < len; i++) {
        val = (val << 8) + data[i];
        bits += 8;
        while (bits >= 0) {
            out.push_back(t[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(t[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// 查找 GDI+ JPEG 编码器的 CLSID
static bool GetJpegEncoderCLSID(CLSID* clsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<uint8_t> buf(size);
    auto* info = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(info[i].MimeType, L"image/jpeg") == 0) {
            *clsid = info[i].Clsid;
            return true;
        }
    }
    return false;
}

// GDI HBITMAP → JPEG 字节流（通过 GDI+ IStream）
std::vector<uint8_t> HBitmapToJpeg(HBITMAP hbm, int quality = 70)
{
    static CLSID jpegClsid{};
    static bool clsidOk = GetJpegEncoderCLSID(&jpegClsid);
    if (!clsidOk) return {};

    Gdiplus::Bitmap bmp(hbm, nullptr);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return {};

    IStream* stream = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (!stream) return {};

    Gdiplus::EncoderParameters ep{};
    ep.Count = 1;
    ep.Parameter[0].Guid           = Gdiplus::EncoderQuality;
    ep.Parameter[0].Type           = Gdiplus::EncoderParameterValueTypeLong;
    ep.Parameter[0].NumberOfValues = 1;
    ULONG q = (ULONG)quality;
    ep.Parameter[0].Value = &q;

    bmp.Save(stream, &jpegClsid, &ep);

    STATSTG stat{};
    stream->Stat(&stat, STATFLAG_NONAME);
    size_t sz = (size_t)stat.cbSize.QuadPart;
    std::vector<uint8_t> result(sz);
    LARGE_INTEGER li{};
    stream->Seek(li, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    stream->Read(result.data(), (ULONG)sz, &read);
    stream->Release();
    return result;
}

// 通过 EnumDisplayMonitors 查找第 N 个显示器的矩形
struct MonitorEnum { int target; int current; RECT rc; bool found; };
static BOOL CALLBACK MonitorEnumProc(HMONITOR, HDC, LPRECT lprc, LPARAM param)
{
    auto* d = reinterpret_cast<MonitorEnum*>(param);
    if (d->current == d->target) {
        d->rc = *lprc;
        d->found = true;
        return FALSE;
    }
    d->current++;
    return TRUE;
}

// GDI 截取指定显示器 → JPEG byte vector
std::vector<uint8_t> CaptureDisplayToJpeg(int displayIndex, int maxW, int maxH, int jpegQuality)
{
    MonitorEnum me{ displayIndex, 0, {}, false };
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc,
                        reinterpret_cast<LPARAM>(&me));
    if (!me.found) {
        me.rc.left = 0;  me.rc.top  = 0;
        me.rc.right  = GetSystemMetrics(SM_CXSCREEN);
        me.rc.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    int srcW = me.rc.right  - me.rc.left;
    int srcH = me.rc.bottom - me.rc.top;
    if (srcW <= 0 || srcH <= 0) return {};

    float aspect = (float)srcW / srcH;
    int dstW = maxW, dstH = maxH;
    if ((float)maxW / maxH > aspect) dstW = (int)(maxH * aspect);
    else                             dstH = (int)(maxW / aspect);
    dstW = (dstW + 1) & ~1;
    dstH = (dstH + 1) & ~1;
    if (dstW <= 0 || dstH <= 0) return {};

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm   = CreateCompatibleBitmap(hdcScreen, dstW, dstH);
    HGDIOBJ oldBm = SelectObject(hdcMem, hbm);

    SetStretchBltMode(hdcMem, HALFTONE);
    StretchBlt(hdcMem, 0, 0, dstW, dstH,
               hdcScreen, me.rc.left, me.rc.top, srcW, srcH, SRCCOPY);

    SelectObject(hdcMem, oldBm);
    auto jpeg = HBitmapToJpeg(hbm, jpegQuality);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return jpeg;
}

// GDI 截取指定窗口 → JPEG byte vector
std::vector<uint8_t> CaptureWindowToJpeg(int64_t windowId, int maxW, int maxH, int jpegQuality)
{
    HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(windowId));
    if (!IsWindow(hwnd)) return {};

    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return {};
    int srcW = wr.right  - wr.left;
    int srcH = wr.bottom - wr.top;
    if (srcW <= 0 || srcH <= 0) return {};

    float aspect = (float)srcW / srcH;
    int dstW = maxW, dstH = maxH;
    if ((float)maxW / maxH > aspect) dstW = (int)(maxH * aspect);
    else                             dstH = (int)(maxW / aspect);
    dstW = (dstW + 1) & ~1;
    dstH = (dstH + 1) & ~1;
    if (dstW <= 0 || dstH <= 0) return {};

    HDC hdcWin = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcWin);
    HBITMAP hbm   = CreateCompatibleBitmap(hdcWin, dstW, dstH);
    HGDIOBJ oldBm = SelectObject(hdcMem, hbm);

    SetStretchBltMode(hdcMem, HALFTONE);
    StretchBlt(hdcMem, 0, 0, dstW, dstH,
               hdcWin, wr.left, wr.top, srcW, srcH, SRCCOPY);

    SelectObject(hdcMem, oldBm);
    auto jpeg = HBitmapToJpeg(hbm, jpegQuality);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcWin);
    return jpeg;
}

} // anonymous namespace

void AgoraManager::StartPreviewLoop(const std::string& sourceType, int64_t sourceId)
{
    StopPreviewLoop();
    m_previewSourceType = sourceType;
    m_previewSourceId   = sourceId;
    m_previewRunning    = true;
    m_previewThread     = std::thread(&AgoraManager::PreviewThreadProc, this);
}

void AgoraManager::StopPreviewLoop()
{
    m_previewRunning = false;
    if (m_previewThread.joinable())
        m_previewThread.join();
}

void AgoraManager::PreviewThreadProc()
{
    // 960×540 JPEG quality 75，单帧 ~40-60KB base64，刷新间隔 200ms (5fps)
    constexpr int MAX_W       = 960;
    constexpr int MAX_H       = 540;
    constexpr int JPEG_Q      = 75;
    constexpr int INTERVAL_MS = 200;
    constexpr int SLEEP_STEP  = 50;

    // GDI+ 需要在线程中初始化
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartupInput si{};
    Gdiplus::GdiplusStartup(&gdipToken, &si, nullptr);

    while (m_previewRunning) {
        std::vector<uint8_t> jpeg;
        if (m_previewSourceType == "display")
            jpeg = CaptureDisplayToJpeg((int)m_previewSourceId, MAX_W, MAX_H, JPEG_Q);
        else
            jpeg = CaptureWindowToJpeg(m_previewSourceId, MAX_W, MAX_H, JPEG_Q);

        if (!jpeg.empty() && onPreviewFrame) {
            onPreviewFrame(Base64Encode(jpeg.data(), jpeg.size()));
        }

        for (int elapsed = 0; elapsed < INTERVAL_MS && m_previewRunning; elapsed += SLEEP_STEP)
            std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_STEP));
    }

    Gdiplus::GdiplusShutdown(gdipToken);
}

} // namespace MDShare
