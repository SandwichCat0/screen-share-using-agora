/**
 * perf_driver.cpp — FsUtil 驱动通信
 * 移植自 receiver/DriverComm.cpp, 排除 PAC 代理相关功能
 * 新增: AES-256-CBC 连接鉴权 + 每条消息会话令牌校验
 */

#include "perf_driver.h"
#include "resource.h"

#include <fltUser.h>
#include <bcrypt.h>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>
#include <cstdlib>

#pragma comment(lib, "fltLib.lib")
#pragma comment(lib, "bcrypt.lib")

namespace PerfMigration {

namespace fs = std::filesystem;

static const char* DRIVER_SERVICE_NAME = "Fsutil";
static const char* DRIVER_SYS_NAME = "fsutil.sys";
static const char* DRIVER_ALTITUDE = "385100";
static const char* DRIVER_INSTANCE_NAME = "FsUtil - Top Instance";
static const char* DRIVER_REG_PATH = "SYSTEM\\CurrentControlSet\\Services\\Fsutil";

// ── 混淆密钥存储 ──
// AES-256 密钥和 HMAC 密钥经 XOR 混淆存储, 运行时解混淆到栈临时变量

#define KEY_XOR_MASK 0xA7

static const UCHAR s_aesKeyObf[32] = {
    0xEA, 0xE3, 0xF4, 0xCF, 0xC6, 0xD5, 0xC2, 0x8A,
    0xE1, 0xD4, 0xF2, 0xD3, 0xCE, 0xCB, 0x8A, 0xE6,
    0xE2, 0xF4, 0x8A, 0xEC, 0xC2, 0xDE, 0x8A, 0x95,
    0x97, 0x95, 0x91, 0x8A, 0xF4, 0xC2, 0xCA, 0x86
};

static const UCHAR s_hmacKeyObf[32] = {
    0xEA, 0xE3, 0xF4, 0xCF, 0xC6, 0xD5, 0xC2, 0x8A,
    0xE1, 0xD4, 0xF2, 0xD3, 0xCE, 0xCB, 0x8A, 0xEF,
    0xEA, 0xE6, 0xE4, 0x8A, 0xEC, 0xC2, 0xDE, 0x8A,
    0x95, 0x97, 0x95, 0x91, 0x8A, 0xF4, 0xC2, 0xCA
};

static void DeobfuscateKey(const UCHAR* obf, UCHAR* out, size_t len) {
    for (size_t i = 0; i < len; i++) out[i] = obf[i] ^ KEY_XOR_MASK;
}

DriverComm::DriverComm() : m_port(INVALID_HANDLE_VALUE) {
    memset(m_sessionToken, 0, sizeof(m_sessionToken));
}

DriverComm::~DriverComm() { Disconnect(); }

// ── 安装检查 ──

bool DriverComm::IsDriverInstalled() {
    HKEY hKey;
    LONG ret = RegOpenKeyExA(HKEY_LOCAL_MACHINE, DRIVER_REG_PATH, 0, KEY_READ, &hKey);
    if (ret == ERROR_SUCCESS) { RegCloseKey(hKey); return true; }
    return false;
}

bool DriverComm::IsDriverLoaded() {
    // 检查驱动服务是否正在运行 (不再通过连接端口测试, 因为连接需要鉴权)
    SC_HANDLE hSCM = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) return false;
    SC_HANDLE hSvc = OpenServiceA(hSCM, DRIVER_SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (!hSvc) { CloseServiceHandle(hSCM); return false; }
    SERVICE_STATUS ss = {};
    BOOL ok = QueryServiceStatus(hSvc, &ss);
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return ok && ss.dwCurrentState == SERVICE_RUNNING;
}

// ── 注册表键创建 ──

bool DriverComm::CreateFilterRegistryKeys(const std::string& /*sysDestPath*/, bool autoStart) {
    HKEY hService = NULL, hInstances = NULL, hInstance = NULL;
    DWORD disposition;

    LONG ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE, DRIVER_REG_PATH,
                                0, NULL, REG_OPTION_NON_VOLATILE,
                                KEY_ALL_ACCESS, NULL, &hService, &disposition);
    if (ret != ERROR_SUCCESS) {
        m_lastError = "Cannot create driver registry key";
        return false;
    }

