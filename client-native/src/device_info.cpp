/**
 * device_info.cpp — Device info collection
 * Uses WMI (COM) for hardware info queries
 */
#include "device_info.h"

// Must include winsock2 before windows.h when WIN32_LEAN_AND_MEAN is defined
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <iphlpapi.h>
#include <winternl.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <functional>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace MDShare {
namespace DeviceInfo {

// ── WMI 辅助 ────────────────────────────────────────

class WmiHelper {
public:
    WmiHelper() {
        HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IWbemLocator, (void**)&m_locator);
        if (FAILED(hr)) return;

        hr = m_locator->ConnectServer(
            _bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &m_service);
        if (FAILED(hr)) return;

        CoSetProxyBlanket(m_service, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                          RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        m_ok = true;
    }

    ~WmiHelper() {
        if (m_service) m_service->Release();
        if (m_locator) m_locator->Release();
    }

    bool IsOk() const { return m_ok; }

    /** 执行 WQL 查询并对每行回调 */
    void Query(const wchar_t* wql, std::function<void(IWbemClassObject*)> callback) {
        if (!m_ok) return;

        IEnumWbemClassObject* enumerator = nullptr;
        HRESULT hr = m_service->ExecQuery(
            _bstr_t(L"WQL"), _bstr_t(wql),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &enumerator);
        if (FAILED(hr)) return;

        IWbemClassObject* obj = nullptr;
        ULONG count = 0;
        while (enumerator->Next(WBEM_INFINITE, 1, &obj, &count) == S_OK) {
            callback(obj);
            obj->Release();
        }
        enumerator->Release();
    }

    /** 获取字符串属性 */
    static std::string GetString(IWbemClassObject* obj, const wchar_t* prop) {
        VARIANT vtProp;
        HRESULT hr = obj->Get(prop, 0, &vtProp, nullptr, nullptr);
        if (SUCCEEDED(hr) && vtProp.vt == VT_BSTR) {
            std::wstring ws(vtProp.bstrVal);
            VariantClear(&vtProp);
            return WideToUtf8(ws);
        }
        VariantClear(&vtProp);
        return "";
    }

    /** 获取 int64 属性 */
    static int64_t GetInt64(IWbemClassObject* obj, const wchar_t* prop) {
        VARIANT vtProp;
        HRESULT hr = obj->Get(prop, 0, &vtProp, nullptr, nullptr);
        if (FAILED(hr)) { VariantClear(&vtProp); return 0; }

        int64_t result = 0;
        if (vtProp.vt == VT_BSTR) {
            result = _wtoi64(vtProp.bstrVal);
        } else if (vtProp.vt == VT_I4) {
            result = vtProp.lVal;
        } else if (vtProp.vt == VT_UI4) {
            result = vtProp.ulVal;
        }
        VariantClear(&vtProp);
        return result;
    }

private:
    IWbemLocator*  m_locator = nullptr;
    IWbemServices* m_service = nullptr;
    bool           m_ok      = false;
};

// ── 各项采集函数 ─────────────────────────────────────

static std::string GetOSName(WmiHelper& wmi) {
    std::string result;
    wmi.Query(L"SELECT Caption FROM Win32_OperatingSystem",
        [&](IWbemClassObject* obj) {
            auto v = WmiHelper::GetString(obj, L"Caption");
            if (!v.empty()) result = v;
        });
    if (result.empty()) {
        DWORD major = 0, minor = 0;
        // 回退
        result = "Windows";
    }
    return result;
}

static std::string GetCPUName(WmiHelper& wmi) {
    std::string result;
    wmi.Query(L"SELECT Name FROM Win32_Processor",
        [&](IWbemClassObject* obj) {
            auto v = WmiHelper::GetString(obj, L"Name");
            if (!v.empty()) result = v;
        });
    if (result.empty()) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        result = std::to_string(si.dwNumberOfProcessors) + " Core CPU";
    }
    return result;
}

