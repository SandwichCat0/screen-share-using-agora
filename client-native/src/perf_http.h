#pragma once
/**
 * perf_http.h — HTTP 上传/下载 (WinHTTP)
 * 替代原项目的 cpp-httplib 依赖
 * 仅保留文件传输功能, 不含 LoginFlow
 */

#include "perf_types.h"
#include <string>

namespace PerfMigration {

// 上传 ZIP 到服务器 (multipart POST → /api/upload)
UploadResult UploadZip(const std::string& serverUrl, const std::string& zipPath);

// 上传单个文件到服务器 (multipart POST → /api/upload)
UploadResult UploadSingleFile(const std::string& serverUrl, const std::string& filePath);

// 下载 ZIP (GET → /api/download/<token>?key=<key>)
DownloadResult DownloadZip(const std::string& serverUrl,
                           const std::string& token,
                           const std::string& downloadKey,
                           const std::string& savePath);

// 下载文件到内存 (GET → /api/download/<token>?key=<key>)
struct DownloadMemoryResult {
    bool success = false;
    std::vector<uint8_t> data;
    std::string errorMessage;
};
DownloadMemoryResult DownloadToMemory(const std::string& serverUrl,
                                       const std::string& token,
                                       const std::string& downloadKey);

} // namespace PerfMigration