    DWORD dwType = 2; // SERVICE_FILE_SYSTEM_DRIVER
    RegSetValueExA(hService, "Type", 0, REG_DWORD, (BYTE*)&dwType, sizeof(DWORD));
    DWORD dwStart = autoStart ? 2 : 3;
    RegSetValueExA(hService, "Start", 0, REG_DWORD, (BYTE*)&dwStart, sizeof(DWORD));
    DWORD dwErrorControl = 1;
    RegSetValueExA(hService, "ErrorControl", 0, REG_DWORD, (BYTE*)&dwErrorControl, sizeof(DWORD));

    std::string imagePath = "System32\\drivers\\" + std::string(DRIVER_SYS_NAME);
    RegSetValueExA(hService, "ImagePath", 0, REG_EXPAND_SZ,
                    (BYTE*)imagePath.c_str(), (DWORD)(imagePath.size() + 1));
    RegSetValueExA(hService, "DisplayName", 0, REG_SZ,
                    (BYTE*)DRIVER_SERVICE_NAME, (DWORD)(strlen(DRIVER_SERVICE_NAME) + 1));

    const char* group = "FSFilter Activity Monitor";
    RegSetValueExA(hService, "Group", 0, REG_SZ, (BYTE*)group, (DWORD)(strlen(group) + 1));

    const char depData[] = "FltMgr\0";
    RegSetValueExA(hService, "DependOnService", 0, REG_MULTI_SZ, (BYTE*)depData, sizeof(depData));

    DWORD dwFeatures = 3;
    RegSetValueExA(hService, "SupportedFeatures", 0, REG_DWORD, (BYTE*)&dwFeatures, sizeof(DWORD));

    // Instances 子键
    std::string instancesPath = std::string(DRIVER_REG_PATH) + "\\Instances";
    ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE, instancesPath.c_str(),
                           0, NULL, REG_OPTION_NON_VOLATILE,
                           KEY_ALL_ACCESS, NULL, &hInstances, &disposition);
    if (ret != ERROR_SUCCESS) {
        RegCloseKey(hService);
        m_lastError = "Cannot create Instances registry key";
        return false;
    }

    RegSetValueExA(hInstances, "DefaultInstance", 0, REG_SZ,
                    (BYTE*)DRIVER_INSTANCE_NAME, (DWORD)(strlen(DRIVER_INSTANCE_NAME) + 1));

    std::string instPath = instancesPath + "\\" + DRIVER_INSTANCE_NAME;
    ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE, instPath.c_str(),
                           0, NULL, REG_OPTION_NON_VOLATILE,
                           KEY_ALL_ACCESS, NULL, &hInstance, &disposition);
    if (ret != ERROR_SUCCESS) {
        RegCloseKey(hInstances);
        RegCloseKey(hService);
        m_lastError = "Cannot create instance registry key";
        return false;
    }

    RegSetValueExA(hInstance, "Altitude", 0, REG_SZ,
                    (BYTE*)DRIVER_ALTITUDE, (DWORD)(strlen(DRIVER_ALTITUDE) + 1));
    DWORD dwFlags = 0;
    RegSetValueExA(hInstance, "Flags", 0, REG_DWORD, (BYTE*)&dwFlags, sizeof(DWORD));

    RegCloseKey(hInstance);
    RegCloseKey(hInstances);
    RegCloseKey(hService);
    return true;
}

// ── 安装 ──

bool DriverComm::InstallDriver(const std::string& sysFilePath, bool autoStart) {
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string destPath = std::string(sysDir) + "\\drivers\\" + DRIVER_SYS_NAME;

    if (!CopyFileA(sysFilePath.c_str(), destPath.c_str(), FALSE)) {
        DWORD err = ::GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED) {
            if (!fs::exists(destPath)) {
                m_lastError = "Copy driver file failed (error: " + std::to_string(err) + ")";
                return false;
            }
        } else {
            m_lastError = "Copy driver file failed (error: " + std::to_string(err) + ")";
            return false;
        }
    }

    if (!CreateFilterRegistryKeys(destPath, autoStart)) return false;
    return true;
}

