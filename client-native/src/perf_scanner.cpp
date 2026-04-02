/**
 * perf_scanner.cpp — 文件扫描 + 时间戳读取
 * 合并移植自 sender/FileScanner.cpp + sender/TimestampReader.cpp
 */

#include "perf_scanner.h"

#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace PerfMigration {

namespace fs = std::filesystem;

// ── 时间戳工具 ──

static uint64_t ParseTimestampFromFilename(const std::string& filename) {
    if (filename.size() < 11 || filename.find("perf_data_") != 0) return 0;
    size_t start = 10;
    size_t end = filename.find('_', start);
    if (end == std::string::npos) end = filename.size();
    try { return std::stoull(filename.substr(start, end - start)); }
    catch (...) { return 0; }
}

static uint64_t FileTimeToUnixTimestamp(const FILETIME& ft) {
    ULARGE_INTEGER ull;
    ull.LowPart = ft.dwLowDateTime;
    ull.HighPart = ft.dwHighDateTime;
    return (ull.QuadPart / 10000000ULL) - 11644473600ULL;
}

static void DetectTimeAnomaly(PerfFileMetadata& meta) {
    uint64_t tsFilename = meta.filenameTimestamp;
    uint64_t tsCreation = FileTimeToUnixTimestamp(meta.creationTimeRaw);
    uint64_t tsModification = FileTimeToUnixTimestamp(meta.modificationTimeRaw);
    const uint64_t threshold = 3;
    auto absDiff = [](uint64_t a, uint64_t b) -> uint64_t { return (a > b) ? (a - b) : (b - a); };
    int anomalyCount = 0;
    if (absDiff(tsFilename, tsCreation) > threshold) anomalyCount++;
    if (absDiff(tsFilename, tsModification) > threshold) anomalyCount++;
    if (absDiff(tsCreation, tsModification) > threshold) anomalyCount++;
    meta.isTimeAnomaly = (anomalyCount > 0);
    meta.creationTimeAnomaly = (absDiff(tsFilename, tsCreation) > threshold);
    meta.modificationTimeAnomaly = (absDiff(tsFilename, tsModification) > threshold);
}

// ── FileScanner ──

std::string FileScanner::ReadInstallPathFromRegistry() {
    HKEY hKey;
    const char* subKey = "SOFTWARE\\Rail\\Dfmclient-Win64-Test";
    LONG result = RegOpenKeyExA(HKEY_CURRENT_USER, subKey, 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) return "";

    char value[512] = {0};
    DWORD valueSize = sizeof(value);
    DWORD type;
    result = RegQueryValueExA(hKey, "InstallPath", nullptr, &type, (LPBYTE)value, &valueSize);
    RegCloseKey(hKey);

    if (result == ERROR_SUCCESS && type == REG_SZ) return std::string(value);
    return "";
}

std::string FileScanner::BuildPerformancePath(const std::string& installPath) {
    if (installPath.empty()) return "";
    std::string perfPath = installPath;
    if (perfPath.back() != '\\' && perfPath.back() != '/') perfPath += "\\";
    perfPath += "DeltaForce\\Binaries\\Win64\\.quality\\performance";
    return perfPath;
}

ScanResult FileScanner::ScanDirectory(const std::string& dirPath) {
    ScanResult result;
    result.targetPath = dirPath;
    try {
        auto fsPath = Utf8ToPath(dirPath);
        if (!fs::exists(fsPath) || !fs::is_directory(fsPath)) {
            result.status = ScanStatus::Failed;
            result.errorMessage = "Directory does not exist";
            return result;
        }
        for (const auto& entry : fs::directory_iterator(fsPath)) {
            if (!entry.is_regular_file()) continue;
            std::string filename = PathToUtf8(entry.path().filename());
            if (!IsPerfDataFile(filename)) continue;

            std::wstring wFullPath = entry.path().native();
            WIN32_FILE_ATTRIBUTE_DATA fileInfo;
            if (GetFileAttributesExW(wFullPath.c_str(), GetFileExInfoStandard, &fileInfo)) {
                PerfFileMetadata meta;
                meta.filename = filename;
                meta.fullPath = PathToUtf8(entry.path());
                meta.fileSize = ((uint64_t)fileInfo.nFileSizeHigh << 32) | fileInfo.nFileSizeLow;
                meta.creationTimeRaw = fileInfo.ftCreationTime;
                meta.modificationTimeRaw = fileInfo.ftLastWriteTime;
                meta.accessTimeRaw = fileInfo.ftLastAccessTime;
                meta.creationTime = FileTimeToString(fileInfo.ftCreationTime);
                meta.modificationTime = FileTimeToString(fileInfo.ftLastWriteTime);
                meta.accessTime = FileTimeToString(fileInfo.ftLastAccessTime);
                meta.filenameTimestamp = ParseTimestampFromFilename(filename);
                DetectTimeAnomaly(meta);
                result.files.push_back(meta);
            }
        }
        std::sort(result.files.begin(), result.files.end(),
            [](const PerfFileMetadata& a, const PerfFileMetadata& b) {
                if (a.isTimeAnomaly != b.isTimeAnomaly) return a.isTimeAnomaly > b.isTimeAnomaly;
                return CompareFileTime(&b.modificationTimeRaw, &a.modificationTimeRaw) < 0;
            });
        result.status = ScanStatus::Completed;
    } catch (const std::exception& e) {
        result.status = ScanStatus::Failed;
        result.errorMessage = std::string("Scan error: ") + e.what();
    }
    return result;
}

