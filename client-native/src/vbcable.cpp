/**
 * vbcable.cpp — VB-Audio Virtual Cable 检测/安装/卸载
 * 对应 C# 版全部 VBCable 相关功能
 */
#include "vbcable.h"
#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <winhttp.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <softpub.h>
#include <mscat.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <fstream>
#include <filesystem>
#include <regex>
#include <sstream>
#include <thread>
#include <chrono>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wintrust.lib")

namespace fs = std::filesystem;

namespace MDShare {
namespace VBCable {

// ── 辅助：OEM INF 标记文件路径 ─────────────────────────

static fs::path GetFlagFilePath() {
    wchar_t appData[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData);
    return fs::path(appData) / "Microsoft" / "WebView2" / ".vbcable_oemdrv";
}

// ── WMI 查询 VBCable 设备 ─────────────────────────────

static bool QueryVBCableDevice(bool& installed, std::string& deviceName, bool& hasError) {
    installed = false;
    deviceName = "";
    hasError = false;

    IWbemLocator* locator = nullptr;
    IWbemServices* service = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator, (void**)&locator);
    if (FAILED(hr)) return false;

    hr = locator->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &service);
    if (FAILED(hr)) { locator->Release(); return false; }

    CoSetProxyBlanket(service, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    IEnumWbemClassObject* enumerator = nullptr;
    hr = service->ExecQuery(
        _bstr_t(L"WQL"),
        _bstr_t(L"SELECT Name, ConfigManagerErrorCode FROM Win32_PnPEntity "
                L"WHERE Name LIKE '%VB-Audio Virtual Cable%' OR PNPDeviceID LIKE '%VBAudioVACWDM%'"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &enumerator);

    if (SUCCEEDED(hr)) {
        IWbemClassObject* obj = nullptr;
        ULONG count = 0;
        while (enumerator->Next(WBEM_INFINITE, 1, &obj, &count) == S_OK) {
            installed = true;

            VARIANT vtName, vtErr;
            obj->Get(L"Name", 0, &vtName, nullptr, nullptr);
            obj->Get(L"ConfigManagerErrorCode", 0, &vtErr, nullptr, nullptr);

            if (vtName.vt == VT_BSTR)
                deviceName = WideToUtf8(vtName.bstrVal);
            if (deviceName.empty())
                deviceName = "VB-Audio Virtual Cable";

            uint32_t errCode = (vtErr.vt == VT_I4) ? vtErr.lVal : 0;
            if (errCode != 0) {
                hasError = true;
            } else {
                hasError = false;
                VariantClear(&vtName);
                VariantClear(&vtErr);
                obj->Release();
                break;
            }

            VariantClear(&vtName);
            VariantClear(&vtErr);
            obj->Release();
        }
        enumerator->Release();
    }

    service->Release();
    locator->Release();
    return true;
}

// ── 轮询设备出现/消失 ────────────────────────────────

static std::string PollForDevice(int timeoutSec, bool untilGone = false) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);

    while (std::chrono::steady_clock::now() < deadline) {
        bool installed = false;
        std::string name;
        bool hasError = false;
        QueryVBCableDevice(installed, name, hasError);

        if (!untilGone && installed && !hasError) return name;
        if (untilGone && !installed) return ""; // 空 = 已消失

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    return untilGone ? "still-present" : "";
}

// ── 在 pnputil 输出中查找 oem*.inf ───────────────────

static std::string FindOemInf(const std::string& output) {
    std::string candidate;
    std::istringstream stream(output);
    std::string line;

    std::regex pubNameRe(R"(^(?:Published Name|发布名称?)\s*:\s*(oem\d+\.inf))", std::regex::icase);

    while (std::getline(stream, line)) {
        // trim
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();

        if (line.empty()) { candidate.clear(); continue; }

        std::smatch match;
        if (std::regex_search(line, match, pubNameRe)) {
            candidate = match[1].str();
            continue;
        }

        if (!candidate.empty()) {
            // 检查是否含 VBCable 关键字
            std::string lower = line;
            for (auto& c : lower) c = (char)tolower((unsigned char)c);
            if (lower.find("vbmmecable") != std::string::npos ||
                lower.find("vbaudiovacwdm") != std::string::npos ||
                lower.find("vb-audio") != std::string::npos) {
                return candidate;
            }
        }
    }
    return "";
}

// ── 运行命令并捕获输出 ───────────────────────────────

static std::string RunCommand(const std::wstring& cmd) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return "";
    if (!SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hReadPipe); CloseHandle(hWritePipe); return "";
    }

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::wstring cmdLine = cmd;
    BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWritePipe);

    std::string output;
    if (ok) {
        char buf[4096];
        DWORD bytesRead;
        while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buf[bytesRead] = 0;
            output += buf;
        }
        DWORD waitResult = WaitForSingleObject(pi.hProcess, 10000);
        if (waitResult == WAIT_TIMEOUT) {
            // 超时后终止子进程，避免僵尸进程
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 3000);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(hReadPipe);
    return output;
}