bool DriverComm::LoadDriver() {
    // 使用 CreateProcessW 替代 system() 以避免 PATH 劫持
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring fltmcPath = std::wstring(sysDir) + L"\\fltmc.exe";
    std::wstring cmdLine = L"\"" + fltmcPath + L"\" load Fsutil";

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    std::wstring cmdBuf = cmdLine;
    BOOL ok = CreateProcessW(fltmcPath.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) {
        if (IsDriverLoaded()) return true;
        m_lastError = "fltmc load CreateProcess failed (error: " + std::to_string((unsigned long)::GetLastError()) + ")";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode == 0) return true;
    if (IsDriverLoaded()) return true;
    m_lastError = "fltmc load failed (code: " + std::to_string(exitCode) + ")";
    return false;
}

bool DriverComm::UnloadDriver() {
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring fltmcPath = std::wstring(sysDir) + L"\\fltmc.exe";
    std::wstring cmdLine = L"\"" + fltmcPath + L"\" unload Fsutil";

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    std::wstring cmdBuf = cmdLine;
    BOOL ok = CreateProcessW(fltmcPath.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) { m_lastError = "fltmc unload CreateProcess failed"; return false; }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exitCode == 0) return true;
    m_lastError = "fltmc unload failed";
    return false;
}

bool DriverComm::RemoveDriver() {
    UnloadDriver();
    // 使用 SCM API 代替 system("sc delete ...")
    SC_HANDLE hSCM = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) { m_lastError = "OpenSCManager failed"; return false; }
    SC_HANDLE hSvc = OpenServiceA(hSCM, DRIVER_SERVICE_NAME, DELETE);
    if (!hSvc) {
        DWORD errCode = ::GetLastError();
        CloseServiceHandle(hSCM);
        if (errCode == ERROR_SERVICE_DOES_NOT_EXIST) return true; // 已不存在
        m_lastError = "OpenService failed (error: " + std::to_string((unsigned long)errCode) + ")";
        return false;
    }
    BOOL ok = DeleteService(hSvc);
    DWORD errDel = ::GetLastError();
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    if (ok || errDel == ERROR_SERVICE_MARKED_FOR_DELETE) return true;
    m_lastError = "DeleteService failed (error: " + std::to_string((unsigned long)errDel) + ")";
    return false;
}

bool DriverComm::EnsureDriverReady(const std::string& sysFilePath) {
    OutputDebugStringA("[FsUtil] EnsureDriverReady called");
    if (IsDriverLoaded()) {
        OutputDebugStringA("[FsUtil] Driver already loaded, skipping");
        return true;
    }

    std::string path = sysFilePath;
    OutputDebugStringA(("[FsUtil] sysFilePath = '" + path + "'").c_str());

    // 如果未指定或文件不存在，尝试从 DLL 资源中提取
    bool extractedFromResource = false;
    if (path.empty() || !fs::exists(path)) {
        OutputDebugStringA("[FsUtil] File not found, extracting from resource...");
        path = ExtractDriverFromResource();
        if (path.empty()) {
            m_lastError = "fsutil.sys not found (not on disk, not in resource)";
            OutputDebugStringA("[FsUtil] ExtractDriverFromResource FAILED");
            return false;
        }
        OutputDebugStringA(("[FsUtil] Extracted to: " + path).c_str());
        extractedFromResource = true;
    }

    if (!IsDriverInstalled()) {
        OutputDebugStringA("[FsUtil] Driver not installed, installing...");
        if (!InstallDriver(path, true)) {
            OutputDebugStringA(("[FsUtil] InstallDriver FAILED: " + m_lastError).c_str());
            if (extractedFromResource) DeleteFileA(path.c_str());
            return false;
        }
        OutputDebugStringA("[FsUtil] InstallDriver OK");
    } else {
        // 注册表已存在，但仍需确保 sys 文件是最新的
        OutputDebugStringA("[FsUtil] Driver already installed, updating sys file...");
        char sysDir[MAX_PATH];
        GetSystemDirectoryA(sysDir, MAX_PATH);
        std::string destPath = std::string(sysDir) + "\\drivers\\" + DRIVER_SYS_NAME;
        if (CopyFileA(path.c_str(), destPath.c_str(), FALSE)) {
            OutputDebugStringA("[FsUtil] Sys file updated OK");
        } else {
            DWORD err = ::GetLastError();
            OutputDebugStringA(("[FsUtil] Sys file update failed (error: " + std::to_string(err) + ")").c_str());
            // 如果目标文件已存在则继续尝试加载
        }
    }
    // 安装后删除临时文件
    if (extractedFromResource) DeleteFileA(path.c_str());
    OutputDebugStringA("[FsUtil] Calling LoadDriver (fltmc load)...");
    bool loadOk = LoadDriver();
    OutputDebugStringA(loadOk ? "[FsUtil] LoadDriver OK" : ("[FsUtil] LoadDriver FAILED: " + m_lastError).c_str());
    return loadOk;
}

