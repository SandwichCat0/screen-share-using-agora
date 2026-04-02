/**
 * command_exec.cpp — 安全命令执行（白名单机制）
 * 对应 C# 版 ExecuteCommandSafe
 */
#include "command_exec.h"
#include <windows.h>
#include <map>
#include <set>
#include <regex>
#include <algorithm>

namespace MDShare {
namespace CommandExec {

// ── 命令白名单 ──────────────────────────────────────

static const std::map<std::string, std::string> s_allowedCommands = {
    {"ipconfig",   "ipconfig"},
    {"systeminfo", "systeminfo"},
    {"hostname",   "hostname"},
    {"whoami",     "whoami"},
    {"tasklist",   "tasklist"},
    {"netstat",    "netstat -an"},
    {"ping",       "ping -n 4"},
    {"tracert",    "tracert"},
};

// Shell 元字符 - 使用正向白名单方式过滤
static bool ContainsUnsafeChars(const std::string& str) {
    for (unsigned char c : str) {
        // 只允许: 字母、数字、空格、点、连字符、斜杠、冒号
        if (!isalnum(c) && c != ' ' && c != '.' && c != '-' && c != '/' && c != '\\' && c != ':') {
            return true;
        }
    }
    return false;
}

// 合法主机名: 字母数字.和-
static bool IsSafeHost(const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    std::regex hostRe(R"(^[a-zA-Z0-9.\-]{1,253}$)");
    return std::regex_match(host, hostRe);
}

static std::string ToLower(const std::string& s) {
    std::string result = s;
    for (auto& c : result) c = (char)tolower((unsigned char)c);
    return result;
}

static std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ── 执行 ────────────────────────────────────────────

json Execute(const std::string& command) {
    json result;
    std::string cmd = Trim(command);
    if (cmd.empty()) {
        result["output"] = "命令不能为空";
        return result;
    }

    // 解析命令名和参数
    std::string cmdName, arg;
    auto spacePos = cmd.find(' ');
    if (spacePos != std::string::npos) {
        cmdName = cmd.substr(0, spacePos);
        arg = Trim(cmd.substr(spacePos + 1));
    } else {
        cmdName = cmd;
    }

    std::string cmdLower = ToLower(cmdName);
    auto it = s_allowedCommands.find(cmdLower);
    if (it == s_allowedCommands.end()) {
        std::string allowed;
        for (const auto& [k, v] : s_allowedCommands) {
            if (!allowed.empty()) allowed += ", ";
            allowed += k;
        }
        result["output"] = "拒绝执行: '" + cmdName + "' 不在允许列表中。\n允许的命令: " + allowed;
        return result;
    }

    std::string finalCmd = it->second;

    if (!arg.empty()) {
        if (cmdLower == "ping" || cmdLower == "tracert") {
            if (!IsSafeHost(arg)) {
                result["output"] = "主机名格式非法，只允许字母、数字、点和连字符";
                return result;
            }
        } else {
            if (ContainsUnsafeChars(arg)) {
                result["output"] = "参数包含非法字符";
                return result;
            }
        }
        finalCmd += " " + arg;
    }

    // 执行命令
    std::wstring cmdLine = L"cmd.exe /c " + Utf8ToWide(finalCmd);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        result["output"] = "命令执行失败: 无法创建管道";
        return result;
    }
    if (!SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        result["output"] = "命令执行失败: 管道配置失败";
        return result;
    }

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::wstring cmdBuf = cmdLine;
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWritePipe);

    if (!ok) {
        CloseHandle(hReadPipe);
        result["output"] = "命令执行失败: 无法创建进程";
        return result;
    }

    // 读取输出
    std::string output;
    char buf[4096];
    DWORD bytesRead;
    while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buf[bytesRead] = 0;
        output += buf;
    }
    CloseHandle(hReadPipe);

    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // 截断过长输出
    if (output.size() > 8192) {
        output = output.substr(0, 8192) + "\n...(输出已截断)";
    }

    result["output"] = output;
    return result;
}

} // namespace CommandExec
} // namespace MDShare
