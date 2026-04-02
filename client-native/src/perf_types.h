#pragma once
/**
 * perf_types.h — 共享类型定义 (从日志迁移项目移植)
 * Sender/Receiver 公用的数据结构
 */

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <windows.h>
#include <filesystem>

namespace PerfMigration {

// ===== UTF-8 路径助手 =====
// MSVC 的 fs::path(string) 按 ACP 解码, 中文路径会乱码
// 必须先转 wstring 再构造 path
inline std::filesystem::path Utf8ToPath(const std::string& utf8) {
    if (utf8.empty()) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return std::filesystem::path(utf8); // fallback
    std::wstring ws(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, ws.data(), wlen);
    return std::filesystem::path(ws);
}

// path → UTF-8 string
inline std::string PathToUtf8(const std::filesystem::path& p) {
    const std::wstring& ws = p.native();
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return p.string(); // fallback
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

// ===== Protobuf 字段类型 =====

enum class FieldType { Varint, String, Bytes, Fixed32, Fixed64 };

struct RawField {
    int fieldNumber = 0;
    FieldType type = FieldType::Varint;
    std::string value;
    uint64_t numericValue = 0;
};

// ===== 已知字段 (protobuf header 中的硬件/系统信息) =====

struct KnownFields {
    std::string deviceId;
    std::string sessionId;
    std::string gameVersion;
    std::string osName;
    std::string osVersion;
    std::string architecture;
    std::string gpuVendor;
    std::string gpuModel;
    std::string driverVersion;
    std::string hardwareId1;
    std::string hardwareId2;
    std::string macAddress;
    std::string cpuModel;
    std::string compression;
};

// ===== 解析结果结构 =====

struct CoordinateData {
    std::string timestamp;
    int frame = 0;
    std::string address;
    float x = 0, y = 0, z = 0;
    std::string duration;
};

struct StringCategories {
    std::vector<std::string> gameInfo, engineInfo, hardwareInfo, pathInfo;
    std::vector<std::string> configParams, loadingFlow, performanceMetrics;
    std::vector<std::string> uuids, timestamps, other;
};

struct FileInfo {
    std::string filename;
    uint64_t timestampRaw = 0;
    std::string identifier;
};

struct HeaderData {
    size_t headerSize = 0;
    std::vector<RawField> rawFields;
    KnownFields knownFields;
    bool hasOuterWrapper = false;
    int outerFieldNumber = 0;
};

struct ContentData {
    size_t size = 0;
    std::vector<std::string> strings;
    std::map<std::string, std::string> keyValues;
    std::vector<CoordinateData> coordinates;
    StringCategories categories;
};

struct ParseResult {
    FileInfo fileInfo;
    size_t fileSize = 0;
    HeaderData header;
    ContentData content;
    bool success = false;
    std::string errorMessage;
};

// ===== 文件扫描相关 (Sender) =====

struct FileTimestamps {
    int64_t creationTime = 0;
    int64_t lastAccessTime = 0;
    int64_t lastWriteTime = 0;
    int64_t changeTime = 0;
};

struct PerfFileMetadata {
    std::string filename;
    std::string fullPath;
    uint64_t fileSize = 0;
    std::string creationTime;
    std::string modificationTime;
    std::string accessTime;
    FILETIME creationTimeRaw{};
    FILETIME modificationTimeRaw{};
    FILETIME accessTimeRaw{};
    uint64_t filenameTimestamp = 0;
    bool isTimeAnomaly = false;
    bool creationTimeAnomaly = false;
    bool modificationTimeAnomaly = false;
};

enum class ScanStatus { NotStarted, InProgress, Completed, Failed };

struct ScanResult {
    ScanStatus status = ScanStatus::NotStarted;
    std::string targetPath;
    std::vector<PerfFileMetadata> files;
    std::string errorMessage;
    bool isFromFallbackScan = false;
    std::vector<std::string> candidatePaths;
    int selectedCandidateIndex = -1;
};

// ===== ZIP 打包/解压 =====

struct PackEntry {
    std::string fullPath;
    std::string filename;
    FileTimestamps timestamps;
};

struct ExtractedFile {
    std::string filename;
    std::string extractedPath;
};

struct ExtractResult {
    bool success = false;
    std::string metadataJson;
    std::vector<ExtractedFile> files;
    std::string tempDir;
    std::string errorMessage;
};

// ===== HTTP 结果 =====

struct UploadResult {
    bool success = false;
    std::string token;
    std::string downloadUrl;
    std::string downloadKey;
    std::string errorMessage;
};

struct DownloadResult {
    bool success = false;
    std::string localPath;
    std::string errorMessage;
};

// ===== 硬件检测 =====

struct HardwareInfo {
    std::string gpuVendor;
    std::string gpuModel;
    std::string driverVersion;
    std::string cpuModel;
    std::string macAddress;
    std::string deviceId;
    std::string hardwareId1;
    std::string hardwareId2;
    std::string osName;
    std::string osVersion;
    std::string architecture;
};

// ===== Metadata 文件条目 (JSON) =====

struct MetadataFileEntry {
    std::string filename;
    int64_t creationTime = 0;
    int64_t lastAccessTime = 0;
    int64_t lastWriteTime = 0;
    int64_t changeTime = 0;
};

} // namespace PerfMigration
