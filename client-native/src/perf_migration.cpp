/**
 * perf_migration.cpp — 日志迁移编排层
 * 所有 perf-* WebView2 消息在此分发处理
 */

#include "perf_migration.h"

#include <winsock2.h>
#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <iphlpapi.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <random>

namespace PerfMigration {

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════
// 硬件检测 (复用已有 WMI 基础设施, 移植自 HardwareDetector)
// ═══════════════════════════════════════════════════════

static std::string GetFirstMacAddress() {
    ULONG bufSize = 0;
    GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, nullptr, &bufSize);
    if (bufSize == 0) return "";

    std::vector<uint8_t> buffer(bufSize);
    PIP_ADAPTER_ADDRESSES pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, pAddresses, &bufSize) != ERROR_SUCCESS) return "";

    for (auto pAddr = pAddresses; pAddr; pAddr = pAddr->Next) {
        if (pAddr->PhysicalAddressLength == 6 && pAddr->IfType != IF_TYPE_SOFTWARE_LOOPBACK) {
            std::ostringstream ss;
            for (int i = 0; i < 6; i++)
                ss << std::hex << std::setw(2) << std::setfill('0') << (int)pAddr->PhysicalAddress[i];
            std::string mac = ss.str();
            for (auto& c : mac) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            return mac;
        }
    }
    return "";
}

static std::string GetOSVersion() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                       "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                       0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return "10.0.0";

    auto readStr = [&](const char* name) -> std::string {
        char buf[256] = {};
        DWORD size = sizeof(buf), type;
        if (RegQueryValueExA(hKey, name, nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS)
            return std::string(buf);
        return "";
    };
    auto readDword = [&](const char* name) -> DWORD {
        DWORD val = 0, size = sizeof(val), type;
        if (RegQueryValueExA(hKey, name, nullptr, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS) return val;
        return 0;
    };

    std::string major = readStr("CurrentMajorVersionNumber");
    if (major.empty()) major = "10";
    DWORD minorNum = readDword("CurrentMinorVersionNumber");
    std::string build = readStr("CurrentBuildNumber");
    RegCloseKey(hKey);

    return major + "." + std::to_string(minorNum) + "." + build;
}

// WMI 查询用内部类
class WmiQuery {
public:
    WmiQuery() : m_pLoc(nullptr), m_pSvc(nullptr), m_initialized(false) {}
    ~WmiQuery() {
        if (m_pSvc) m_pSvc->Release();
        if (m_pLoc) m_pLoc->Release();
        if (m_initialized) CoUninitialize();
    }

    bool Init() {
        HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
        m_initialized = true;

        hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                                  RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                                  nullptr, EOAC_NONE, nullptr);

        hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                              IID_IWbemLocator, (LPVOID*)&m_pLoc);
        if (FAILED(hr)) return false;

        hr = m_pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, 0, 0, 0, 0, &m_pSvc);
        if (FAILED(hr)) return false;

        hr = CoSetProxyBlanket(m_pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                               RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                               nullptr, EOAC_NONE);
        return SUCCEEDED(hr);
    }

    std::string QuerySingle(const wchar_t* wql, const wchar_t* property) {
        if (!m_pSvc) return "";
        IEnumWbemClassObject* pEnumerator = nullptr;
        HRESULT hr = m_pSvc->ExecQuery(bstr_t("WQL"), bstr_t(wql),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator);
        if (FAILED(hr) || !pEnumerator) return "";

        IWbemClassObject* pObj = nullptr;
        ULONG uReturn = 0;
        std::string result;

        if (pEnumerator->Next(WBEM_INFINITE, 1, &pObj, &uReturn) == WBEM_S_NO_ERROR && uReturn) {
            VARIANT vtProp;
            hr = pObj->Get(property, 0, &vtProp, 0, 0);
            if (SUCCEEDED(hr) && vtProp.vt == VT_BSTR) {
                _bstr_t bstr(vtProp.bstrVal);
                result = (const char*)bstr;
            }
            VariantClear(&vtProp);
            pObj->Release();
        }
        pEnumerator->Release();
        return result;
    }

private:
    IWbemLocator* m_pLoc;
    IWbemServices* m_pSvc;
    bool m_initialized;
};

// 前向声明 — 定义在文件后部的 CPU 伪装辅助函数
static std::string ReadSavedFakeCpuName();

HardwareInfo DetectLocalHardware() {
    HardwareInfo info;
    info.osName = "Windows";
    info.architecture = "x64";
    info.osVersion = GetOSVersion();

    WmiQuery wmi;
    if (wmi.Init()) {
        info.gpuModel = wmi.QuerySingle(L"SELECT Name FROM Win32_VideoController", L"Name");
        info.driverVersion = wmi.QuerySingle(L"SELECT DriverVersion FROM Win32_VideoController", L"DriverVersion");
        if (info.gpuModel.find("NVIDIA") != std::string::npos) info.gpuVendor = "NVIDIA";
        else if (info.gpuModel.find("AMD") != std::string::npos || info.gpuModel.find("Radeon") != std::string::npos) info.gpuVendor = "AMD";
        else if (info.gpuModel.find("Intel") != std::string::npos) info.gpuVendor = "Intel";
        info.cpuModel = wmi.QuerySingle(L"SELECT Name FROM Win32_Processor", L"Name");
    }
    info.macAddress = GetFirstMacAddress();

    // 如果已配置了伪装 CPU 名称（未过期），用它覆盖 WMI 返回的真实值
    // 确保 perf_data protobuf 中的 cpuModel 与注册表 ProcessorNameString 一致
    {
        std::string fakeCpu = ReadSavedFakeCpuName();
        if (!fakeCpu.empty()) {
            info.cpuModel = fakeCpu;
        }
    }

    // 从本机已有的 perf_data 文件中读取 deviceId / osVersion / hardwareId
    // 这些字段无法通过 WMI 获取，只能从已有文件的 protobuf header 中提取
    try {
        ScanResult sr = FileScanner::AutoScan();
        // AutoScan 未找到，尝试全盘扫描
        if (sr.status != ScanStatus::Completed || sr.files.empty()) {
            auto candidates = FileScanner::RecursiveScanDrives(9);
            for (const auto& dir : candidates) {
                sr = FileScanner::ScanDirectory(dir);
                if (sr.status == ScanStatus::Completed && !sr.files.empty()) break;
            }
        }
        // 选取最新的 perf_data 文件解析
        if (sr.status == ScanStatus::Completed && !sr.files.empty()) {
            // 按修改时间降序，取最新的文件
            std::string newestFile = sr.files[0].fullPath;
            for (const auto& f : sr.files) {
                if (f.modificationTime > sr.files[0].modificationTime)
                    newestFile = f.fullPath;
            }
            PerfDataParser parser(newestFile);
            if (parser.Load() && parser.ParseHeader()) {
                const auto& kf = parser.GetResult().header.knownFields;
                if (!kf.deviceId.empty())     info.deviceId     = kf.deviceId;
                if (!kf.osVersion.empty())    info.osVersion    = kf.osVersion;
                if (!kf.hardwareId1.empty())  info.hardwareId1  = kf.hardwareId1;
                if (!kf.hardwareId2.empty())  info.hardwareId2  = kf.hardwareId2;
            }
        }
    } catch (...) {
        // 解析失败不影响其他硬件检测结果
    }

    return info;
}

// ═══════════════════════════════════════════════════════
// MigrationManager
// ═══════════════════════════════════════════════════════

MigrationManager::MigrationManager() {
    // 启动时清理上次可能残留的临时文件
    CleanupResidualTempFiles();
}

MigrationManager::~MigrationManager() {
    if (!m_extractResult.tempDir.empty()) {
        CleanupTempDir(m_extractResult.tempDir);
    }
}