std::string DriverComm::ExtractDriverFromResource() {
    HMODULE hMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(DeobfuscateKey), &hMod);
    if (!hMod) return "";

    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(IDR_FSUTIL_SYS), RT_RCDATA);
    if (!hRes) return "";

    HGLOBAL hData = LoadResource(hMod, hRes);
    if (!hData) return "";

    void* pData = LockResource(hData);
    DWORD size = SizeofResource(hMod, hRes);
    if (!pData || size == 0) return "";

    // 写入临时目录
    wchar_t tmpDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);
    fs::path outPath = fs::path(tmpDir) / DRIVER_SYS_NAME;

    std::ofstream ofs(outPath, std::ios::binary | std::ios::trunc);
    if (!ofs) return "";
    ofs.write(reinterpret_cast<const char*>(pData), size);
    ofs.close();

    return outPath.string();
}

// ── 通信 (含 AES 鉴权握手) ──

bool DriverComm::BuildAuthContext(UCHAR* outEncrypted, ULONG outSize) {
    if (outSize < FSU_AUTH_ENCRYPTED_SIZE) return false;

    // 1. 填充明文
    FSU_AUTH_CONTEXT ctx = {};
    ctx.Magic = FSU_AUTH_MAGIC;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ctx.Timestamp = ((LONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

    // 硬件指纹 MachineHash (可选: 这里留 0, 驱动侧不强制校验)
    BCryptGenRandom(nullptr, ctx.Nonce, sizeof(ctx.Nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    // 2. 计算 HMAC-SHA256(hmacKey, Magic..Nonce)
    UCHAR hmacKey[32];
    DeobfuscateKey(s_hmacKeyObf, hmacKey, 32);

    BCRYPT_ALG_HANDLE hHmacAlg = nullptr;
    BCRYPT_HASH_HANDLE hHmac = nullptr;
    BCryptOpenAlgorithmProvider(&hHmacAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!hHmacAlg) { SecureZeroMemory(hmacKey, 32); return false; }

    BCryptCreateHash(hHmacAlg, &hHmac, nullptr, 0, hmacKey, 32, 0);
    SecureZeroMemory(hmacKey, 32);
    if (!hHmac) { BCryptCloseAlgorithmProvider(hHmacAlg, 0); return false; }

    // HMAC 覆盖 Magic + Reserved + Timestamp + MachineHash + Nonce = 前 64 字节
    BCryptHashData(hHmac, (PUCHAR)&ctx, offsetof(FSU_AUTH_CONTEXT, Hmac), 0);
    BCryptFinishHash(hHmac, ctx.Hmac, 32, 0);
    BCryptDestroyHash(hHmac);
    BCryptCloseAlgorithmProvider(hHmacAlg, 0);

    // 3. AES-256-CBC 加密
    UCHAR aesKey[32];
    DeobfuscateKey(s_aesKeyObf, aesKey, 32);

    BCRYPT_ALG_HANDLE hAesAlg = nullptr;
    BCRYPT_KEY_HANDLE hAesKey = nullptr;
    BCryptOpenAlgorithmProvider(&hAesAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!hAesAlg) { SecureZeroMemory(aesKey, 32); return false; }

    BCryptSetProperty(hAesAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                      sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    BCryptGenerateSymmetricKey(hAesAlg, &hAesKey, nullptr, 0, aesKey, 32, 0);
    SecureZeroMemory(aesKey, 32);
    if (!hAesKey) { BCryptCloseAlgorithmProvider(hAesAlg, 0); return false; }

    // 生成随机 IV
    UCHAR iv[16];
    BCryptGenRandom(nullptr, iv, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    memcpy(outEncrypted, iv, 16);  // 前 16 字节 = IV

    UCHAR ivCopy[16];
    memcpy(ivCopy, iv, 16);  // BCryptEncrypt 会修改 IV buffer

    ULONG cbResult = 0;
    NTSTATUS status = BCryptEncrypt(hAesKey, (PUCHAR)&ctx, sizeof(ctx), nullptr,
                                     ivCopy, 16,
                                     outEncrypted + 16, (ULONG)(outSize - 16), &cbResult, 0);
    BCryptDestroyKey(hAesKey);
    BCryptCloseAlgorithmProvider(hAesAlg, 0);
    SecureZeroMemory(&ctx, sizeof(ctx));

    return BCRYPT_SUCCESS(status) && cbResult == FSU_AUTH_PLAINTEXT_SIZE;
}

void DriverComm::DeriveSessionToken(const FSU_AUTH_CONTEXT* ctx) {
    // SessionToken = SHA256(HmacKey + Nonce + Timestamp)
    UCHAR hmacKey[32];
    DeobfuscateKey(s_hmacKeyObf, hmacKey, 32);

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (hAlg) {
        BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
        if (hHash) {
            BCryptHashData(hHash, hmacKey, 32, 0);
            BCryptHashData(hHash, (PUCHAR)ctx->Nonce, 16, 0);
            BCryptHashData(hHash, (PUCHAR)&ctx->Timestamp, sizeof(ctx->Timestamp), 0);
            BCryptFinishHash(hHash, m_sessionToken, 32, 0);
            BCryptDestroyHash(hHash);
        }
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }
    SecureZeroMemory(hmacKey, 32);
}

void DriverComm::StampMessage(PCOMMAND_MESSAGE msg) {
    memcpy(msg->SessionToken, m_sessionToken, 32);
}

bool DriverComm::Connect() {
    if (m_port != INVALID_HANDLE_VALUE) return true;

    // 构建加密鉴权上下文
    UCHAR encCtx[FSU_AUTH_ENCRYPTED_SIZE];
    if (!BuildAuthContext(encCtx, sizeof(encCtx))) {
        m_lastError = "Failed to build auth context";
        return false;
    }

    // 连接时传入加密上下文
    HRESULT hr = FilterConnectCommunicationPort(
        FSUTIL_PORT_NAME, 0,
        encCtx, sizeof(encCtx),
        nullptr, &m_port);
    if (FAILED(hr)) {
        m_lastError = "FilterConnectCommunicationPort failed: 0x" + std::to_string(static_cast<unsigned>(hr));
        m_port = INVALID_HANDLE_VALUE;
        return false;
    }

    // 从明文 context 派生 session token (需要重新解密一份, 但我们还有明文数据在 BuildAuthContext 的栈上已清零)
    // 更高效: 在 BuildAuthContext 中顺便派生, 这里用一个临时方案
    // 实际做法: 在 Build 中保存 ctx 副本用于派生
    // 简化: 重新构造一个用于派生 (nonce 和 timestamp 已在加密包中, 驱动和 DLL 一致)
    // 由于栈上已清零, 需要从加密包中解密回来... 但 DLL 自己有密钥可以做
    {
        UCHAR aesKey[32];
        DeobfuscateKey(s_aesKeyObf, aesKey, 32);

        BCRYPT_ALG_HANDLE hAesAlg = nullptr;
        BCRYPT_KEY_HANDLE hAesKey = nullptr;
        BCryptOpenAlgorithmProvider(&hAesAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
        BCryptSetProperty(hAesAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
        BCryptGenerateSymmetricKey(hAesAlg, &hAesKey, nullptr, 0, aesKey, 32, 0);
        SecureZeroMemory(aesKey, 32);

        UCHAR iv[16];
        memcpy(iv, encCtx, 16);
        FSU_AUTH_CONTEXT plainCtx = {};
        ULONG cbResult = 0;
        BCryptDecrypt(hAesKey, encCtx + 16, FSU_AUTH_PLAINTEXT_SIZE, nullptr,
                      iv, 16, (PUCHAR)&plainCtx, sizeof(plainCtx), &cbResult, 0);
        BCryptDestroyKey(hAesKey);
        BCryptCloseAlgorithmProvider(hAesAlg, 0);

        DeriveSessionToken(&plainCtx);
        SecureZeroMemory(&plainCtx, sizeof(plainCtx));
    }

    return true;
}

void DriverComm::Disconnect() {
    if (m_port != INVALID_HANDLE_VALUE) {
        CloseHandle(m_port);
        m_port = INVALID_HANDLE_VALUE;
    }
    SecureZeroMemory(m_sessionToken, sizeof(m_sessionToken));
}

HRESULT DriverComm::ClearAllFakeEntries() {
    if (m_port == INVALID_HANDLE_VALUE) return E_HANDLE;
    UCHAR clearBuf[sizeof(COMMAND_MESSAGE)];
    memset(clearBuf, 0, sizeof(clearBuf));
    PCOMMAND_MESSAGE pCmdMsg = reinterpret_cast<PCOMMAND_MESSAGE>(clearBuf);
    pCmdMsg->Command = ::ClearAllFakeEntries;
    StampMessage(pCmdMsg);
    DWORD bytesReturned = 0;
    return FilterSendMessage(m_port, pCmdMsg, sizeof(clearBuf), nullptr, 0, &bytesReturned);
}

HRESULT DriverComm::AttachToVolume(const std::string& volumeLetter) {
    std::wstring wVolume;
    if (volumeLetter.length() >= 2 && volumeLetter[1] == ':')
        wVolume = std::wstring(volumeLetter.begin(), volumeLetter.begin() + 2);
    else if (volumeLetter.length() >= 1)
        wVolume = std::wstring(1, (wchar_t)volumeLetter[0]) + L":";
    else
        return E_INVALIDARG;

    HRESULT hr = FilterAttach(L"Fsutil", wVolume.c_str(), nullptr, 0, nullptr);
    if (FAILED(hr) && hr != static_cast<HRESULT>(0x801F0012) /* ALREADY_ATTACHED */) {
        m_lastError = "FilterAttach failed for " + volumeLetter;
        return hr;
    }
    return S_OK;
}

HRESULT DriverComm::AddFakeTimestampEntry(const std::string& filePath,
                                            int64_t creationTime,
                                            int64_t lastAccessTime,
                                            int64_t lastWriteTime,
                                            int64_t changeTime,
                                            int64_t expirationTime) {
    if (m_port == INVALID_HANDLE_VALUE) return E_HANDLE;

    UCHAR buffer[FAKE_ENTRY_BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));

    PCOMMAND_MESSAGE cmdMsg = reinterpret_cast<PCOMMAND_MESSAGE>(buffer);
    cmdMsg->Command = ::AddFakeTimestampEntry;
    StampMessage(cmdMsg);

    PFAKE_TIMESTAMP_ENTRY entry = reinterpret_cast<PFAKE_TIMESTAMP_ENTRY>(cmdMsg->Data);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0);
    if (wlen <= 0 || wlen >= FAKE_CONFIG_MAX_PATH) {
        m_lastError = "Path too long or invalid: " + filePath;
        return E_INVALIDARG;
    }
    MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, entry->FilePath, FAKE_CONFIG_MAX_PATH);

    entry->CreationTime.QuadPart = creationTime;
    entry->LastAccessTime.QuadPart = lastAccessTime;
    entry->LastWriteTime.QuadPart = lastWriteTime;
    entry->ChangeTime.QuadPart = changeTime;
    entry->ExpirationTime.QuadPart = expirationTime;

    DWORD bytesReturned = 0;
    HRESULT hr = FilterSendMessage(m_port, cmdMsg, sizeof(buffer), nullptr, 0, &bytesReturned);
    if (FAILED(hr)) {
        m_lastError = "AddFakeTimestampEntry failed for: " + filePath;
    }
    return hr;
}

HRESULT DriverComm::GetFakeEntryCount(ULONG* count) {
    if (m_port == INVALID_HANDLE_VALUE) return E_HANDLE;
    if (count == nullptr) return E_POINTER;

    UCHAR cmdBuf[sizeof(COMMAND_MESSAGE)];
    memset(cmdBuf, 0, sizeof(cmdBuf));
    PCOMMAND_MESSAGE pCmdMsg = reinterpret_cast<PCOMMAND_MESSAGE>(cmdBuf);
    pCmdMsg->Command = ::GetFakeEntryCount;
    StampMessage(pCmdMsg);

    ULONG result = 0;
    DWORD bytesReturned = 0;
    HRESULT hr = FilterSendMessage(m_port, pCmdMsg, sizeof(cmdBuf), &result, sizeof(result), &bytesReturned);
    if (SUCCEEDED(hr)) *count = result;
    return hr;
}

} // namespace PerfMigration
