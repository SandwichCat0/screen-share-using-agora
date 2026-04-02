#pragma once
/**
 * perf_scanner.h — 文件扫描 + 时间戳读取
 * 合并 FileScanner + TimestampReader
 */

#include "perf_types.h"
#include <string>
#include <vector>

namespace PerfMigration {

class FileScanner {
public:
    static std::string ReadInstallPathFromRegistry();
    static std::string BuildPerformancePath(const std::string& installPath);
    static ScanResult ScanDirectory(const std::string& dirPath);
    static std::vector<std::string> RecursiveScanDrives(int maxDepth = 9);
    static ScanResult AutoScan();

private:
    static std::string FileTimeToString(const FILETIME& ft);
    static void RecursiveSearch(const std::string& currentPath, int currentDepth,
                                 int maxDepth, std::vector<std::string>& results, bool& shouldStop);
    static bool IsTargetPath(const std::string& path);
    static std::vector<std::string> GetLogicalDrivesList();
    static bool IsPerfDataFile(const std::string& filename);
};

// 读取文件的 4 个时间戳 (FileBasicInfo)
bool ReadFileTimestamps(const std::string& filePath, FileTimestamps& ts);

// FILETIME int64 → 可读字符串
std::string FileTimeInt64ToString(int64_t ft);

} // namespace PerfMigration