// ── 保存 OEM INF ─────────────────────────────────────

static void SaveOemInf() {
    auto output = RunCommand(L"pnputil.exe /enum-drivers");
    auto oemInf = FindOemInf(output);
    if (!oemInf.empty()) {
        auto flagPath = GetFlagFilePath();
        fs::create_directories(flagPath.parent_path());
        std::ofstream ofs(flagPath);
        ofs << oemInf;
    }
}

// ── 下载文件（WinHTTP） ──────────────────────────────

static bool DownloadFile(const std::wstring& url, const fs::path& outPath) {
    // 解析 URL: https://host/path
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostName[256]{};
    wchar_t urlPath[1024]{};
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 1024;

    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &urlComp))
        return false;

    HINTERNET hSession = WinHttpOpen(L"Microsoft WebView2 Runtime Host/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath, nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::ofstream ofs(outPath, std::ios::binary);
    char buf[8192];
    DWORD bytesRead;
    while (WinHttpReadData(hRequest, buf, sizeof(buf), &bytesRead) && bytesRead > 0) {
        ofs.write(buf, bytesRead);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return true;
}

// ── 解压 ZIP（Shell COM） ────────────────────────────

static bool UnzipFile(const fs::path& zipPath, const fs::path& destDir) {
    // 使用 PowerShell 解压，对路径中的特殊字符进行转义
    auto escapePath = [](const std::wstring& p) -> std::wstring {
        std::wstring escaped;
        for (wchar_t c : p) {
            if (c == L'\'') escaped += L"''";
            else escaped += c;
        }
        return escaped;
    };
    std::wstring cmd = L"powershell.exe -NoProfile -Command \"Expand-Archive -Path '"
        + escapePath(zipPath.wstring()) + L"' -DestinationPath '" + escapePath(destDir.wstring()) + L"' -Force\"";

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::wstring cmdLine = cmd;
    BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return false;

    WaitForSingleObject(pi.hProcess, 60000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode == 0;
}

// ── Check ────────────────────────────────────────────

json Check() {
    bool installed = false;
    std::string deviceName;
    bool hasError = false;
    QueryVBCableDevice(installed, deviceName, hasError);

    json result;
    result["installed"]  = installed;
    result["deviceName"] = deviceName;
    result["hasError"]   = hasError;
    return result;
}

// ── IPolicyConfig (undocumented COM interface for setting default audio device) ──

// {F8679F50-850A-41CF-9C72-430F290290C8}
static const GUID IID_IPolicyConfig = {
    0xF8679F50, 0x850A, 0x41CF, {0x9C, 0x72, 0x43, 0x0F, 0x29, 0x02, 0x90, 0xC8}
};

// {870AF99C-171D-4F9E-AF0D-E63DF40C2BC9}
static const GUID CLSID_CPolicyConfigClient = {
    0x870AF99C, 0x171D, 0x4F9E, {0xAF, 0x0D, 0xE6, 0x3D, 0xF4, 0x0C, 0x2B, 0xC9}
};

MIDL_INTERFACE("F8679F50-850A-41CF-9C72-430F290290C8")
IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

// ── CheckDefaultRecording ────────────────────────────

json CheckDefaultRecording() {
    json result;
    result["isCableOutput"] = false;
    result["currentDevice"] = "";
    result["installed"] = false;

    IMMDeviceEnumerator* pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) return result;

    // Check if VBCable is installed first
    bool vbInstalled = false;
    std::string vbName;
    bool vbErr = false;
    QueryVBCableDevice(vbInstalled, vbName, vbErr);
    result["installed"] = vbInstalled && !vbErr;

    // Get default recording (capture) device
    IMMDevice* pDefault = nullptr;
    hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDefault);
    if (SUCCEEDED(hr) && pDefault) {
        IPropertyStore* pProps = nullptr;
        hr = pDefault->OpenPropertyStore(STGM_READ, &pProps);
        if (SUCCEEDED(hr) && pProps) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            // PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0}, 14
            hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
            if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
                std::wstring name(varName.pwszVal);
                result["currentDevice"] = WideToUtf8(name.c_str());
                // Check if it contains "CABLE Output" (VB-Cable recording device name)
                if (name.find(L"CABLE Output") != std::wstring::npos) {
                    result["isCableOutput"] = true;
                }
            }
            PropVariantClear(&varName);
            pProps->Release();
        }
        pDefault->Release();
    }

    pEnumerator->Release();
    return result;
}

