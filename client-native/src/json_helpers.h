#pragma once
/**
 * json_helpers.h — JSON helpers + string conversion
 * Uses nlohmann/json
 */
#include <windows.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>

namespace MDShare {

using json = nlohmann::json;

/** 构建 {type: ..., data: ...} 消息 */
inline std::string MakeMessage(const std::string& type, const json& data = nullptr) {
    json msg;
    msg["type"] = type;
    msg["data"] = data;
    return msg.dump();
}

/** 宽字符串转 UTF-8 */
inline std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), result.data(), size, nullptr, nullptr);
    return result;
}

/** UTF-8 转宽字符串 */
inline std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), size);
    return result;
}

} // namespace MDShare
