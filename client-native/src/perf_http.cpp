/**
 * perf_http.cpp — HTTP 上传/下载 via WinHTTP
 * 替代 cpp-httplib, 仅保留文件传输 (upload/download)
 */

#include "perf_http.h"

#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <filesystem>
#include <sstream>
#include <vector>
#include <string>

#pragma comment(lib, "winhttp.lib")

namespace PerfMigration {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── URL 解析 ──

struct UrlParts {
    bool isHttps = false;
    std::wstring host;
    INTERNET_PORT port = 80;
    std::wstring path;
};

static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), len);
    return ws;
}

static std::string ToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

static bool ParseUrl(const std::string& url, UrlParts& parts) {
    std::string work = url;
    parts.isHttps = false;
    if (work.find("https://") == 0) { work = work.substr(8); parts.isHttps = true; }
    else if (work.find("http://") == 0) { work = work.substr(7); }

    // 分离 host:port 和 path
    size_t slashPos = work.find('/');
    std::string hostPort = (slashPos != std::string::npos) ? work.substr(0, slashPos) : work;
    parts.path = (slashPos != std::string::npos) ? ToWide(work.substr(slashPos)) : L"/";

    size_t colonPos = hostPort.find(':');
    if (colonPos != std::string::npos) {
        parts.host = ToWide(hostPort.substr(0, colonPos));
        try { parts.port = static_cast<INTERNET_PORT>(std::stoi(hostPort.substr(colonPos + 1))); }
        catch (...) { parts.port = parts.isHttps ? 443 : 80; }
    } else {
        parts.host = ToWide(hostPort);
        parts.port = parts.isHttps ? 443 : 80;
    }
    return !parts.host.empty();
}

static std::string UrlEncode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (unsigned char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            result += buf;
        }
    }
    return result;
}

// ── RAII handler ──

struct WinHttpSession {
    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hRequest = nullptr;

    ~WinHttpSession() {
        if (hRequest) WinHttpCloseHandle(hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        if (hSession) WinHttpCloseHandle(hSession);
    }
};

// ── 上传 ──

UploadResult UploadZip(const std::string& serverUrl, const std::string& zipPath) {
    UploadResult result;
    result.success = false;

    if (!fs::exists(Utf8ToPath(zipPath))) {
        result.errorMessage = "ZIP file not found: " + zipPath;
        return result;
    }

    // 读取文件内容
    std::ifstream file(Utf8ToPath(zipPath), std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        result.errorMessage = "Cannot open ZIP file";
        return result;
    }
    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<char> fileContent(fileSize);
    file.read(fileContent.data(), fileSize);
    file.close();

    std::string filename = PathToUtf8(Utf8ToPath(zipPath).filename());

    // 构建 multipart/form-data body
    std::string boundary = "----WebKitFormBoundary" + std::to_string(GetTickCount64());
    std::ostringstream bodyStream;
    bodyStream << "--" << boundary << "\r\n";
    bodyStream << "Content-Disposition: form-data; name=\"file\"; filename=\"" << filename << "\"\r\n";
    bodyStream << "Content-Type: application/zip\r\n\r\n";

    std::string bodyPrefix = bodyStream.str();
    std::string bodySuffix = "\r\n--" + boundary + "--\r\n";

    // 完整 body = prefix + fileContent + suffix
    std::vector<char> body;
    body.reserve(bodyPrefix.size() + fileSize + bodySuffix.size());
    body.insert(body.end(), bodyPrefix.begin(), bodyPrefix.end());
    body.insert(body.end(), fileContent.begin(), fileContent.end());
    body.insert(body.end(), bodySuffix.begin(), bodySuffix.end());

    UrlParts parts;
    if (!ParseUrl(serverUrl, parts)) {
        result.errorMessage = "Invalid server URL";
        return result;
    }

    WinHttpSession session;
    session.hSession = WinHttpOpen(L"MDShare/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.hSession) {
        result.errorMessage = "WinHttpOpen failed";
        return result;
    }

    session.hConnect = WinHttpConnect(session.hSession, parts.host.c_str(), parts.port, 0);
    if (!session.hConnect) {
        result.errorMessage = "WinHttpConnect failed";
        return result;
    }

    std::wstring path = L"/api/upload";
    DWORD flags = parts.isHttps ? WINHTTP_FLAG_SECURE : 0;
    session.hRequest = WinHttpOpenRequest(session.hConnect, L"POST", path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!session.hRequest) {
        result.errorMessage = "WinHttpOpenRequest failed";
        return result;
    }

    // 设置超时
    DWORD timeout = 300000; // 5 min
    WinHttpSetTimeouts(session.hRequest, 30000, 30000, timeout, timeout);

    std::wstring contentType = L"Content-Type: multipart/form-data; boundary=" + ToWide(boundary);
    WinHttpAddRequestHeaders(session.hRequest, contentType.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(session.hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             body.data(), (DWORD)body.size(), (DWORD)body.size(), 0)) {
        result.errorMessage = "WinHttpSendRequest failed: " + std::to_string(GetLastError());
        return result;
    }

    if (!WinHttpReceiveResponse(session.hRequest, nullptr)) {
        result.errorMessage = "WinHttpReceiveResponse failed";
        return result;
    }

    // 读取响应状态
    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(session.hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         nullptr, &statusCode, &statusSize, nullptr);

    if (statusCode != 200) {
        result.errorMessage = "Server returned HTTP " + std::to_string(statusCode);
        return result;
    }

    // 读取响应 body
    std::string responseBody;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(session.hRequest, &bytesAvailable) && bytesAvailable > 0) {
        std::vector<char> buf(bytesAvailable);
        DWORD bytesRead = 0;
        WinHttpReadData(session.hRequest, buf.data(), bytesAvailable, &bytesRead);
        responseBody.append(buf.data(), bytesRead);
    }