bool MigrationManager::HandleMessage(const std::string& type, const json& msg, const SendCallback& send) {
    // Sender
    if (type == "perf-scan-files")         { HandleScanFiles(msg, send); return true; }
    if (type == "perf-scan-drives")        { HandleScanDrives(msg, send); return true; }
    if (type == "perf-read-timestamps")    { HandleReadTimestamps(msg, send); return true; }
    if (type == "perf-pack-upload")        { HandlePackAndUpload(msg, send); return true; }
    // Receiver
    if (type == "perf-detect-hardware")    { HandleDetectHardware(msg, send); return true; }
    if (type == "perf-download-extract")   { HandleDownloadAndExtract(msg, send); return true; }
    if (type == "perf-apply")              { HandleApplyFiles(msg, send); return true; }
    if (type == "perf-one-click-migrate")  { HandleOneClickMigrate(msg, send); return true; }
    // WeGame QQ
    if (type == "perf-scan-wegame")        { HandleScanWeGame(msg, send); return true; }
    if (type == "perf-wegame-download")    { HandleWeGameDownload(msg, send); return true; }
    if (type == "perf-wegame-apply")       { HandleWeGameApply(msg, send); return true; }
    // Driver
    if (type == "perf-driver-status")      { HandleDriverStatus(msg, send); return true; }
    if (type == "perf-driver-install")     { HandleDriverInstall(msg, send); return true; }
    if (type == "perf-driver-uninstall")   { HandleDriverUninstall(msg, send); return true; }
    // CPU
    if (type == "perf-get-cpu-info")       { HandleGetCpuInfo(msg, send); return true; }
    if (type == "perf-set-cpu-name")       { HandleSetCpuName(msg, send); return true; }
    // Realtime sync (屏幕共享期间)
    if (type == "perf-realtime-list")      { HandleRealtimeList(msg, send); return true; }
    if (type == "perf-realtime-upload")    { HandleRealtimeUpload(msg, send); return true; }
    if (type == "perf-realtime-receive")   { HandleRealtimeReceive(msg, send); return true; }
    return false;
}

// ═══════════════════════════════════════════════════════
// Sender 端
// ═══════════════════════════════════════════════════════

void MigrationManager::HandleScanFiles(const json& msg, const SendCallback& send) {
    json result;
    try {
        std::string dirPath = msg.value("path", "");
        ScanResult sr;
        if (dirPath.empty()) {
            sr = FileScanner::AutoScan();
            // 自动扫描失败时，回退到全盘搜索
            if (sr.status != ScanStatus::Completed || sr.files.empty()) {
                auto candidates = FileScanner::RecursiveScanDrives(9);
                if (!candidates.empty()) {
                    if (candidates.size() == 1) {
                        // 只有一个候选→直接扫描
                        sr = FileScanner::ScanDirectory(candidates[0]);
                    } else {
                        // 多个候选→返回给前端让用户选
                        json driveResult;
                        driveResult["success"] = true;
                        driveResult["candidates"] = candidates;
                        send("perf-scan-drives-result", driveResult.dump());
                        return;
                    }
                }
            }
        } else {
            sr = FileScanner::ScanDirectory(dirPath);
        }

        result["success"] = (sr.status == ScanStatus::Completed);
        result["targetPath"] = sr.targetPath;
        result["errorMessage"] = sr.errorMessage;

        json files = json::array();
        for (const auto& f : sr.files) {
            json fj;
            fj["filename"] = f.filename;
            fj["fullPath"] = f.fullPath;
            fj["fileSize"] = f.fileSize;
            fj["creationTime"] = f.creationTime;
            fj["modificationTime"] = f.modificationTime;
            fj["accessTime"] = f.accessTime;
            fj["filenameTimestamp"] = f.filenameTimestamp;
            fj["isTimeAnomaly"] = f.isTimeAnomaly;
            fj["creationTimeAnomaly"] = f.creationTimeAnomaly;
            fj["modificationTimeAnomaly"] = f.modificationTimeAnomaly;
            files.push_back(fj);
        }
        result["files"] = files;
    } catch (const std::exception& e) {
        result["success"] = false;
        result["errorMessage"] = e.what();
    }
    send("perf-scan-result", result.dump());
}

void MigrationManager::HandleScanDrives(const json& /*msg*/, const SendCallback& send) {
    json result;
    try {
        auto candidates = FileScanner::RecursiveScanDrives(9);
        result["success"] = !candidates.empty();
        result["candidates"] = candidates;
    } catch (const std::exception& e) {
        result["success"] = false;
        result["errorMessage"] = e.what();
    }
    send("perf-scan-drives-result", result.dump());
}

void MigrationManager::HandleReadTimestamps(const json& msg, const SendCallback& send) {
    json result;
    try {
        std::string filePath = msg.value("filePath", "");
        FileTimestamps ts;
        if (ReadFileTimestamps(filePath, ts)) {
            result["success"] = true;
            result["creationTime"] = ts.creationTime;
            result["lastAccessTime"] = ts.lastAccessTime;
            result["lastWriteTime"] = ts.lastWriteTime;
            result["changeTime"] = ts.changeTime;
        } else {
            result["success"] = false;
            result["errorMessage"] = "Failed to read timestamps";
        }
    } catch (const std::exception& e) {
        result["success"] = false;
        result["errorMessage"] = e.what();
    }
    send("perf-timestamps-result", result.dump());
}

void MigrationManager::HandlePackAndUpload(const json& msg, const SendCallback& send) {
    json result;
    try {
        std::string serverUrl = msg.value("serverUrl", "");
        auto files = msg.value("files", json::array());

        // 构建 PackEntry
        std::vector<PackEntry> entries;
        for (const auto& f : files) {
            PackEntry pe;
            pe.fullPath = f.value("fullPath", "");
            pe.filename = f.value("filename", "");
            if (!ReadFileTimestamps(pe.fullPath, pe.timestamps)) {
                pe.timestamps = {}; // 读取失败则使用零值
            }
            entries.push_back(pe);
        }

        // 打包 ZIP
        std::string tempZip = MakeTempPath(kTempPrefixUpload, kTempSuffixCab);

        send("perf-progress", json({{"stage", "packing"}, {"progress", 0.2}}).dump());

        if (!CreateZipPackage(tempZip, entries)) {
            result["success"] = false;
            result["errorMessage"] = "Failed to create ZIP";
            send("perf-upload-result", result.dump());
            return;
        }

        send("perf-progress", json({{"stage", "uploading"}, {"progress", 0.5}}).dump());

        // 上传
        UploadResult ur = UploadZip(serverUrl, tempZip);

        // 清理临时文件
        try { fs::remove(tempZip); } catch (...) {}

        result["success"] = ur.success;
        result["token"] = ur.token;
        result["downloadUrl"] = ur.downloadUrl;
        result["downloadKey"] = ur.downloadKey;
        result["errorMessage"] = ur.errorMessage;
    } catch (const std::exception& e) {
        result["success"] = false;
        result["errorMessage"] = e.what();
    }
    send("perf-upload-result", result.dump());
}

// ═══════════════════════════════════════════════════════
// Receiver 端
// ═══════════════════════════════════════════════════════

void MigrationManager::HandleDetectHardware(const json& /*msg*/, const SendCallback& send) {
    json result;
    try {
        m_hwInfo = DetectLocalHardware();
        result["success"] = true;
        result["gpuVendor"] = m_hwInfo.gpuVendor;
        result["gpuModel"] = m_hwInfo.gpuModel;
        result["driverVersion"] = m_hwInfo.driverVersion;
        result["cpuModel"] = m_hwInfo.cpuModel;
        result["macAddress"] = m_hwInfo.macAddress;
        result["osName"] = m_hwInfo.osName;
        result["osVersion"] = m_hwInfo.osVersion;
        result["architecture"] = m_hwInfo.architecture;
    } catch (const std::exception& e) {
        result["success"] = false;
        result["errorMessage"] = e.what();
    }
    send("perf-hardware-result", result.dump());
}