// ── SetDefaultRecordingToCable ───────────────────────

json SetDefaultRecordingToCable() {
    json result;
    result["success"] = false;

    IMMDeviceEnumerator* pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) { result["error"] = "Failed to create MMDeviceEnumerator"; return result; }

    // Enumerate all capture (recording) devices to find CABLE Output
    IMMDeviceCollection* pCollection = nullptr;
    hr = pEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) { pEnumerator->Release(); result["error"] = "Failed to enumerate capture devices"; return result; }

    UINT count = 0;
    pCollection->GetCount(&count);

    LPWSTR targetDeviceId = nullptr;
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* pDevice = nullptr;
        if (FAILED(pCollection->Item(i, &pDevice))) continue;

        IPropertyStore* pProps = nullptr;
        if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR) {
                std::wstring name(varName.pwszVal);
                if (name.find(L"CABLE Output") != std::wstring::npos) {
                    pDevice->GetId(&targetDeviceId);
                }
            }
            PropVariantClear(&varName);
            pProps->Release();
        }
        pDevice->Release();
        if (targetDeviceId) break;
    }
    pCollection->Release();

    if (!targetDeviceId) {
        pEnumerator->Release();
        result["error"] = "CABLE Output device not found";
        return result;
    }

    // Use IPolicyConfig to set default
    IPolicyConfig* pPolicyConfig = nullptr;
    hr = CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_INPROC_SERVER,
        IID_IPolicyConfig, (void**)&pPolicyConfig);
    if (SUCCEEDED(hr) && pPolicyConfig) {
        hr = pPolicyConfig->SetDefaultEndpoint(targetDeviceId, eConsole);
        if (SUCCEEDED(hr)) {
            pPolicyConfig->SetDefaultEndpoint(targetDeviceId, eMultimedia);
            pPolicyConfig->SetDefaultEndpoint(targetDeviceId, eCommunications);
            result["success"] = true;
        } else {
            result["error"] = "SetDefaultEndpoint failed";
        }
        pPolicyConfig->Release();
    } else {
        result["error"] = "Failed to create PolicyConfig";
    }

    CoTaskMemFree(targetDeviceId);
    pEnumerator->Release();
    return result;
}

// ── Install ──────────────────────────────────────────

