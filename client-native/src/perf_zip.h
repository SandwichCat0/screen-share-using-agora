#pragma once
/**
 * perf_zip.h — ZIP 打包/解压 (使用 miniz)
 * 替代原项目的 minizip 依赖
 */

#include "perf_types.h"
#include <string>
#include <vector>

namespace PerfMigration {

// ── 混淆临时文件名（仿 Windows 系统诊断文件）──
// 前缀常量——清理函数据此识别残留
inline const char* kTempPrefixUpload   = "DiagTrack_";   // 上传打包 ZIP
inline const char* kTempPrefixDownload = "CompatTel_";   // 下载 ZIP
inline const char* kTempPrefixExtract  = "WPR_";         // 解压目录
inline const char* kTempPrefixWgDown   = "AppDiag_";     // WeGame 下载 ZIP
inline const char* kTempSuffixCab      = ".cab";         // ZIP 伪装后缀
inline const char* kTempSuffixRename   = ".etw";         // 重命名临时后缀

// 生成混淆临时路径  (返回 %TEMP%\<prefix><hex><suffix>)
std::string MakeTempPath(const char* prefix, const char* suffix);

// 生成混淆临时目录路径
std::string MakeTempDir(const char* prefix);

// 创建 ZIP: metadata.json + perf_data files
bool CreateZipPackage(const std::string& outputPath, const std::vector<PackEntry>& entries);

// 解压 ZIP 到临时目录
ExtractResult ExtractZipPackage(const std::string& zipPath);

// 清理临时目录
void CleanupTempDir(const std::string& tempDir);

// 启动时清理上次残留的临时文件/目录
void CleanupResidualTempFiles();

} // namespace PerfMigration