void MigrationManager::HandleDownloadAndExtract(const json& msg, const SendCallback& send) {
    json result;
    try {
        std::string serverUrl = msg.value("serverUrl", "");
        std::string token = msg.value("token", "");
        std::string downloadKey = msg.value("downloadKey", "");

        // 也支持完整 URL 解析
        std::string fullUrl = msg.value("fullUrl", "");
        if (!fullUrl.empty()) {
            size_t apiPos = fullUrl.find("/api/download/");
            if (apiPos != std::string::npos) {
                serverUrl = fullUrl.substr(0, apiPos);
                std::string rest = fullUrl.substr(apiPos + 14);
                size_t qPos = rest.find('?');
                if (qPos != std::string::npos) {
                    token = rest.substr(0, qPos);
                    std::string params = rest.substr(qPos + 1);
                    size_t keyPos = params.find("key=");
                    if (keyPos != std::string::npos) {
                        downloadKey = params.substr(keyPos + 4);
                        size_t ampPos = downloadKey.find('&');
                        if (ampPos != std::string::npos) downloadKey = downloadKey.substr(0, ampPos);
                    }
                } else {
                    token = rest;
                }
            }
        }

        send("perf-progress", json({{"stage", "downloading"}, {"progress", 0.1}}).dump());

        // 下载
        std::string tempZip = MakeTempPath(kTempPrefixDownload, kTempSuffixCab);

        DownloadResult dr = DownloadZip(serverUrl, token, downloadKey, tempZip);
        if (!dr.success) {
            result["success"] = false;
            result["errorMessage"] = "Download failed: " + dr.errorMessage;
            send("perf-download-result", result.dump());
            return;
        }

        send("perf-progress", json({{"stage", "extracting"}, {"progress", 0.5}}).dump());

        // 解压
        if (!m_extractResult.tempDir.empty()) CleanupTempDir(m_extractResult.tempDir);
        m_extractResult = ExtractZipPackage(tempZip);

        try { fs::remove(tempZip); } catch (...) {}

        if (!m_extractResult.success) {
            result["success"] = false;
            result["errorMessage"] = "Extract failed: " + m_extractResult.errorMessage;
            send("perf-download-result", result.dump());
            return;
        }

        // 解析 metadata
        m_metadataFiles.clear();
        try {
            json meta = json::parse(m_extractResult.metadataJson);
            for (const auto& f : meta["files"]) {
                MetadataFileEntry mfe;
                mfe.filename = f.value("filename", "");
                auto& ts = f["timestamps"];
                mfe.creationTime = ts.value("creation_time", (int64_t)0);
                mfe.lastAccessTime = ts.value("last_access_time", (int64_t)0);
                mfe.lastWriteTime = ts.value("last_write_time", (int64_t)0);
                mfe.changeTime = ts.value("change_time", (int64_t)0);
                m_metadataFiles.push_back(mfe);
            }
        } catch (...) {}

        result["success"] = true;
        result["fileCount"] = (int)m_extractResult.files.size();
        json fileList = json::array();
        for (const auto& ef : m_extractResult.files) {
            fileList.push_back({{"filename", ef.filename}});
        }
        result["files"] = fileList;
    } catch (const std::exception& e) {
        result["success"] = false;
        result["errorMessage"] = e.what();
    }
    send("perf-download-result", result.dump());
}

void MigrationManager::HandleApplyFiles(const json& msg, const SendCallback& send) {
    json result;
    try {
        std::string targetDir = msg.value("targetPath", "");
        int fakeDays = msg.value("fakeDays", 30);
        std::string sysFilePath = msg.value("sysFilePath", "");

        // 自动检测目标路径
        if (targetDir.empty()) {
            std::string regPath = FileScanner::ReadInstallPathFromRegistry();
            if (!regPath.empty()) {
                targetDir = FileScanner::BuildPerformancePath(regPath);
            }
        }

        if (targetDir.empty()) {
            result["success"] = false;
            result["errorMessage"] = "Target path not specified";
            send("perf-apply-result", result.dump());
            return;
        }

        // 硬件字段 (从前端传来或使用本地检测)
        KnownFields modifiedFields;
        modifiedFields.gpuVendor = msg.value("gpuVendor", m_hwInfo.gpuVendor);
        modifiedFields.gpuModel = msg.value("gpuModel", m_hwInfo.gpuModel);
        modifiedFields.driverVersion = msg.value("driverVersion", m_hwInfo.driverVersion);
        modifiedFields.cpuModel = msg.value("cpuModel", m_hwInfo.cpuModel);
        modifiedFields.macAddress = msg.value("macAddress", m_hwInfo.macAddress);
        modifiedFields.deviceId = msg.value("deviceId", m_hwInfo.deviceId);
        modifiedFields.hardwareId1 = msg.value("hardwareId1", m_hwInfo.hardwareId1);
        modifiedFields.hardwareId2 = msg.value("hardwareId2", m_hwInfo.hardwareId2);
        modifiedFields.osVersion = msg.value("osVersion", m_hwInfo.osVersion);
        modifiedFields.osName = "Windows";
        modifiedFields.architecture = "x64";

        // 1. 确保目标目录存在
        auto fsTargetDir = Utf8ToPath(targetDir);
        if (!fs::exists(fsTargetDir)) {
            try { fs::create_directories(fsTargetDir); } catch (...) {}
        }

        // 2. 复制文件 + 修改硬件字段
        int fileCount = static_cast<int>(m_extractResult.files.size());
        int successCount = 0;

        for (int i = 0; i < fileCount; i++) {
            const auto& ef = m_extractResult.files[i];
            auto fsDest = fsTargetDir / Utf8ToPath(ef.filename);
            std::string destPath = PathToUtf8(fsDest);

            float progress = 0.1f + static_cast<float>(i) / fileCount * 0.5f;
            send("perf-progress", json({{"stage", "processing"}, {"progress", progress},
                                         {"current", i + 1}, {"total", fileCount}}).dump());

            try {
                fs::copy_file(Utf8ToPath(ef.extractedPath), fsDest, fs::copy_options::overwrite_existing);
            } catch (...) { continue; }

            // 修改 protobuf header
            try {
                PerfDataParser parser(destPath);
                if (parser.Load() && parser.ParseHeader()) {
                    auto newHeader = parser.ReserializeHeader(modifiedFields);
                    const auto& compressedData = parser.GetCompressedData();

                    std::vector<uint8_t> newFile;
                    newFile.insert(newFile.end(), newHeader.begin(), newHeader.end());
                    newFile.insert(newFile.end(), compressedData.begin(), compressedData.end());

                    std::ofstream outFile(Utf8ToPath(destPath), std::ios::binary);
                    outFile.write(reinterpret_cast<const char*>(newFile.data()), newFile.size());
                    outFile.close();
                }
            } catch (...) {}

            successCount++;
        }

        // 3. 设置文件时间戳 ($STANDARD_INFORMATION + $FILE_NAME)
        send("perf-progress", json({{"stage", "timestamps"}, {"progress", 0.65}}).dump());

        int tsSetCount = 0;
        for (size_t i = 0; i < m_metadataFiles.size(); i++) {
            const auto& mf = m_metadataFiles[i];
            auto fsDest = fsTargetDir / Utf8ToPath(mf.filename);
            std::string destPath = PathToUtf8(fsDest);
            if (!fs::exists(fsDest)) continue;

            // SetFileInformationByHandle (SI)
            HANDLE hFile = CreateFileW(fsDest.c_str(), FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) continue;

            FILE_BASIC_INFO fbi = {};
            fbi.CreationTime.QuadPart = mf.creationTime;
            fbi.LastAccessTime.QuadPart = mf.lastAccessTime;
            fbi.LastWriteTime.QuadPart = mf.lastWriteTime;
            fbi.ChangeTime.QuadPart = mf.changeTime;

            SetFileInformationByHandle(hFile, FileBasicInfo, &fbi, sizeof(fbi));
            CloseHandle(hFile);

            // 重命名技巧: 触发 $SI → $FN 同步
            std::wstring wDestPath = fsDest.native();
            std::wstring wTempPath = wDestPath + L".etw";
            if (MoveFileW(wDestPath.c_str(), wTempPath.c_str()))
                MoveFileW(wTempPath.c_str(), wDestPath.c_str());

            // 再次设置 (重命名可能更新了 $SI)
            hFile = CreateFileW(fsDest.c_str(), FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                SetFileInformationByHandle(hFile, FileBasicInfo, &fbi, sizeof(fbi));
                CloseHandle(hFile);
            }
            tsSetCount++;
        }

        // 4. 驱动时间戳伪造
        send("perf-progress", json({{"stage", "driver"}, {"progress", 0.7}}).dump());

        bool driverSuccess = false;

        // 查找 fsutil.sys
        if (sysFilePath.empty()) {
            // 查找 DLL 同目录下的 fsutil.sys
            wchar_t dllPath[MAX_PATH];
            HMODULE hMod = nullptr;
            // 使用 DetectLocalHardware 的地址定位当前 DLL
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&DetectLocalHardware), &hMod);
            if (hMod) {
                GetModuleFileNameW(hMod, dllPath, MAX_PATH);
                fs::path p(dllPath);
                fs::path candidate = p.parent_path() / "fsutil.sys";
                if (fs::exists(candidate)) sysFilePath = candidate.string();
            }
        }

        // sysFilePath 为空时 EnsureDriverReady 会自动从 DLL 资源中提取 fsutil.sys
        if (!m_driver.EnsureDriverReady(sysFilePath)) {
            // 驱动安装失败，记录但不阻断迁移
            result["driverError"] = m_driver.GetLastError();
        }

        if (m_driver.IsConnected() || m_driver.Connect()) {
            m_driver.ClearAllFakeEntries();

            // Attach volume
            if (targetDir.length() >= 2 && targetDir[1] == ':') {
                m_driver.AttachToVolume(targetDir.substr(0, 2));
            }

            // 计算过期时间
            int64_t expirationTime = 0;
            if (fakeDays > 0) {
                FILETIME ft;
                GetSystemTimeAsFileTime(&ft);
                ULARGE_INTEGER uli;
                uli.LowPart = ft.dwLowDateTime;
                uli.HighPart = ft.dwHighDateTime;
                uli.QuadPart += (int64_t)fakeDays * 24LL * 3600LL * 10000000LL;
                expirationTime = (int64_t)uli.QuadPart;
            }

            int tsSuccess = 0;
            for (size_t i = 0; i < m_metadataFiles.size(); i++) {
                const auto& mf = m_metadataFiles[i];
                std::string destPath = targetDir + "\\" + mf.filename;
                HRESULT hr = m_driver.AddFakeTimestampEntry(destPath,
                    mf.creationTime, mf.lastAccessTime, mf.lastWriteTime,
                    mf.changeTime, expirationTime);
                if (SUCCEEDED(hr)) tsSuccess++;
            }

            driverSuccess = (tsSuccess > 0);
        }

        result["success"] = (successCount > 0);
        result["filesProcessed"] = successCount;
        result["totalFiles"] = fileCount;
        result["timestampsSet"] = tsSetCount;
        result["driverConnected"] = m_driver.IsConnected();
        result["driverFakeEntries"] = driverSuccess;

        // 附带已接收文件列表及对局结束时间（创建时间）
        json receivedFiles = json::array();
        for (const auto& mf : m_metadataFiles) {
            json rf;
            rf["filename"] = mf.filename;
            // 将 FILETIME int64 转为可读时间
            if (mf.creationTime != 0) {
                FILETIME ft;
                ULARGE_INTEGER uli;
                uli.QuadPart = static_cast<uint64_t>(mf.creationTime);
                ft.dwLowDateTime = uli.LowPart;
                ft.dwHighDateTime = uli.HighPart;
                SYSTEMTIME stUTC, stLocal;
                FileTimeToSystemTime(&ft, &stUTC);
                SystemTimeToTzSpecificLocalTime(nullptr, &stUTC, &stLocal);
                char buf[64];
                sprintf_s(buf, "%04d-%02d-%02d %02d:%02d:%02d",
                    stLocal.wYear, stLocal.wMonth, stLocal.wDay,
                    stLocal.wHour, stLocal.wMinute, stLocal.wSecond);
                rf["creationTime"] = std::string(buf);
            } else {
                rf["creationTime"] = "";
            }
            receivedFiles.push_back(rf);
        }
        result["receivedFiles"] = receivedFiles;

        // 清理
        CleanupTempDir(m_extractResult.tempDir);
        m_extractResult.tempDir.clear();
    } catch (const std::exception& e) {
        result["success"] = false;
        result["errorMessage"] = e.what();
    }
    send("perf-apply-result", result.dump());
}