void Install(SendCallback sendFn) {
    json progress;
    progress["status"] = "downloading";
    progress["percent"] = 0;
    sendFn("vbcable-install-progress", progress);

    // 预检
    bool installed = false;
    std::string deviceName;
    bool hasError = false;
    QueryVBCableDevice(installed, deviceName, hasError);

    if (installed && !hasError) {
        json result;
        result["success"] = true;
        result["deviceName"] = deviceName;
        sendFn("vbcable-install-result", result);
        return;
    }
    if (installed && hasError) {
        json result;
        result["success"] = false;
        result["error"] = "Detected " + deviceName + " (device has errors).\nPlease uninstall the driver first, then reinstall.";
        sendFn("vbcable-install-result", result);
        return;
    }

    // 下载
    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    fs::path tempDir = fs::path(tempPath) / "MSWebView2_VBC";
    fs::create_directories(tempDir);
    fs::path zipPath = tempDir / "VBCABLE.zip";
    fs::path extractDir = tempDir / "VBCABLE_Driver";

    bool downloaded = DownloadFile(
        L"https://vbcable.oss-cn-beijing.aliyuncs.com/VBCABLE_Driver_Pack45.zip",
        zipPath);

    if (!downloaded) {
        json result;
        result["success"] = false;
        result["error"] = "下载 VBCable 安装包失败";
        sendFn("vbcable-install-result", result);
        try { fs::remove_all(tempDir); } catch (...) {}
        return;
    }

    progress["status"] = "installing";
    progress["percent"] = 60;
    sendFn("vbcable-install-progress", progress);

    // 解压
    if (fs::exists(extractDir)) fs::remove_all(extractDir);
    if (!UnzipFile(zipPath, extractDir)) {
        json result;
        result["success"] = false;
        result["error"] = "解压安装包失败";
        sendFn("vbcable-install-result", result);
        try { fs::remove_all(tempDir); } catch (...) {}
        return;
    }

    // 查找安装程序
    fs::path installer;
    for (auto& entry : fs::recursive_directory_iterator(extractDir)) {
        if (!entry.is_regular_file()) continue;
        auto name = entry.path().filename().wstring();
        if (_wcsicmp(name.c_str(), L"VBCABLE_Setup_x64.exe") == 0) {
            installer = entry.path();
            break;
        }
    }
    if (installer.empty()) {
        for (auto& entry : fs::recursive_directory_iterator(extractDir)) {
            if (!entry.is_regular_file()) continue;
            auto name = entry.path().filename().wstring();
            if (name.find(L"VBCABLE_Setup") != std::wstring::npos && name.find(L".exe") != std::wstring::npos) {
                installer = entry.path();
                break;
            }
        }
    }

    if (installer.empty()) {
        json result;
        result["success"] = false;
        result["error"] = "安装包内未找到安装程序";
        sendFn("vbcable-install-result", result);
        try { fs::remove_all(tempDir); } catch (...) {}
        return;
    }

    // 验证签名
    DWORD dwEncoding, dwContentType, dwFormatType;
    HCERTSTORE hStore = nullptr;
    HCRYPTMSG hMsg = nullptr;
    BOOL sigOk = CryptQueryObject(CERT_QUERY_OBJECT_FILE, installer.c_str(),
        CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
        CERT_QUERY_FORMAT_FLAG_BINARY, 0,
        &dwEncoding, &dwContentType, &dwFormatType, &hStore, &hMsg, nullptr);

    if (sigOk && hStore) {
        // 使用 WinVerifyTrust 进行完整的 Authenticode 签名验证
        WINTRUST_FILE_INFO fileInfo{};
        fileInfo.cbStruct = sizeof(fileInfo);
        fileInfo.pcwszFilePath = installer.c_str();
        fileInfo.hFile = nullptr;
        fileInfo.pgKnownSubject = nullptr;

        GUID policyGUID = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        WINTRUST_DATA trustData{};
        trustData.cbStruct = sizeof(trustData);
        trustData.dwUIChoice = WTD_UI_NONE;
        trustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
        trustData.dwUnionChoice = WTD_CHOICE_FILE;
        trustData.pFile = &fileInfo;
        trustData.dwStateAction = WTD_STATEACTION_VERIFY;
        trustData.dwProvFlags = WTD_SAFER_FLAG;

        LONG trustStatus = WinVerifyTrust(nullptr, &policyGUID, &trustData);

        // 清理 WinVerifyTrust 状态
        trustData.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(nullptr, &policyGUID, &trustData);

        if (trustStatus != ERROR_SUCCESS) {
            std::stringstream hexErr;
            hexErr << std::hex << trustStatus;
            json result;
            result["success"] = false;
            result["error"] = "安装程序 Authenticode 签名校验失败 (0x" + hexErr.str() + ")，已拒绝执行";
            sendFn("vbcable-install-result", result);
            CertCloseStore(hStore, 0);
            if (hMsg) CryptMsgClose(hMsg);
            try { fs::remove_all(tempDir); } catch (...) {}
            return;
        }

        // 额外检查证书主题是否包含 VB-Audio
        PCCERT_CONTEXT pCert = CertEnumCertificatesInStore(hStore, nullptr);
        if (pCert) {
            char subjectName[512]{};
            CertNameToStrA(pCert->dwCertEncodingType, &pCert->pCertInfo->Subject,
                          CERT_X500_NAME_STR, subjectName, sizeof(subjectName));
            std::string subject(subjectName);

            bool validSigner = (subject.find("VB-Audio") != std::string::npos ||
                               subject.find("BUREL VINCENT") != std::string::npos);
            CertFreeCertificateContext(pCert);

            if (!validSigner) {
                json result;
                result["success"] = false;
                result["error"] = "安装程序签名校验失败：发布者非 VB-Audio，已拒绝执行";
                sendFn("vbcable-install-result", result);
                CertCloseStore(hStore, 0);
                if (hMsg) CryptMsgClose(hMsg);
                try { fs::remove_all(tempDir); } catch (...) {}
                return;
            }
        }
        CertCloseStore(hStore, 0);
        if (hMsg) CryptMsgClose(hMsg);
    } else {
        json result;
        result["success"] = false;
        result["error"] = "安装程序未经数字签名，已拒绝执行";
        sendFn("vbcable-install-result", result);
        try { fs::remove_all(tempDir); } catch (...) {}
        return;
    }

    // 执行安装
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = installer.c_str();
    sei.lpParameters = L"-i -h";
    sei.lpDirectory = installer.parent_path().c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        json result;
        result["success"] = false;
        if (err == ERROR_CANCELLED) {
            result["error"] = "安装已取消（UAC 提权被拒绝），请点击「是」以允许安装驱动";
        } else {
            result["error"] = "安装进程启动失败，错误码: " + std::to_string(err);
        }
        sendFn("vbcable-install-result", result);
        try { fs::remove_all(tempDir); } catch (...) {}
        return;
    }

    progress["status"] = "installing";
    progress["percent"] = 75;
    sendFn("vbcable-install-progress", progress);

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);

    // 轮询设备
    progress["status"] = "detecting";
    progress["percent"] = 88;
    sendFn("vbcable-install-progress", progress);

    std::string nowName = PollForDevice(30);

    if (!nowName.empty()) {
        SaveOemInf();
        json result;
        result["success"] = true;
        result["deviceName"] = nowName;
        sendFn("vbcable-install-result", result);
    } else if (exitCode == 0) {
        SaveOemInf();
        json result;
        result["success"] = true;
        result["deviceName"] = "VB-Audio Virtual Cable";
        result["note"] = "驱动已安装，如应用内未显示设备请重启系统后再试";
        sendFn("vbcable-install-result", result);
    } else {
        json result;
        result["success"] = false;
        result["error"] = "安装程序退出码 " + std::to_string(exitCode) + "，驱动未检测到。\n"
                          "请以管理员身份重新运行，或手动运行安装程序一次。";
        sendFn("vbcable-install-result", result);
    }

    // 清理临时文件
    try { fs::remove_all(tempDir); } catch (...) {}
}

