/**
 * perf_zip.cpp — ZIP 打包/解压 (miniz 替代 minizip)
 * 使用 miniz 的 mz_zip_writer/mz_zip_reader API
 * 所有文件路径均通过 Utf8ToPath 转换，以支持中文路径
 */

#include "perf_zip.h"
#include "perf_types.h"

#include <windows.h>
#include <bcrypt.h>
#include <miniz.h>
#include <nlohmann/json.hpp>

#pragma comment(lib, "bcrypt.lib")

#include <fstream>
#include <filesystem>
#include <ctime>
#include <cstring>
#include <vector>
#include <random>
#include <sstream>
#include <iomanip>

namespace PerfMigration {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── 生成加密安全的随机十六进制串 (128-bit) ──
static std::string RandomHex32() {
    UCHAR randBytes[16]{};
    // 使用 BCryptGenRandom 提供密码学安全的随机性
    if (BCRYPT_SUCCESS(BCryptGenRandom(nullptr, randBytes, sizeof(randBytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        std::ostringstream oss;
        for (int i = 0; i < 16; ++i)
            oss << std::hex << std::setfill('0') << std::setw(2) << (int)randBytes[i];
        return oss.str();
    }
    // 回退: 使用 std::random_device
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << dist(gen)
        << std::setw(16) << dist(gen);
    return oss.str();
}

std::string MakeTempPath(const char* prefix, const char* suffix) {
    return PathToUtf8(fs::temp_directory_path()) +
           "\\" + prefix + RandomHex32() + suffix;
}

std::string MakeTempDir(const char* prefix) {
    return PathToUtf8(fs::temp_directory_path()) +
           "\\" + prefix + RandomHex32();
}

// ── 辅助：读取文件到 vector（支持中文路径）──
static bool ReadFileToVec(const std::string& utf8Path, std::vector<char>& out) {
    std::ifstream f(Utf8ToPath(utf8Path), std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    f.read(out.data(), sz);
    return true;
}

// ── 辅助：将内存写入文件（支持中文路径）──
static bool WriteVecToFile(const std::string& utf8Path, const void* data, size_t size) {
    std::ofstream f(Utf8ToPath(utf8Path), std::ios::binary);
    if (!f.is_open()) return false;
    f.write(static_cast<const char*>(data), size);
    return f.good();
}

bool CreateZipPackage(const std::string& outputPath, const std::vector<PackEntry>& entries) {
    // 使用堆内存模式，避免 miniz 内部 fopen 不支持 UTF-8 路径
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        return false;
    }

    // metadata.json
    json metadata;
    metadata["version"] = 1;
    json fileList = json::array();
    for (const auto& entry : entries) {
        json fileEntry;
        fileEntry["filename"] = entry.filename;
        fileEntry["timestamps"]["creation_time"] = entry.timestamps.creationTime;
        fileEntry["timestamps"]["last_access_time"] = entry.timestamps.lastAccessTime;
        fileEntry["timestamps"]["last_write_time"] = entry.timestamps.lastWriteTime;
        fileEntry["timestamps"]["change_time"] = entry.timestamps.changeTime;
        fileList.push_back(fileEntry);
    }
    metadata["files"] = fileList;

    std::string metaStr = metadata.dump(2);
    if (!mz_zip_writer_add_mem(&zip, "metadata.json",
                                metaStr.data(), metaStr.size(),
                                MZ_DEFAULT_COMPRESSION)) {
        mz_zip_writer_end(&zip);
        return false;
    }

    // 添加每个 perf_data 文件（先读入内存，再添加到 ZIP）
    for (const auto& entry : entries) {
        std::vector<char> fileData;
        if (!ReadFileToVec(entry.fullPath, fileData)) {
            mz_zip_writer_end(&zip);
            return false;
        }
        if (!mz_zip_writer_add_mem(&zip, entry.filename.c_str(),
                                    fileData.data(), fileData.size(),
                                    MZ_DEFAULT_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            return false;
        }
    }

    // 最终化并导出堆数据
    void* archiveData = nullptr;
    size_t archiveSize = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &archiveData, &archiveSize)) {
        mz_zip_writer_end(&zip);
        return false;
    }
    mz_zip_writer_end(&zip);

    if (!archiveData) return false;
    bool ok = WriteVecToFile(outputPath, archiveData, archiveSize);
    mz_free(archiveData);
    return ok;
}

ExtractResult ExtractZipPackage(const std::string& zipPath) {
    ExtractResult result;
    result.success = false;

    // 先把 ZIP 整体读入内存，避免 miniz 内部 fopen 不支持 UTF-8
    std::vector<char> zipData;
    if (!ReadFileToVec(zipPath, zipData)) {
        result.errorMessage = "Failed to read ZIP: " + zipPath;
        return result;
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0)) {
        result.errorMessage = "Failed to parse ZIP: " + zipPath;
        return result;
    }

    // 创建临时目录（混淆为 Windows 系统诊断目录名）
    std::string tempDir = MakeTempDir(kTempPrefixExtract);
    fs::create_directories(Utf8ToPath(tempDir));
    result.tempDir = tempDir;

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);

    for (mz_uint i = 0; i < numFiles; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        std::string name = stat.m_filename;

        if (name == "metadata.json") {
            // 读取 metadata 到内存
            size_t size = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
            if (data) {
                result.metadataJson = std::string(static_cast<char*>(data), size);
                mz_free(data);
            }
        } else {
            // 解压到临时目录（先提取到堆，再写入文件）
            std::string outPath = tempDir + "\\" + name;
            size_t size = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
            if (data) {
                if (WriteVecToFile(outPath, data, size)) {
                    ExtractedFile ef;
                    ef.filename = name;
                    ef.extractedPath = outPath;
                    result.files.push_back(ef);
                }
                mz_free(data);
            }
        }
    }

    mz_zip_reader_end(&zip);

    if (result.metadataJson.empty()) {
        result.errorMessage = "metadata.json not found in ZIP";
        return result;
    }

    result.success = true;
    return result;
}

void CleanupTempDir(const std::string& tempDir) {
    try {
        fs::remove_all(Utf8ToPath(tempDir));
    } catch (...) {}
}

void CleanupResidualTempFiles() {
    try {
        fs::path tempRoot = fs::temp_directory_path();
        // 需要清理的前缀列表
        const char* prefixes[] = {
            kTempPrefixUpload,    // DiagTrack_
            kTempPrefixDownload,  // CompatTel_
            kTempPrefixExtract,   // WPR_
            kTempPrefixWgDown,    // AppDiag_
        };
        for (auto& entry : fs::directory_iterator(tempRoot, fs::directory_options::skip_permission_denied)) {
            std::string name = PathToUtf8(entry.path().filename());
            for (const char* prefix : prefixes) {
                if (name.rfind(prefix, 0) == 0) {
                    // 匹配到残留文件/目录，删除
                    try { fs::remove_all(entry.path()); } catch (...) {}
                    break;
                }
            }
        }
    } catch (...) {}
}

} // namespace PerfMigration