static std::string GetTotalMemory(WmiHelper& wmi) {
    std::string result = "--";
    wmi.Query(L"SELECT TotalPhysicalMemory FROM Win32_ComputerSystem",
        [&](IWbemClassObject* obj) {
            auto bytes = WmiHelper::GetInt64(obj, L"TotalPhysicalMemory");
            if (bytes > 0) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
                result = buf;
            }
        });
    return result;
}

static std::string GetGPUName(WmiHelper& wmi) {
    std::vector<std::string> gpus;
    wmi.Query(L"SELECT Name FROM Win32_VideoController",
        [&](IWbemClassObject* obj) {
            auto name = WmiHelper::GetString(obj, L"Name");
            if (!name.empty()) gpus.push_back(name);
        });
    if (gpus.empty()) return "--";
    std::string result;
    for (size_t i = 0; i < gpus.size(); i++) {
        if (i > 0) result += "\n";
        result += gpus[i];
    }
    return result;
}

static std::string GetStorageInfo(WmiHelper& wmi) {
    std::vector<std::string> disks;
    wmi.Query(L"SELECT Model, Size FROM Win32_DiskDrive",
        [&](IWbemClassObject* obj) {
            auto model = WmiHelper::GetString(obj, L"Model");
            auto size = WmiHelper::GetInt64(obj, L"Size");
            if (model.empty()) model = "Unknown";
            char buf[256];
            snprintf(buf, sizeof(buf), "%s (%.1f GB)", model.c_str(), size / (1024.0 * 1024.0 * 1024.0));
            disks.push_back(buf);
        });
    if (disks.empty()) return "--";
    std::string result;
    for (size_t i = 0; i < disks.size(); i++) {
        if (i > 0) result += "\n";
        result += disks[i];
    }
    return result;
}

static json GetNetworkAdapters() {
    json adapters = json::array();
    ULONG bufSize = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, nullptr, &bufSize);
    if (bufSize == 0) return adapters;

    auto buffer = std::make_unique<char[]>(bufSize);
    auto addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.get());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, addrs, &bufSize) != NO_ERROR)
        return adapters;

    for (auto curr = addrs; curr; curr = curr->Next) {
        if (curr->OperStatus != IfOperStatusUp) continue;
        if (curr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        json adapter;
        adapter["name"] = WideToUtf8(curr->Description);

        // 类型
        switch (curr->IfType) {
            case IF_TYPE_ETHERNET_CSMACD: adapter["type"] = "Ethernet"; break;
            case IF_TYPE_IEEE80211:       adapter["type"] = "Wireless"; break;
            default:                      adapter["type"] = "Other"; break;
        }

        // MAC
        if (curr->PhysicalAddressLength > 0) {
            char mac[32]{};
            snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                curr->PhysicalAddress[0], curr->PhysicalAddress[1],
                curr->PhysicalAddress[2], curr->PhysicalAddress[3],
                curr->PhysicalAddress[4], curr->PhysicalAddress[5]);
            adapter["mac"] = mac;
        } else {
            adapter["mac"] = "";
        }

        adapters.push_back(adapter);
    }
    return adapters;
}

static std::string GetLocalIP() {
    ULONG bufSize = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, nullptr, &bufSize);
    if (bufSize == 0) return "127.0.0.1";

    auto buffer = std::make_unique<char[]>(bufSize);
    auto addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.get());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, addrs, &bufSize) != NO_ERROR)
        return "127.0.0.1";

    for (auto curr = addrs; curr; curr = curr->Next) {
        if (curr->OperStatus != IfOperStatusUp) continue;
        if (curr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        for (auto ua = curr->FirstUnicastAddress; ua; ua = ua->Next) {
            auto sa = (struct sockaddr_in*)ua->Address.lpSockaddr;
            if (sa->sin_family == AF_INET) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                return ip;
            }
        }
    }
    return "127.0.0.1";
}