// ── Uninstall ────────────────────────────────────────

void Uninstall(SendCallback sendFn) {
    json progress;
    progress["status"] = "uninstalling";
    progress["percent"] = 30;
    sendFn("vbcable-uninstall-progress", progress);

    // 读取标记文件
    std::string oemInf;
    auto flagPath = GetFlagFilePath();
    if (fs::exists(flagPath)) {
        std::ifstream ifs(flagPath);
        std::getline(ifs, oemInf);
    }

    // 标记文件缺失则扫描
    if (oemInf.empty()) {
        auto output = RunCommand(L"pnputil.exe /enum-drivers");
        oemInf = FindOemInf(output);
    }

    if (oemInf.empty()) {
        json result;
        result["success"] = false;
        result["error"] = "VB-Audio Virtual Cable driver not found, may have been manually removed";
        sendFn("vbcable-uninstall-result", result);
        return;
    }

    // 用 runas 运行 pnputil /delete-driver
    std::wstring args = L"/delete-driver " + Utf8ToWide(oemInf) + L" /uninstall /force";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"pnputil.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        json result;
        result["success"] = false;
        result["error"] = (err == ERROR_CANCELLED) ? "Uninstall cancelled (UAC elevation denied)" : "Failed to start uninstall process";
        sendFn("vbcable-uninstall-result", result);
        return;
    }

    WaitForSingleObject(sei.hProcess, 30000);
    DWORD exitCode = 1;
    GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);

    if (exitCode == 0) {
        try { fs::remove(flagPath); } catch (...) {}

        progress["status"] = "verifying";
        progress["percent"] = 75;
        sendFn("vbcable-uninstall-progress", progress);

        auto remaining = PollForDevice(10, true);
        if (remaining.empty()) {
            json result;
            result["success"] = true;
            sendFn("vbcable-uninstall-result", result);
        } else {
            json result;
            result["success"] = true;
            result["note"] = "Driver removed from driver store, reboot required to fully uninstall";
            sendFn("vbcable-uninstall-result", result);
        }
    } else {
        json result;
        result["success"] = false;
        result["error"] = "pnputil exit code " + std::to_string(exitCode) + ", uninstall may be incomplete";
        sendFn("vbcable-uninstall-result", result);
    }
}

// ── Fire-and-forget 卸载 ─────────────────────────────

void TryUninstallFireAndForget() {
    auto flagPath = GetFlagFilePath();
    if (!fs::exists(flagPath)) return;

    try {
        std::ifstream ifs(flagPath);
        std::string oemInf;
        std::getline(ifs, oemInf);
        ifs.close();
        if (oemInf.empty()) return;

        std::wstring args = L"/delete-driver " + Utf8ToWide(oemInf) + L" /uninstall /force";

        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";
        sei.lpFile = L"pnputil.exe";
        sei.lpParameters = args.c_str();
        sei.nShow = SW_HIDE;
        ShellExecuteExW(&sei);
    } catch (...) {}
}

} // namespace VBCable
} // namespace MDShare
