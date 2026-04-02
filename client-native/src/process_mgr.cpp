/**
 * process_mgr.cpp — 进程管理
 * 对应 C# 版 SendProcessList / KillProcessSafe
 */
#include "process_mgr.h"
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <vector>
#include <set>
#include <string>
#include <filesystem>

#pragma comment(lib, "psapi.lib")

namespace fs = std::filesystem;

namespace MDShare {
namespace ProcessMgr {

// ── 受保护进程名（不允许结束） ───────────────────────

static const std::set<std::wstring, std::less<>> s_protectedNames = {
    L"System", L"svchost", L"csrss", L"wininit", L"services",
    L"lsass", L"smss", L"winlogon", L"explorer", L"dwm"
};

static bool IsProtected(const std::wstring& name) {
    // 不区分大小写比较
    for (const auto& p : s_protectedNames) {
        if (_wcsicmp(name.c_str(), p.c_str()) == 0) return true;
    }
    return false;
}

static std::string FormatBytes(SIZE_T bytes) {
    const char* sizes[] = { "B", "KB", "MB", "GB" };
    double len = (double)bytes;
    int order = 0;
    while (len >= 1024.0 && order < 3) {
        order++;
        len /= 1024.0;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f %s", len, sizes[order]);
    return buf;
}

// ── 进程列表 ─────────────────────────────────────────

struct ProcEntry {
    std::string name;
    DWORD       pid;
    SIZE_T      memory;
};

json GetProcessList() {
    json result = json::array();

    std::vector<ProcEntry> entries;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcEntry entry;
            entry.name = WideToUtf8(pe.szExeFile);
            // 去掉 .exe 后缀
            if (entry.name.size() > 4) {
                auto ext = entry.name.substr(entry.name.size() - 4);
                for (auto& c : ext) c = (char)tolower((unsigned char)c);
                if (ext == ".exe") entry.name = entry.name.substr(0, entry.name.size() - 4);
            }
            entry.pid = pe.th32ProcessID;
            entry.memory = 0;

            // 尝试获取内存信息
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
            if (hProc) {
                PROCESS_MEMORY_COUNTERS pmc{};
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                    entry.memory = pmc.WorkingSetSize;
                }
                CloseHandle(hProc);
            }

            entries.push_back(entry);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    // 按内存降序排列
    std::sort(entries.begin(), entries.end(),
        [](const ProcEntry& a, const ProcEntry& b) { return a.memory > b.memory; });

    // 最多200个
    size_t count = std::min<size_t>(entries.size(), 200);
    for (size_t i = 0; i < count; i++) {
        json proc;
        proc["name"]   = entries[i].name;
        proc["pid"]    = entries[i].pid;
        proc["memory"] = entries[i].memory > 0 ? FormatBytes(entries[i].memory) : "--";
        proc["cpu"]    = "--";
        result.push_back(proc);
    }

    return result;
}

// ── 安全结束进程 ─────────────────────────────────────

json KillProcess(int pid) {
    // 获取进程名
    std::wstring procName;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if ((int)pe.th32ProcessID == pid) {
                    procName = pe.szExeFile;
                    // 去 .exe
                    if (procName.size() > 4 && _wcsicmp(procName.c_str() + procName.size() - 4, L".exe") == 0)
                        procName = procName.substr(0, procName.size() - 4);
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    if (procName.empty()) {
        json r;
        r["output"] = "进程不存在: PID " + std::to_string(pid);
        return r;
    }

    // 保护检查
    if (IsProtected(procName)) {
        json r;
        r["output"] = "拒绝结束系统进程: " + WideToUtf8(procName);
        return r;
    }

    // 结束进程
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!hProc) {
        json r;
        r["output"] = "无权限结束进程: " + WideToUtf8(procName) + " (PID: " + std::to_string(pid) + ")";
        return r;
    }

    // 防止 PID 复用 TOCTOU: 在 OpenProcess 后再次验证进程名
    {
        wchar_t exePath[MAX_PATH]{};
        DWORD exePathLen = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, exePath, &exePathLen)) {
            std::wstring exeName = fs::path(exePath).stem().wstring();
            if (_wcsicmp(exeName.c_str(), procName.c_str()) != 0) {
                CloseHandle(hProc);
                json r;
                r["output"] = "进程已不存在 (PID 已被复用): " + std::to_string(pid);
                return r;
            }
        }
    }

    BOOL ok = TerminateProcess(hProc, 1);
    CloseHandle(hProc);

    if (ok) {
        json r;
        json activity;
        activity["text"]      = "已结束进程: " + WideToUtf8(procName) + " (PID: " + std::to_string(pid) + ")";
        activity["icon"]      = "fa-skull";
        activity["iconColor"] = "text-red-400";
        activity["iconBg"]    = "bg-red-900/30";
        r["activity"] = activity;
        return r;
    } else {
        json r;
        r["output"] = "结束进程失败: " + WideToUtf8(procName);
        return r;
    }
}

} // namespace ProcessMgr
} // namespace MDShare