// ═══════════════════════════════════════════════════════
// 一键迁移 (接收方: 下载 + 硬件检测 + 应用)
// ═══════════════════════════════════════════════════════

void MigrationManager::HandleOneClickMigrate(const json& msg, const SendCallback& send) {
    json result;
    try {
        std::string serverUrl = msg.value("serverUrl", "");
        std::string token = msg.value("token", "");
        std::string downloadKey = msg.value("downloadKey", "");
        std::string targetDir = msg.value("targetPath", "");
        int fakeDays = msg.value("fakeDays", 15);

        if (serverUrl.empty() || token.empty()) {
            result["success"] = false;
            result["errorMessage"] = "Missing serverUrl or token";
            send("perf-one-click-result", result.dump());
            return;
        }

        // ── 1. 下载 ──
        send("perf-progress", json({{"stage", "downloading"}, {"progress", 0.05}}).dump());

        std::string tempZip = MakeTempPath(kTempPrefixDownload, kTempSuffixCab);

        DownloadResult dr = DownloadZip(serverUrl, token, downloadKey, tempZip);
        if (!dr.success) {
            result["success"] = false;
            result["errorMessage"] = "Download failed: " + dr.errorMessage;
            send("perf-one-click-result", result.dump());
            return;
        }

        // ── 2. 解压 ──
        send("perf-progress", json({{"stage", "extracting"}, {"progress", 0.2}}).dump());

        if (!m_extractResult.tempDir.empty()) CleanupTempDir(m_extractResult.tempDir);
        m_extractResult = ExtractZipPackage(tempZip);
        try { fs::remove(Utf8ToPath(tempZip)); } catch (...) {}

        if (!m_extractResult.success) {
            result["success"] = false;
            result["errorMessage"] = "Extract failed: " + m_extractResult.errorMessage;
            send("perf-one-click-result", result.dump());
            return;
        }

        // 解析 metadata
        m_metadataFiles.clear();
        try {
            json meta = json::parse(m_extractResult.metadataJson);
            for (const auto& f : meta["files"]) {
                MetadataFileEntry mfe;
                mfe.filename = f.value("filename", "");
                auto& ts = f["timestamps"];
                mfe.creationTime = ts.value("creation_time", (int64_t)0);
                mfe.lastAccessTime = ts.value("last_access_time", (int64_t)0);
                mfe.lastWriteTime = ts.value("last_write_time", (int64_t)0);
                mfe.changeTime = ts.value("change_time", (int64_t)0);
                m_metadataFiles.push_back(mfe);
            }
        } catch (...) {}

        // ── 3. 硬件检测 ──
        send("perf-progress", json({{"stage", "detecting"}, {"progress", 0.3}}).dump());
        m_hwInfo = DetectLocalHardware();

        // ── 4. 应用文件 ──
        // 构造一个与 HandleApplyFiles 兼容的 msg
        json applyMsg;
        applyMsg["targetPath"] = targetDir;
        applyMsg["fakeDays"] = fakeDays;
        // 使用检测到的本机硬件
        applyMsg["gpuVendor"] = m_hwInfo.gpuVendor;
        applyMsg["gpuModel"] = m_hwInfo.gpuModel;
        applyMsg["driverVersion"] = m_hwInfo.driverVersion;
        applyMsg["cpuModel"] = m_hwInfo.cpuModel;
        applyMsg["macAddress"] = m_hwInfo.macAddress;
        applyMsg["deviceId"] = m_hwInfo.deviceId;
        applyMsg["hardwareId1"] = m_hwInfo.hardwareId1;
        applyMsg["hardwareId2"] = m_hwInfo.hardwareId2;
        applyMsg["osVersion"] = m_hwInfo.osVersion;

        // 直接调用 HandleApplyFiles, 但把最终回复类型替换
        HandleApplyFiles(applyMsg, [&send](const std::string& type, const std::string& dataJson) {
            if (type == "perf-apply-result") {
                send("perf-one-click-result", dataJson);
            } else {
                send(type, dataJson);  // perf-progress 等直接透传
            }
        });
        return; // HandleApplyFiles 已发送结果

    } catch (const std::exception& e) {
        result["success"] = false;
        result["errorMessage"] = e.what();
    }
    send("perf-one-click-result", result.dump());
}

// ═══════════════════════════════════════════════════════
// WeGame QQ 登录日志迁移
// ═══════════════════════════════════════════════════════