std::vector<std::string> FileScanner::RecursiveScanDrives(int maxDepth) {
    std::vector<std::string> results;
    auto drives = GetLogicalDrivesList();
    auto startTime = std::chrono::steady_clock::now();
    bool shouldStop = false;
    for (const auto& drive : drives) {
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > 30) break;
        RecursiveSearch(drive, 0, maxDepth, results, shouldStop);
        if (shouldStop) break;
    }
    return results;
}

void FileScanner::RecursiveSearch(const std::string& currentPath, int currentDepth, int maxDepth,
                                   std::vector<std::string>& results, bool& shouldStop) {
    if (currentDepth > maxDepth || shouldStop) return;
    try {
        if (IsTargetPath(currentPath)) {
            results.push_back(currentPath);
            if (results.size() >= 10) { shouldStop = true; return; }
        }
        auto fsPath = Utf8ToPath(currentPath);
        for (const auto& entry : fs::directory_iterator(fsPath)) {
            if (!entry.is_directory()) continue;
            std::string dirname = PathToUtf8(entry.path().filename());
            if (dirname.empty() || dirname[0] == '.' || dirname[0] == '$') continue;
            if (dirname == "Windows" || dirname == "Program Files" ||
                dirname == "ProgramData" || dirname == "System32" ||
                dirname == "Program Files (x86)") continue;
            RecursiveSearch(PathToUtf8(entry.path()), currentDepth + 1, maxDepth, results, shouldStop);
            if (shouldStop) break;
        }
    } catch (...) {}
}

ScanResult FileScanner::AutoScan() {
    ScanResult result;
    std::string regPath = ReadInstallPathFromRegistry();
    if (!regPath.empty()) {
        std::string perfPath = BuildPerformancePath(regPath);
        auto fsPath = Utf8ToPath(perfPath);
        if (fs::exists(fsPath) && fs::is_directory(fsPath)) {
            result = ScanDirectory(perfPath);
            if (result.status == ScanStatus::Completed) return result;
        }
    }
    result.status = ScanStatus::NotStarted;
    result.errorMessage = "Registry path not found or invalid";
    return result;
}

std::string FileScanner::FileTimeToString(const FILETIME& ft) {
    SYSTEMTIME stUTC, stLocal;
    FileTimeToSystemTime(&ft, &stUTC);
    SystemTimeToTzSpecificLocalTime(nullptr, &stUTC, &stLocal);
    char buffer[64];
    sprintf_s(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
              stLocal.wYear, stLocal.wMonth, stLocal.wDay,
              stLocal.wHour, stLocal.wMinute, stLocal.wSecond);
    return std::string(buffer);
}

bool FileScanner::IsTargetPath(const std::string& path) {
    std::string pathLower = path;
    std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::tolower);
    return pathLower.find("win64\\.quality\\performance") != std::string::npos ||
           pathLower.find("win64/.quality/performance") != std::string::npos;
}

std::vector<std::string> FileScanner::GetLogicalDrivesList() {
    std::vector<std::string> drives;
    DWORD driveMask = ::GetLogicalDrives();
    for (char drive = 'C'; drive <= 'Z'; drive++) {
        if (driveMask & (1 << (drive - 'A'))) {
            std::string drivePath = std::string(1, drive) + ":\\";
            if (GetDriveTypeA(drivePath.c_str()) == DRIVE_FIXED) drives.push_back(drivePath);
        }
    }
    return drives;
}

bool FileScanner::IsPerfDataFile(const std::string& filename) {
    return filename.find("perf_data_") == 0;
}

// ── TimestampReader ──

bool ReadFileTimestamps(const std::string& filePath, FileTimestamps& ts) {
    fs::path fp = Utf8ToPath(filePath);

    HANDLE h = CreateFileW(fp.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE) return false;

    struct {
        LARGE_INTEGER CreationTime;
        LARGE_INTEGER LastAccessTime;
        LARGE_INTEGER LastWriteTime;
        LARGE_INTEGER ChangeTime;
        DWORD FileAttributes;
    } info;

    BOOL ok = GetFileInformationByHandleEx(h, FileBasicInfo, &info, sizeof(info));
    CloseHandle(h);

    if (!ok) return false;

    ts.creationTime = info.CreationTime.QuadPart;
    ts.lastAccessTime = info.LastAccessTime.QuadPart;
    ts.lastWriteTime = info.LastWriteTime.QuadPart;
    ts.changeTime = info.ChangeTime.QuadPart;
    return true;
}

std::string FileTimeInt64ToString(int64_t ft) {
    FILETIME fileTime;
    ULARGE_INTEGER ull;
    ull.QuadPart = static_cast<uint64_t>(ft);
    fileTime.dwLowDateTime = ull.LowPart;
    fileTime.dwHighDateTime = ull.HighPart;

    SYSTEMTIME stUTC, stLocal;
    FileTimeToSystemTime(&fileTime, &stUTC);
    SystemTimeToTzSpecificLocalTime(nullptr, &stUTC, &stLocal);

    char buffer[64];
    sprintf_s(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
              stLocal.wYear, stLocal.wMonth, stLocal.wDay,
              stLocal.wHour, stLocal.wMinute, stLocal.wSecond);
    return std::string(buffer);
}

} // namespace PerfMigration
