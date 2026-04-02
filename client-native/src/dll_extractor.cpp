/**
 * dll_extractor.cpp — 运行时从嵌入 ZIP 资源解压 Agora DLLs
 */
#include "dll_extractor.h"
#include "resource.h"

#include <windows.h>
#include <bcrypt.h>
#include <miniz.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

extern HINSTANCE g_hInstance;

namespace DllExtractor {

static bool s_extracted = false;
static fs::path s_extractDir;

bool EnsureExtracted()
{
    if (s_extracted) return true;

    // ── 1. 确定解压目录 %TEMP%\MDShareNative_<random>\ 以避免可预测路径 ──
    wchar_t tempBuf[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempBuf);

    // 生成加密安全的随机后缀
    UCHAR randBytes[16]{};
    BCryptGenRandom(nullptr, randBytes, sizeof(randBytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::ostringstream hexStream;
    for (int i = 0; i < 16; ++i)
        hexStream << std::hex << std::setfill('0') << std::setw(2) << (int)randBytes[i];
    std::string randSuffix = hexStream.str();
    std::wstring wSuffix(randSuffix.begin(), randSuffix.end());

    s_extractDir = fs::path(tempBuf) / (L"MDShareNative_" + wSuffix);
    std::error_code ec;
    fs::create_directories(s_extractDir, ec);

    // ── 2. 从 RC 资源加载 ZIP ──
    HRSRC hRes = FindResourceW(g_hInstance, MAKEINTRESOURCEW(IDR_AGORA_DEPS_ZIP), RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hMem = LoadResource(g_hInstance, hRes);
    if (!hMem) return false;
    const void* zipData = LockResource(hMem);
    DWORD zipSize = SizeofResource(g_hInstance, hRes);
    if (!zipData || zipSize == 0) return false;

    // ── 3. 用 miniz 遍历并解压每个 DLL ──
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData, zipSize, 0))
        return false;

    int numFiles = static_cast<int>(mz_zip_reader_get_num_files(&zip));
    for (int i = 0; i < numFiles; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;

        // 只取文件名（去掉 ZIP 内路径）
        fs::path outPath = s_extractDir / fs::path(stat.m_filename).filename();

        // 跳过已存在且大小相同的文件（避免重复写入 / 文件锁冲突）
        if (fs::exists(outPath, ec) &&
            fs::file_size(outPath, ec) == stat.m_uncomp_size)
            continue;

        // 解压到堆，再用 wide-char 路径写入（兼容非 ASCII 用户名）
        size_t uncompSize = 0;
        void* buf = mz_zip_reader_extract_to_heap(&zip, i, &uncompSize, 0);
        if (!buf) continue;

        std::ofstream ofs(outPath, std::ios::binary);
        if (ofs) ofs.write(static_cast<const char*>(buf), uncompSize);
        ofs.close();
        mz_free(buf);
    }
    mz_zip_reader_end(&zip);

    // ── 4. 设置 DLL 搜索目录 ──
    SetDllDirectoryW(s_extractDir.c_str());

    s_extracted = true;
    return true;
}

void Cleanup()
{
    // 1. 清理当前会话的解压目录
    if (s_extracted && !s_extractDir.empty()) {
        SetDllDirectoryW(nullptr);
        std::error_code ec;
        fs::remove_all(s_extractDir, ec);
        s_extracted = false;
    }

    // 2. 无条件生成 VBS 脚本延迟清理所有 MDShareNative_* 残留
    wchar_t tempBuf[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempBuf);
    std::wstring tempDir = tempBuf;
    std::wstring vbsPath = tempDir + L"_mdshare_cleanup.vbs";

    std::ofstream vbs(vbsPath, std::ios::trunc);
    if (vbs.is_open()) {
        std::string tempDirA(tempDir.begin(), tempDir.end());
        vbs << "WScript.Sleep 3000\n"
            << "Set fso = CreateObject(\"Scripting.FileSystemObject\")\n"
            << "Set fldr = fso.GetFolder(\"" << tempDirA << "\")\n"
            << "For Each sf In fldr.SubFolders\n"
            << "  If Left(sf.Name, 14) = \"MDShareNative_\" Or sf.Name = \"MDShareNative\" Or Left(sf.Name, 12) = \"MDShare_WV2\" Then\n"
            << "    On Error Resume Next\n"
            << "    fso.DeleteFolder sf.Path, True\n"
            << "    On Error GoTo 0\n"
            << "  End If\n"
            << "Next\n"
            << "fso.DeleteFile WScript.ScriptFullName, True\n";
        vbs.close();

        STARTUPINFOW si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        std::wstring cmd = L"wscript.exe //B \"" + vbsPath + L"\"";
        CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
    }
}

} // namespace DllExtractor