// FILETIME int64 → "YYYY-MM-DD HH:MM:SS" 本地时间
static std::string FormatFileTimeI64(int64_t ft64) {
    if (ft64 == 0) return "";
    FILETIME ft;
    ULARGE_INTEGER uli;
    uli.QuadPart = static_cast<uint64_t>(ft64);
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    SYSTEMTIME stUTC, stLocal;
    FileTimeToSystemTime(&ft, &stUTC);
    SystemTimeToTzSpecificLocalTime(nullptr, &stUTC, &stLocal);
    char buf[64];
    sprintf_s(buf, "%04d-%02d-%02d %02d:%02d:%02d",
        stLocal.wYear, stLocal.wMonth, stLocal.wDay,
        stLocal.wHour, stLocal.wMinute, stLocal.wSecond);
    return std::string(buf);
}

// FILETIME int64 → "YYYY-MM-DDTHH:MM:SS" (用于 datetime-local input)
static std::string FormatFileTimeI64Local(int64_t ft64) {
    if (ft64 == 0) return "";
    FILETIME ft;
    ULARGE_INTEGER uli;
    uli.QuadPart = static_cast<uint64_t>(ft64);
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    SYSTEMTIME stUTC, stLocal;
    FileTimeToSystemTime(&ft, &stUTC);
    SystemTimeToTzSpecificLocalTime(nullptr, &stUTC, &stLocal);
    char buf[64];
    sprintf_s(buf, "%04d-%02d-%02dT%02d:%02d:%02d",
        stLocal.wYear, stLocal.wMonth, stLocal.wDay,
        stLocal.wHour, stLocal.wMinute, stLocal.wSecond);
    return std::string(buf);
}

// 解析 "YYYY-MM-DDTHH:MM:SS" 或 "YYYY-MM-DD HH:MM:SS" → FILETIME int64
static int64_t ParseDateTimeToFileTime(const std::string& s) {
    if (s.empty()) return 0;
    SYSTEMTIME st = {};
    // 支持 T 或空格分隔
    int r = sscanf_s(s.c_str(), "%hd-%hd-%hdT%hd:%hd:%hd",
        &st.wYear, &st.wMonth, &st.wDay, &st.wHour, &st.wMinute, &st.wSecond);
    if (r < 6) {
        r = sscanf_s(s.c_str(), "%hd-%hd-%hd %hd:%hd:%hd",
            &st.wYear, &st.wMonth, &st.wDay, &st.wHour, &st.wMinute, &st.wSecond);
    }
    if (r < 6) return 0;
    FILETIME ftLocal, ftUTC;
    SystemTimeToFileTime(&st, &ftLocal);
    LocalFileTimeToFileTime(&ftLocal, &ftUTC);
    ULARGE_INTEGER uli;
    uli.LowPart = ftUTC.dwLowDateTime;
    uli.HighPart = ftUTC.dwHighDateTime;
    return static_cast<int64_t>(uli.QuadPart);
}

// 获取 WeGame login_pic 路径
static std::string GetWeGameLoginPicPath() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (len == 0) return "";
    fs::path p(buf);
    p = p / "Tencent" / "WeGame" / "login_pic";
    return PathToUtf8(p);
}

// 扫描 login_pic 目录中无后缀名文件
static json ScanWeGameFiles(const std::string& dir) {
    json files = json::array();
    auto fsDir = Utf8ToPath(dir);
    if (!fs::exists(fsDir) || !fs::is_directory(fsDir)) return files;

    for (const auto& entry : fs::directory_iterator(fsDir)) {
        if (!entry.is_regular_file()) continue;
        auto fname = entry.path().filename();
        // 只取无后缀名的文件
        if (fname.has_extension()) continue;

        std::string filename = PathToUtf8(fname);
        std::string fullPath = PathToUtf8(entry.path());

        // 读取文件时间
        FileTimestamps ts{};
        ReadFileTimestamps(fullPath, ts);

        json f;
        f["filename"] = filename;
        f["fullPath"] = fullPath;
        f["creationTime"] = FormatFileTimeI64(ts.creationTime);
        f["modificationTime"] = FormatFileTimeI64(ts.lastWriteTime);
        f["creationTimeRaw"] = ts.creationTime;
        f["modificationTimeRaw"] = ts.lastWriteTime;
        files.push_back(f);
    }
    return files;
}

void MigrationManager::HandleScanWeGame(const json& msg, const SendCallback& send) {
    json result;
    bool localOnly = msg.value("localOnly", false);
    try {
        std::string dir = GetWeGameLoginPicPath();
        if (dir.empty()) {
            result["success"] = false;
            result["errorMessage"] = "无法获取 APPDATA 路径";
        } else {
            json files = ScanWeGameFiles(dir);
            result["success"] = !files.empty();
            result["files"] = files;
            result["directory"] = dir;
            if (files.empty()) result["errorMessage"] = "login_pic 目录为空或不存在";
        }
    } catch (const std::exception& e) {
        result["success"] = false;
        result["errorMessage"] = e.what();
    }
    result["localOnly"] = localOnly;
    send("perf-scan-wegame-result", result.dump());
}

void MigrationManager::HandleWeGameDownload(const json& msg, const SendCallback& send) {
    json result;
    try {
        std::string serverUrl = msg.value("serverUrl", "");
        std::string token = msg.value("token", "");
        std::string downloadKey = msg.value("downloadKey", "");

        send("perf-progress", json({{"stage", "downloading"}, {"progress", 0.1}}).dump());

        // 下载 ZIP
        std::string tempZip = MakeTempPath(kTempPrefixWgDown, kTempSuffixCab);
        DownloadResult dr = DownloadZip(serverUrl, token, downloadKey, tempZip);
        if (!dr.success) {
            result["success"] = false;
            result["errorMessage"] = "下载失败: " + dr.errorMessage;
            send("perf-wegame-download-result", result.dump());
            return;
        }

        send("perf-progress", json({{"stage", "extracting"}, {"progress", 0.5}}).dump());

        // 解压
        if (!m_extractResult.tempDir.empty()) CleanupTempDir(m_extractResult.tempDir);
        m_extractResult = ExtractZipPackage(tempZip);
        try { fs::remove(tempZip); } catch (...) {}

        if (!m_extractResult.success) {
            result["success"] = false;
            result["errorMessage"] = "解压失败: " + m_extractResult.errorMessage;
            send("perf-wegame-download-result", result.dump());
            return;
        }

        // 解析 metadata 获取原始时间
        m_metadataFiles.clear();
        try {
            json meta = json::parse(m_extractResult.metadataJson);
            for (const auto& f : meta["files"]) {
                MetadataFileEntry mfe;
                mfe.filename = f.value("filename", "");
                auto& ts = f["timestamps"];
                mfe.creationTime = ts.value("creation_time", (int64_t)0);
                mfe.lastAccessTime = ts.value("last_access_time", (int64_t)0);
                mfe.lastWriteTime = ts.value("last_write_time", (int64_t)0);
                mfe.changeTime = ts.value("change_time", (int64_t)0);
                m_metadataFiles.push_back(mfe);
            }
        } catch (...) {}

        // 构建文件列表返回前端
        result["success"] = true;
        result["extractDir"] = m_extractResult.tempDir;
        json fileList = json::array();
        for (const auto& ef : m_extractResult.files) {
            json fj;
            fj["filename"] = ef.filename;
            fj["extractedPath"] = ef.extractedPath;
            // 从 metadata 找到对应时间
            for (const auto& mf : m_metadataFiles) {
                if (mf.filename == ef.filename) {
                    fj["creationTime"] = FormatFileTimeI64(mf.creationTime);
                    fj["modificationTime"] = FormatFileTimeI64(mf.lastWriteTime);
                    fj["creationTimeLocal"] = FormatFileTimeI64Local(mf.creationTime);
                    fj["modificationTimeLocal"] = FormatFileTimeI64Local(mf.lastWriteTime);
                    break;
                }
            }
            fileList.push_back(fj);
        }
        result["files"] = fileList;
    } catch (const std::exception& e) {
        result["success"] = false;
        result["errorMessage"] = e.what();
    }
    send("perf-wegame-download-result", result.dump());
}

