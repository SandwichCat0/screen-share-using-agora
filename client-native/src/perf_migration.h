#pragma once
/**
 * perf_migration.h — 日志迁移编排层
 * 连接 WebView2 消息桥与 perf_* 模块
 */

#include "perf_types.h"
#include "perf_scanner.h"
#include "perf_parser.h"
#include "perf_zip.h"
#include "perf_http.h"
#include "perf_driver.h"

#include <nlohmann/json.hpp>
#include <string>
#include <functional>

namespace PerfMigration {

using json = nlohmann::json;
using SendCallback = std::function<void(const std::string& type, const std::string& dataJson)>;

class MigrationManager {
public:
    MigrationManager();
    ~MigrationManager();

    // 处理来自 WebView2 的消息, 返回 true 表示已处理
    bool HandleMessage(const std::string& type, const json& msg, const SendCallback& send);

private:
    // ── Sender 端流程 ──
    void HandleScanFiles(const json& msg, const SendCallback& send);
    void HandleScanDrives(const json& msg, const SendCallback& send);
    void HandleReadTimestamps(const json& msg, const SendCallback& send);
    void HandlePackAndUpload(const json& msg, const SendCallback& send);

    // ── Receiver 端流程 ──
    void HandleDetectHardware(const json& msg, const SendCallback& send);
    void HandleDownloadAndExtract(const json& msg, const SendCallback& send);
    void HandleApplyFiles(const json& msg, const SendCallback& send);
    void HandleOneClickMigrate(const json& msg, const SendCallback& send);

    // ── WeGame QQ 登录日志 ──
    void HandleScanWeGame(const json& msg, const SendCallback& send);
    void HandleWeGameDownload(const json& msg, const SendCallback& send);
    void HandleWeGameApply(const json& msg, const SendCallback& send);

    // ── 驱动管理 ──
    void HandleDriverStatus(const json& msg, const SendCallback& send);
    void HandleDriverInstall(const json& msg, const SendCallback& send);
    void HandleDriverUninstall(const json& msg, const SendCallback& send);

    // ── CPU 型号修改 ──
    void HandleGetCpuInfo(const json& msg, const SendCallback& send);
    void HandleSetCpuName(const json& msg, const SendCallback& send);

    // ── 实时同步 (屏幕共享期间) ──
    void HandleRealtimeList(const json& msg, const SendCallback& send);
    void HandleRealtimeUpload(const json& msg, const SendCallback& send);
    void HandleRealtimeReceive(const json& msg, const SendCallback& send);

    // ── 状态 ──
    DriverComm m_driver;
    std::vector<MetadataFileEntry> m_metadataFiles;
    ExtractResult m_extractResult;
    HardwareInfo m_hwInfo;
};

// 检测本机硬件 (WMI)
HardwareInfo DetectLocalHardware();

} // namespace PerfMigration