static std::string GetHardwareFingerprint(WmiHelper& wmi) {
    // 采集 4 项硬件特征生成不可伪造的指纹
    std::string biosUuid, boardSerial, diskSerial, processorId;

    wmi.Query(L"SELECT UUID FROM Win32_ComputerSystemProduct",
        [&](IWbemClassObject* obj) {
            auto v = WmiHelper::GetString(obj, L"UUID");
            if (!v.empty() && v != "FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF") biosUuid = v;
        });

    wmi.Query(L"SELECT SerialNumber FROM Win32_BaseBoard",
        [&](IWbemClassObject* obj) {
            auto v = WmiHelper::GetString(obj, L"SerialNumber");
            if (!v.empty() && v != "To Be Filled By O.E.M." && v != "Default string") boardSerial = v;
        });

    wmi.Query(L"SELECT SerialNumber FROM Win32_DiskDrive WHERE Index=0",
        [&](IWbemClassObject* obj) {
            auto v = WmiHelper::GetString(obj, L"SerialNumber");
            if (!v.empty()) diskSerial = v;
        });

    wmi.Query(L"SELECT ProcessorId FROM Win32_Processor",
        [&](IWbemClassObject* obj) {
            auto v = WmiHelper::GetString(obj, L"ProcessorId");
            if (!v.empty()) processorId = v;
        });

    // 拼接: "BIOS:{uuid}|BOARD:{serial}|DISK:{serial}|CPU:{id}"
    std::string material = "BIOS:" + biosUuid + "|BOARD:" + boardSerial
                         + "|DISK:" + diskSerial + "|CPU:" + processorId;

    // SHA-256 哈希
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    UCHAR hashResult[32] = {};

    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        if (BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0))) {
            BCryptHashData(hHash, (PUCHAR)material.data(), (ULONG)material.size(), 0);
            BCryptFinishHash(hHash, hashResult, sizeof(hashResult), 0);
            BCryptDestroyHash(hHash);
        }
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }

    // 转为 64 字符 hex
    char hex[65] = {};
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", hashResult[i]);
    }
    return hex;
}

static std::string GetComputerName_() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(buf, &size);
    return WideToUtf8(buf);
}

static std::string GetUserName_() {
    wchar_t buf[256]{};
    DWORD size = 256;
    GetUserNameW(buf, &size);
    return WideToUtf8(buf);
}

static std::string GetOSVersion() {
    // 使用 RtlGetVersion 获取真实版本号
    typedef NTSTATUS(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
        if (fn) {
            RTL_OSVERSIONINFOW osvi{};
            osvi.dwOSVersionInfoSize = sizeof(osvi);
            fn(&osvi);
            char buf[64];
            snprintf(buf, sizeof(buf), "%lu.%lu.%lu",
                osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber);
            return buf;
        }
    }
    return "Unknown";
}

// ── 主收集函数 ───────────────────────────────────────

json Collect() {
    WmiHelper wmi;

    std::string computerName = GetComputerName_();

    json info;
    info["computerName"]    = computerName;
    info["userName"]        = GetUserName_();
    info["os"]              = GetOSName(wmi);
    info["osVersion"]       = GetOSVersion();
    info["cpu"]             = GetCPUName(wmi);
    info["memory"]          = GetTotalMemory(wmi);
    info["gpu"]             = GetGPUName(wmi);
    info["storage"]         = GetStorageInfo(wmi);
    info["networkAdapters"] = GetNetworkAdapters();
    info["clientVersion"]   = "1.0.0";
    info["permissionGroup"] = (GetUserName_() == "SYSTEM") ? "SYSTEM" : "User";

    // CID: hash of computer name
    std::hash<std::string> hasher;
    char cidBuf[32];
    snprintf(cidBuf, sizeof(cidBuf), "%zX", hasher(computerName));
    info["cid"] = cidBuf;

    info["machineGuid"]  = GetHardwareFingerprint(wmi);
    info["ipAddress"]    = GetLocalIP();
    info["ipLocation"]   = "";

    return info;
}

} // namespace DeviceInfo
} // namespace MDShare