void MigrationManager::HandleWeGameApply(const json& msg, const SendCallback& send) {
    json result;
    try {
        std::string targetDir = GetWeGameLoginPicPath();
        if (targetDir.empty()) {
            result["success"] = false;
            result["errorMessage"] = "无法获取本机 login_pic 路径";
            send("perf-wegame-apply-result", result.dump());
            return;
        }

        auto fsTargetDir = Utf8ToPath(targetDir);
        if (!fs::exists(fsTargetDir)) {
            try { fs::create_directories(fsTargetDir); } catch (...) {}
        }

        auto files = msg.value("files", json::array());
        int successCount = 0;
        int tsCount = 0;

        for (size_t i = 0; i < files.size(); i++) {
            const auto& f = files[i];
            std::string filename = f.value("filename", "");
            std::string extractedPath = f.value("extractedPath", "");
            std::string customCT = f.value("customCreationTime", "");
            std::string customMT = f.value("customModificationTime", "");

            if (filename.empty() || extractedPath.empty()) continue;

            auto fsDest = fsTargetDir / Utf8ToPath(filename);

            send("perf-progress", json({{"stage", "processing"}, {"progress",
                0.1 + 0.6 * (double)i / files.size()},
                {"current", i + 1}, {"total", files.size()}}).dump());

            // 复制文件
            try {
                fs::copy_file(Utf8ToPath(extractedPath), fsDest, fs::copy_options::overwrite_existing);
            } catch (...) { continue; }
            successCount++;

            // 设置自定义时间戳
            int64_t ct = ParseDateTimeToFileTime(customCT);
            int64_t mt = ParseDateTimeToFileTime(customMT);
            if (ct == 0 && mt == 0) {
                // 没有自定义时间，尝试从 metadata 获取
                for (const auto& mf : m_metadataFiles) {
                    if (mf.filename == filename) {
                        ct = mf.creationTime;
                        mt = mf.lastWriteTime;
                        break;
                    }
                }
            }

            if (ct != 0 || mt != 0) {
                HANDLE hFile = CreateFileW(fsDest.c_str(), FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    FILE_BASIC_INFO fbi = {};
                    // 先读取现有时间
                    GetFileInformationByHandleEx(hFile, FileBasicInfo, &fbi, sizeof(fbi));
                    if (ct != 0) fbi.CreationTime.QuadPart = ct;
                    if (mt != 0) {
                        fbi.LastWriteTime.QuadPart = mt;
                        fbi.ChangeTime.QuadPart = mt;
                    }
                    SetFileInformationByHandle(hFile, FileBasicInfo, &fbi, sizeof(fbi));
                    CloseHandle(hFile);

                    // 重命名技巧同步 $FILE_NAME 时间
                    std::wstring wDest = fsDest.native();
                    std::wstring wTemp = wDest + L".ts_tmp";
                    if (MoveFileW(wDest.c_str(), wTemp.c_str()))
                        MoveFileW(wTemp.c_str(), wDest.c_str());

                    hFile = CreateFileW(fsDest.c_str(), FILE_WRITE_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (hFile != INVALID_HANDLE_VALUE) {
                        FILE_BASIC_INFO fbi2 = {};
                        GetFileInformationByHandleEx(hFile, FileBasicInfo, &fbi2, sizeof(fbi2));
                        if (ct != 0) fbi2.CreationTime.QuadPart = ct;
                        if (mt != 0) {
                            fbi2.LastWriteTime.QuadPart = mt;
                            fbi2.ChangeTime.QuadPart = mt;
                        }
                        SetFileInformationByHandle(hFile, FileBasicInfo, &fbi2, sizeof(fbi2));
                        CloseHandle(hFile);
                    }
                    tsCount++;
                }
            }
        }

        // 清理临时目录
        if (!m_extractResult.tempDir.empty()) {
            CleanupTempDir(m_extractResult.tempDir);
            m_extractResult.tempDir.clear();
        }

        result["success"] = (successCount > 0);
        result["filesApplied"] = successCount;
        result["timestampsSet"] = tsCount;
        result["totalFiles"] = (int)files.size();
    } catch (const std::exception& e) {
        result["success"] = false;
        result["errorMessage"] = e.what();
    }
    send("perf-wegame-apply-result", result.dump());
}

// ═══════════════════════════════════════════════════════
// 驱动管理
// ═══════════════════════════════════════════════════════

void MigrationManager::HandleDriverStatus(const json& /*msg*/, const SendCallback& send) {
    json result;
    result["installed"] = m_driver.IsDriverInstalled();
    result["loaded"] = m_driver.IsDriverLoaded();
    result["connected"] = m_driver.IsConnected();

    if (m_driver.IsConnected()) {
        ULONG count = 0;
        if (SUCCEEDED(m_driver.GetFakeEntryCount(&count)))
            result["fakeEntryCount"] = (int)count;
    }
    send("perf-driver-status-result", result.dump());
}

void MigrationManager::HandleDriverInstall(const json& msg, const SendCallback& send) {
    json result;
    std::string sysPath = msg.value("sysFilePath", "");
    // sysPath 可以为空 — EnsureDriverReady 会从 DLL 资源中自动提取
    bool ok = m_driver.EnsureDriverReady(sysPath);
    if (ok && !m_driver.IsConnected()) m_driver.Connect();
    result["success"] = ok;
    result["connected"] = m_driver.IsConnected();
    if (!ok) result["errorMessage"] = m_driver.GetLastError();
    send("perf-driver-install-result", result.dump());
}

void MigrationManager::HandleDriverUninstall(const json& /*msg*/, const SendCallback& send) {
    json result;
    m_driver.Disconnect();
    result["success"] = m_driver.RemoveDriver();
    if (!result["success"]) result["errorMessage"] = m_driver.GetLastError();
    send("perf-driver-uninstall-result", result.dump());
}

// ═══════════════════════════════════════════════════════
// CPU 型号修改
// ═══════════════════════════════════════════════════════

// 读取当前 CPU 名称 (ProcessorNameString)
static std::string ReadCurrentCpuName() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS) return "";
    char buf[256] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    RegQueryValueExA(hKey, "ProcessorNameString", nullptr, &type, (BYTE*)buf, &size);
    RegCloseKey(hKey);
    return std::string(buf);
}

// 读取驱动注册表中已保存的伪造 CPU 名称
static std::string ReadSavedFakeCpuName() {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Services\\Fsutil\\Parameters",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS) return "";
    char buf[256] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    LONG ret = RegQueryValueExA(hKey, "CpuName", nullptr, &type, (BYTE*)buf, &size);
    if (ret != ERROR_SUCCESS) { RegCloseKey(hKey); return ""; }

    // 检查过期
    ULONGLONG expiration = 0;
    DWORD qsize = sizeof(expiration);
    ret = RegQueryValueExA(hKey, "CpuNameExpiration", nullptr, &type, (BYTE*)&expiration, &qsize);
    RegCloseKey(hKey);
    if (ret == ERROR_SUCCESS && expiration != 0) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULONGLONG now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        if (now >= expiration) return ""; // 已过期
    }
    return std::string(buf);
}

// 写入所有 CentralProcessor\N 的 ProcessorNameString
static bool WriteCpuNameToProcessor(const std::string& cpuName) {
    HKEY hProc;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DESCRIPTION\\System\\CentralProcessor",
                      0, KEY_READ, &hProc) != ERROR_SUCCESS) return false;
    char subkey[64];
    DWORD idx = 0, len = sizeof(subkey);
    while (RegEnumKeyExA(hProc, idx++, subkey, &len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        HKEY hCore;
        if (RegOpenKeyExA(hProc, subkey, 0, KEY_SET_VALUE, &hCore) == ERROR_SUCCESS) {
            RegSetValueExA(hCore, "ProcessorNameString", 0, REG_SZ,
                           (const BYTE*)cpuName.c_str(), (DWORD)(cpuName.size() + 1));
            RegCloseKey(hCore);
        }
        len = sizeof(subkey);
    }
    RegCloseKey(hProc);
    return true;
}