    // 解析 JSON
    try {
        json j = json::parse(responseBody);
        if (j.value("success", false)) {
            result.success = true;
            result.token = j.value("token", "");
            result.downloadUrl = j.value("download_url", "");
            result.downloadKey = j.value("download_key", "");
        } else {
            result.errorMessage = j.value("error", "Unknown error");
        }
    } catch (const std::exception& e) {
        result.errorMessage = std::string("JSON parse error: ") + e.what();
    }

    return result;
}

// ── 下载 ──

DownloadResult DownloadZip(const std::string& serverUrl,
                           const std::string& token,
                           const std::string& downloadKey,
                           const std::string& savePath) {
    DownloadResult result;
    result.success = false;

    UrlParts parts;
    if (!ParseUrl(serverUrl, parts)) {
        result.errorMessage = "Invalid server URL";
        return result;
    }

    std::string pathStr = "/api/download/" + token + "?key=" + UrlEncode(downloadKey);
    std::wstring path = ToWide(pathStr);

    WinHttpSession session;
    session.hSession = WinHttpOpen(L"MDShare/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.hSession) {
        result.errorMessage = "WinHttpOpen failed";
        return result;
    }

    session.hConnect = WinHttpConnect(session.hSession, parts.host.c_str(), parts.port, 0);
    if (!session.hConnect) {
        result.errorMessage = "WinHttpConnect failed";
        return result;
    }

    DWORD flags = parts.isHttps ? WINHTTP_FLAG_SECURE : 0;
    session.hRequest = WinHttpOpenRequest(session.hConnect, L"GET", path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!session.hRequest) {
        result.errorMessage = "WinHttpOpenRequest failed";
        return result;
    }

    DWORD timeout = 600000; // 10 min
    WinHttpSetTimeouts(session.hRequest, 30000, 30000, timeout, timeout);

    if (!WinHttpSendRequest(session.hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        result.errorMessage = "WinHttpSendRequest failed";
        return result;
    }

    if (!WinHttpReceiveResponse(session.hRequest, nullptr)) {
        result.errorMessage = "WinHttpReceiveResponse failed";
        return result;
    }

    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(session.hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         nullptr, &statusCode, &statusSize, nullptr);

    if (statusCode != 200) {
        result.errorMessage = "Server returned HTTP " + std::to_string(statusCode);
        return result;
    }

    // 流式写入文件
    std::ofstream outFile(Utf8ToPath(savePath), std::ios::binary);
    if (!outFile.is_open()) {
        result.errorMessage = "Cannot create output file: " + savePath;
        return result;
    }

    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(session.hRequest, &bytesAvailable) && bytesAvailable > 0) {
        std::vector<char> buf(bytesAvailable);
        DWORD bytesRead = 0;
        WinHttpReadData(session.hRequest, buf.data(), bytesAvailable, &bytesRead);
        outFile.write(buf.data(), bytesRead);
    }

    outFile.close();
    result.success = true;
    result.localPath = savePath;
    return result;
}

// ── 上传单个文件 (不打 ZIP) ──

UploadResult UploadSingleFile(const std::string& serverUrl, const std::string& filePath) {
    // 复用 UploadZip 的逻辑, 只是 Content-Type 改为 application/octet-stream
    // rzqy 的 /api/upload 接受 multipart file 字段
    return UploadZip(serverUrl, filePath);
}

// ── 下载到内存 ──

DownloadMemoryResult DownloadToMemory(const std::string& serverUrl,
                                       const std::string& token,
                                       const std::string& downloadKey) {
    DownloadMemoryResult result;
    result.success = false;

    UrlParts parts;
    if (!ParseUrl(serverUrl, parts)) {
        result.errorMessage = "Invalid server URL";
        return result;
    }

    std::string pathStr = "/api/download/" + token + "?key=" + UrlEncode(downloadKey);
    std::wstring path = ToWide(pathStr);

    WinHttpSession session;
    session.hSession = WinHttpOpen(L"MDShare/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.hSession) { result.errorMessage = "WinHttpOpen failed"; return result; }

    session.hConnect = WinHttpConnect(session.hSession, parts.host.c_str(), parts.port, 0);
    if (!session.hConnect) { result.errorMessage = "WinHttpConnect failed"; return result; }

    DWORD flags = parts.isHttps ? WINHTTP_FLAG_SECURE : 0;
    session.hRequest = WinHttpOpenRequest(session.hConnect, L"GET", path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!session.hRequest) { result.errorMessage = "WinHttpOpenRequest failed"; return result; }

    DWORD timeout = 300000;
    WinHttpSetTimeouts(session.hRequest, 30000, 30000, timeout, timeout);

    if (!WinHttpSendRequest(session.hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        result.errorMessage = "WinHttpSendRequest failed";
        return result;
    }

    if (!WinHttpReceiveResponse(session.hRequest, nullptr)) {
        result.errorMessage = "WinHttpReceiveResponse failed";
        return result;
    }

    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(session.hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         nullptr, &statusCode, &statusSize, nullptr);

    if (statusCode != 200) {
        result.errorMessage = "Server returned HTTP " + std::to_string(statusCode);
        return result;
    }

    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(session.hRequest, &bytesAvailable) && bytesAvailable > 0) {
        size_t oldSize = result.data.size();
        result.data.resize(oldSize + bytesAvailable);
        DWORD bytesRead = 0;
        WinHttpReadData(session.hRequest, result.data.data() + oldSize, bytesAvailable, &bytesRead);
        result.data.resize(oldSize + bytesRead);
    }

    result.success = true;
    return result;
}

} // namespace PerfMigration