// 写入 ACPI 枚举中 CPU 设备的 FriendlyName
static bool WriteCpuNameToAcpi(const std::string& cpuName) {
    HKEY hAcpi;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Enum\\ACPI",
                      0, KEY_READ, &hAcpi) != ERROR_SUCCESS) return false;
    char devKey[512];
    DWORD idx = 0, len = sizeof(devKey);
    bool wrote = false;
    while (RegEnumKeyExA(hAcpi, idx++, devKey, &len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        // CPU 设备 ID 包含 GenuineIntel 或 AuthenticAMD
        if (strstr(devKey, "GenuineIntel") || strstr(devKey, "AuthenticAMD") ||
            strstr(devKey, "genuineintel") || strstr(devKey, "authenticamd")) {
            HKEY hDevice;
            if (RegOpenKeyExA(hAcpi, devKey, 0, KEY_READ, &hDevice) == ERROR_SUCCESS) {
                char inst[64];
                DWORD j = 0, ilen = sizeof(inst);
                while (RegEnumKeyExA(hDevice, j++, inst, &ilen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                    HKEY hInst;
                    if (RegOpenKeyExA(hDevice, inst, 0, KEY_SET_VALUE, &hInst) == ERROR_SUCCESS) {
                        RegSetValueExA(hInst, "FriendlyName", 0, REG_SZ,
                                       (const BYTE*)cpuName.c_str(), (DWORD)(cpuName.size() + 1));
                        RegCloseKey(hInst);
                        wrote = true;
                    }
                    ilen = sizeof(inst);
                }
                RegCloseKey(hDevice);
            }
        }
        len = sizeof(devKey);
    }
    RegCloseKey(hAcpi);
    return wrote;
}

// 重命名 ACPI CPU 设备注册表键名，使其匹配伪造的 CPU 名称
// 例: GenuineIntel_-_Intel64_Family_6_Model_183_-_Intel(R)_Core(TM)_i7-14700KF
//   → GenuineIntel_-_Intel64_Family_6_Model_183_-_Intel(R)_Core(TM)_i9-12900K
static bool RenameAcpiCpuKey(const std::string& fakeCpuName) {
    // NtRenameKey 结构 (避免引入 winternl.h 产生冲突)
    struct NT_UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };
    typedef LONG(NTAPI* PFN_NtRenameKey)(HANDLE, NT_UNICODE_STRING*);

    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    auto pNtRenameKey = (PFN_NtRenameKey)GetProcAddress(ntdll, "NtRenameKey");
    if (!pNtRenameKey) return false;

    // UTF-8 → Wide, 空格→下划线
    int wlen = MultiByteToWideChar(CP_UTF8, 0, fakeCpuName.c_str(), -1, nullptr, 0);
    if (wlen <= 1) return false;
    std::wstring wFakeName(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, fakeCpuName.c_str(), -1, &wFakeName[0], wlen);
    for (auto& c : wFakeName) { if (c == L' ') c = L'_'; }

    HKEY hAcpi;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Enum\\ACPI",
                      0, KEY_READ, &hAcpi) != ERROR_SUCCESS)
        return false;

    WCHAR devKey[512];
    DWORD idx = 0, len = ARRAYSIZE(devKey);
    bool renamed = false;

    while (RegEnumKeyExW(hAcpi, idx, devKey, &len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        if (wcsstr(devKey, L"GenuineIntel") || wcsstr(devKey, L"AuthenticAMD")) {
            char dbg[1024];
            WideCharToMultiByte(CP_UTF8, 0, devKey, -1, dbg, sizeof(dbg), nullptr, nullptr);
            OutputDebugStringA(("[FsUtil] ACPI key found: " + std::string(dbg)).c_str());

            // 找最后一个 "_-_" 分隔符
            WCHAR* lastSep = nullptr;
            WCHAR* p = devKey;
            while ((p = wcsstr(p, L"_-_")) != nullptr) { lastSep = p; p += 3; }

            if (lastSep) {
                std::wstring newName(devKey, lastSep + 3);
                newName += wFakeName;

                if (newName != devKey) {
                    char dbgNew[1024];
                    WideCharToMultiByte(CP_UTF8, 0, newName.c_str(), -1, dbgNew, sizeof(dbgNew), nullptr, nullptr);
                    OutputDebugStringA(("[FsUtil] Rename target: " + std::string(dbgNew)).c_str());

                    HKEY hDevice;
                    // KEY_WRITE | DELETE 是 NtRenameKey 所需的最小权限
                    LONG openRet = RegOpenKeyExW(hAcpi, devKey, 0, KEY_WRITE | DELETE,
                                      &hDevice);
                    if (openRet == ERROR_SUCCESS) {
                        NT_UNICODE_STRING uNewName;
                        uNewName.Length = (USHORT)(newName.size() * sizeof(WCHAR));
                        uNewName.MaximumLength = uNewName.Length + sizeof(WCHAR);
                        uNewName.Buffer = const_cast<PWSTR>(newName.c_str());

                        LONG st = pNtRenameKey((HANDLE)hDevice, &uNewName);
                        OutputDebugStringA(("[FsUtil] NtRenameKey status: 0x" + 
                            (std::stringstream() << std::hex << (unsigned long)st).str()).c_str());
                        if (st == 0) renamed = true;
                        RegCloseKey(hDevice);
                        if (st == 0) { idx = 0; len = ARRAYSIZE(devKey); continue; } // 重命名后重新枚举
                    } else {
                        OutputDebugStringA(("[FsUtil] RegOpenKeyEx FAILED: error " + std::to_string(openRet)).c_str());
                    }
                } else {
                    OutputDebugStringA("[FsUtil] Key already matches, skip");
                }
            }
        }
        idx++;
        len = ARRAYSIZE(devKey);
    }

    RegCloseKey(hAcpi);
    return renamed;
}

// 保存伪造 CPU 配置到驱动注册表 (开机持久化)
static bool SaveCpuNameToDriverReg(const std::string& cpuName, int fakeDays) {
    HKEY hKey;
    std::string regPath = "SYSTEM\\CurrentControlSet\\Services\\Fsutil\\Parameters";
    DWORD disp;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, &disp) != ERROR_SUCCESS)
        return false;

    RegSetValueExA(hKey, "CpuName", 0, REG_SZ,
                   (const BYTE*)cpuName.c_str(), (DWORD)(cpuName.size() + 1));

    // 计算过期时间: fakeDays 天后
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    ULONGLONG expiration = now + (ULONGLONG)fakeDays * 24ULL * 3600ULL * 10000000ULL;
    RegSetValueExA(hKey, "CpuNameExpiration", 0, REG_QWORD,
                   (const BYTE*)&expiration, sizeof(expiration));

    RegCloseKey(hKey);
    return true;
}

void MigrationManager::HandleGetCpuInfo(const json& /*msg*/, const SendCallback& send) {
    json result;
    result["currentCpu"] = ReadCurrentCpuName();
    std::string saved = ReadSavedFakeCpuName();
    if (!saved.empty()) result["fakeCpu"] = saved;
    result["success"] = true;
    send("perf-get-cpu-info-result", result.dump());
}

void MigrationManager::HandleSetCpuName(const json& msg, const SendCallback& send) {
    json result;
    std::string cpuName = msg.value("cpuName", "");
    int fakeDays = msg.value("fakeDays", 15);

    if (cpuName.empty()) {
        result["success"] = false;
        result["errorMessage"] = "CPU name is empty";
        send("perf-set-cpu-name-result", result.dump());
        return;
    }

    // 安全校验: 限制长度和字符集（只允许字母、数字、空格、括号、连字符、点、@、下划线）
    if (cpuName.size() > 128) {
        result["success"] = false;
        result["errorMessage"] = "CPU name too long (max 128 chars)";
        send("perf-set-cpu-name-result", result.dump());
        return;
    }
    for (char c : cpuName) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == ' ' || c == '-' || c == '_' || c == '(' || c == ')' || c == '@' ||
              c == '.' || c == '/' || c == '#' || c == ',')) {
            result["success"] = false;
            result["errorMessage"] = "CPU name contains invalid characters. Only alphanumeric, spaces, and ()-.@_#, are allowed.";
            send("perf-set-cpu-name-result", result.dump());
            return;
        }
    }

    bool ok1 = WriteCpuNameToProcessor(cpuName);
    bool ok2 = WriteCpuNameToAcpi(cpuName);
    bool ok3 = SaveCpuNameToDriverReg(cpuName, fakeDays);
    bool ok4 = RenameAcpiCpuKey(cpuName);

    result["success"] = ok1; // ProcessorNameString 是最关键的
    result["processorWrite"] = ok1;
    result["acpiWrite"] = ok2;
    result["driverPersist"] = ok3;
    result["acpiRename"] = ok4;
    if (!ok1) result["errorMessage"] = "Failed to write ProcessorNameString (need admin?)";
    send("perf-set-cpu-name-result", result.dump());
}

// ═══════════════════════════════════════════════════════
// 实时同步 (屏幕共享期间 perf_data 自动上传/下载)
// ═══════════════════════════════════════════════════════

void MigrationManager::HandleRealtimeList(const json& msg, const SendCallback& send) {
    json result;
    try {
        std::string dirPath = msg.value("path", "");
        if (dirPath.empty()) {
            result["success"] = false;
            result["errorMessage"] = "path is required";
            send("perf-realtime-list-result", result.dump());
            return;
        }

        auto fsDir = Utf8ToPath(dirPath);
        if (!fs::exists(fsDir) || !fs::is_directory(fsDir)) {
            result["success"] = false;
            result["errorMessage"] = "Directory does not exist";
            send("perf-realtime-list-result", result.dump());
            return;
        }

        json files = json::array();
        for (const auto& entry : fs::directory_iterator(fsDir)) {
            if (!entry.is_regular_file()) continue;
            std::string fname = PathToUtf8(entry.path().filename());
            if (fname.find("perf_data_") != 0) continue;

            json fj;
            fj["name"] = fname;
            fj["size"] = entry.file_size();
            // 修改时间 → unix millis
            auto ftime = entry.last_write_time();
            auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            fj["mtime"] = sctp.time_since_epoch().count();
            files.push_back(fj);
        }

        result["success"] = true;
        result["files"] = files;
        result["path"] = dirPath;
    } catch (const std::exception& e) {
        result["success"] = false;
        result["errorMessage"] = e.what();
    }
    send("perf-realtime-list-result", result.dump());
}

void MigrationManager::HandleRealtimeUpload(const json& msg, const SendCallback& send) {
    json result;
    try {
        std::string serverUrl = msg.value("serverUrl", "");
        std::string filePath = msg.value("filePath", "");

        if (serverUrl.empty() || filePath.empty()) {
            result["success"] = false;
            result["errorMessage"] = "serverUrl and filePath are required";
            send("perf-realtime-upload-result", result.dump());
            return;
        }

        auto fsPath = Utf8ToPath(filePath);
        if (!fs::exists(fsPath)) {
            result["success"] = false;
            result["errorMessage"] = "File not found: " + filePath;
            send("perf-realtime-upload-result", result.dump());
            return;
        }

        std::string filename = PathToUtf8(fsPath.filename());

        // 使用 UploadSingleFile 直接上传原始文件 (不打 ZIP)
        UploadResult ur = UploadSingleFile(serverUrl, filePath);

        result["success"] = ur.success;
        result["token"] = ur.token;
        result["downloadKey"] = ur.downloadKey;
        result["downloadUrl"] = ur.downloadUrl;
        result["filename"] = filename;
        result["errorMessage"] = ur.errorMessage;
    } catch (const std::exception& e) {
        result["success"] = false;
        result["errorMessage"] = e.what();
    }
    send("perf-realtime-upload-result", result.dump());
}

void MigrationManager::HandleRealtimeReceive(const json& msg, const SendCallback& send) {
    json result;
    try {
        std::string serverUrl = msg.value("serverUrl", "");
        std::string token = msg.value("token", "");
        std::string downloadKey = msg.value("downloadKey", "");
        std::string filename = msg.value("filename", "");
        std::string targetDir = msg.value("targetDir", "");

        if (serverUrl.empty() || token.empty() || targetDir.empty()) {
            result["success"] = false;
            result["errorMessage"] = "serverUrl, token, and targetDir are required";
            send("perf-realtime-receive-result", result.dump());
            return;
        }

        // 1. 下载文件到内存
        auto dlResult = DownloadToMemory(serverUrl, token, downloadKey);
        if (!dlResult.success) {
            result["success"] = false;
            result["errorMessage"] = "Download failed: " + dlResult.errorMessage;
            send("perf-realtime-receive-result", result.dump());
            return;
        }

        // 2. 确保目标目录存在
        auto fsTargetDir = Utf8ToPath(targetDir);
        if (!fs::exists(fsTargetDir)) {
            try { fs::create_directories(fsTargetDir); } catch (...) {}
        }

        // 3. 写入临时文件用于 protobuf 解析
        if (filename.empty()) filename = "perf_data_received";
        auto fsDest = fsTargetDir / Utf8ToPath(filename);
        std::string destPath = PathToUtf8(fsDest);

        {
            std::ofstream outFile(fsDest, std::ios::binary);
            if (!outFile.is_open()) {
                result["success"] = false;
                result["errorMessage"] = "Cannot write to target: " + destPath;
                send("perf-realtime-receive-result", result.dump());
                return;
            }
            outFile.write(reinterpret_cast<const char*>(dlResult.data.data()), dlResult.data.size());
        }

        // 4. 检测本机硬件 (使用缓存或重新检测)
        // 读取前端传来的硬件信息, 如果有的话优先使用
        KnownFields modifiedFields;
        if (msg.contains("hwInfo") && msg["hwInfo"].is_object()) {
            const auto& hw = msg["hwInfo"];
            modifiedFields.gpuVendor = hw.value("gpuVendor", "");
            modifiedFields.gpuModel = hw.value("gpuModel", "");
            modifiedFields.driverVersion = hw.value("driverVersion", "");
            modifiedFields.cpuModel = hw.value("cpuModel", "");
            modifiedFields.macAddress = hw.value("macAddress", "");
            modifiedFields.deviceId = hw.value("deviceId", "");
            modifiedFields.hardwareId1 = hw.value("hardwareId1", "");
            modifiedFields.hardwareId2 = hw.value("hardwareId2", "");
            modifiedFields.osVersion = hw.value("osVersion", "");
        } else {
            // 使用缓存的硬件信息, 如果为空则重新检测
            if (m_hwInfo.gpuModel.empty()) {
                m_hwInfo = DetectLocalHardware();
            }
            modifiedFields.gpuVendor = m_hwInfo.gpuVendor;
            modifiedFields.gpuModel = m_hwInfo.gpuModel;
            modifiedFields.driverVersion = m_hwInfo.driverVersion;
            modifiedFields.cpuModel = m_hwInfo.cpuModel;
            modifiedFields.macAddress = m_hwInfo.macAddress;
            modifiedFields.deviceId = m_hwInfo.deviceId;
            modifiedFields.hardwareId1 = m_hwInfo.hardwareId1;
            modifiedFields.hardwareId2 = m_hwInfo.hardwareId2;
            modifiedFields.osVersion = m_hwInfo.osVersion;
        }
        modifiedFields.osName = "Windows";
        modifiedFields.architecture = "x64";

        // 5. 修改 protobuf header
        bool headerModified = false;
        try {
            PerfDataParser parser(destPath);
            if (parser.Load() && parser.ParseHeader()) {
                auto newHeader = parser.ReserializeHeader(modifiedFields);
                const auto& compressedData = parser.GetCompressedData();

                std::vector<uint8_t> newFile;
                newFile.insert(newFile.end(), newHeader.begin(), newHeader.end());
                newFile.insert(newFile.end(), compressedData.begin(), compressedData.end());

                std::ofstream outFile(fsDest, std::ios::binary);
                outFile.write(reinterpret_cast<const char*>(newFile.data()), newFile.size());
                outFile.close();
                headerModified = true;
            }
        } catch (...) {
            // protobuf 修改失败, 文件仍然保留 (原始格式)
        }

        result["success"] = true;
        result["filename"] = filename;
        result["destPath"] = destPath;
        result["headerModified"] = headerModified;
        result["fileSize"] = (uint64_t)dlResult.data.size();
    } catch (const std::exception& e) {
        result["success"] = false;
        result["errorMessage"] = e.what();
    }
    send("perf-realtime-receive-result", result.dump());
}

} // namespace PerfMigration
