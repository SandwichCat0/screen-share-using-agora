/**
 * perf_parser.cpp — PerfData protobuf header 解析 + zlib 解压
 * 移植自 日志迁移/receiver/PerfDataParser.cpp
 * 变更: zlib → miniz (API 兼容)
 */

#include "perf_parser.h"
#include "perf_proto.h"

#include <miniz.h>

#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <regex>
#include <cctype>

namespace PerfMigration {

// ── 构造/析构 ──

PerfDataParser::PerfDataParser(const std::string& filePath)
    : m_filePath(filePath) {}

PerfDataParser::~PerfDataParser() = default;

// ── 加载文件 ──

bool PerfDataParser::Load() {
    std::ifstream file(Utf8ToPath(m_filePath), std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        m_result.errorMessage = "Cannot open: " + m_filePath;
        return false;
    }

    m_result.fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    m_rawData.resize(m_result.fileSize);
    file.read(reinterpret_cast<char*>(m_rawData.data()), m_result.fileSize);
    file.close();

    return true;
}

// ── 清理字符串中的空字符 ──

static std::string CleanString(const std::string& s) {
    std::string result;
    for (char c : s) {
        if (c != '\0') result += c;
    }
    return result;
}

// ── 解析 Header ──

bool PerfDataParser::ParseHeader() {
    if (m_rawData.empty()) {
        m_result.errorMessage = "No data loaded";
        return false;
    }

    // 查找 zlib 压缩数据起始位置 (0x78 0x9C)
    int zlibStart = ProtobufParser::FindZlibStart(m_rawData);
    if (zlibStart < 0) {
        m_result.errorMessage = "Cannot find zlib header";
        return false;
    }

    // 分离 header 和压缩数据
    m_headerData.assign(m_rawData.begin(), m_rawData.begin() + zlibStart);
    m_compressedData.assign(m_rawData.begin() + zlibStart, m_rawData.end());
    m_result.header.headerSize = m_headerData.size();

    // 解析 protobuf 字段
    size_t pos = 0;
    int fieldCount = 0;

    while (pos < m_headerData.size()) {
        auto [tagValue, newPos] = ProtobufParser::ParseVarint(m_headerData, pos);
        if (newPos == pos) break;
        pos = newPos;

        int fieldNum = static_cast<int>(tagValue >> 3);
        int wireType = static_cast<int>(tagValue & 0x07);

        RawField field;
        field.fieldNumber = fieldNum;

        if (wireType == 0) {  // Varint
            auto [value, p2] = ProtobufParser::ParseVarint(m_headerData, pos);
            pos = p2;
            field.type = FieldType::Varint;
            field.numericValue = value;
            field.value = std::to_string(value);
            m_result.header.rawFields.push_back(field);
            fieldCount++;

        } else if (wireType == 2) {  // Length-delimited
            auto [length, p2] = ProtobufParser::ParseVarint(m_headerData, pos);
            pos = p2;

            if (pos + length <= m_headerData.size()) {
                std::vector<uint8_t> content(m_headerData.begin() + pos,
                                              m_headerData.begin() + pos + length);
                pos += static_cast<size_t>(length);

                // 尝试解析为 UTF-8 字符串
                bool isValidString = true;
                for (uint8_t b : content) {
                    if (b < 0x20 && b != 0x0A && b != 0x0D && b != 0x09 && b != 0x00) {
                        isValidString = false;
                        break;
                    }
                }

                if (isValidString && !content.empty()) {
                    field.type = FieldType::String;
                    field.value = std::string(content.begin(), content.end());
                    m_result.header.rawFields.push_back(field);
                    fieldCount++;
                } else {
                    // 尝试解析为嵌套 protobuf 消息
                    bool parsedAsMessage = false;
                    bool isFirstLevelNested = (fieldCount == 0);
                    size_t innerPos = 0;

                    while (innerPos < content.size()) {
                        auto [innerTag, ip2] = ProtobufParser::ParseVarint(content, innerPos);
                        if (ip2 == innerPos) break;
                        innerPos = ip2;

                        int innerFieldNum = static_cast<int>(innerTag >> 3);
                        int innerWireType = static_cast<int>(innerTag & 0x07);

                        RawField innerField;
                        innerField.fieldNumber = innerFieldNum;

                        if (innerWireType == 0) {  // Varint
                            auto [val, ip3] = ProtobufParser::ParseVarint(content, innerPos);
                            innerPos = ip3;
                            innerField.type = FieldType::Varint;
                            innerField.numericValue = val;
                            innerField.value = std::to_string(val);
                            m_result.header.rawFields.push_back(innerField);
                            fieldCount++;
                            parsedAsMessage = true;

                        } else if (innerWireType == 2) {  // String/bytes
                            auto [innerLen, ip3] = ProtobufParser::ParseVarint(content, innerPos);
                            innerPos = ip3;
                            if (innerPos + innerLen <= content.size()) {
                                std::string s(content.begin() + innerPos,
                                             content.begin() + innerPos + innerLen);
                                innerPos += static_cast<size_t>(innerLen);
                                innerField.type = FieldType::String;
                                innerField.value = s;
                                m_result.header.rawFields.push_back(innerField);
                                fieldCount++;
                                parsedAsMessage = true;
                            } else {
                                break;
                            }

                        } else if (innerWireType == 5) {  // Fixed32
                            if (innerPos + 4 <= content.size()) {
                                uint32_t value = *reinterpret_cast<const uint32_t*>(&content[innerPos]);
                                innerPos += 4;
                                innerField.type = FieldType::Fixed32;
                                innerField.numericValue = value;
                                innerField.value = std::to_string(value);
                                m_result.header.rawFields.push_back(innerField);
                                fieldCount++;
                                parsedAsMessage = true;
                            } else {
                                break;
                            }

                        } else if (innerWireType == 1) {  // Fixed64
                            if (innerPos + 8 <= content.size()) {
                                uint64_t value = *reinterpret_cast<const uint64_t*>(&content[innerPos]);
                                innerPos += 8;
                                innerField.type = FieldType::Fixed64;
                                innerField.numericValue = value;
                                innerField.value = std::to_string(value);
                                m_result.header.rawFields.push_back(innerField);
                                fieldCount++;
                                parsedAsMessage = true;
                            } else {
                                break;
                            }

                        } else {
                            break;
                        }
                    }

                    if (!parsedAsMessage) {
                        field.type = FieldType::Bytes;
                        std::stringstream ss;
                        for (uint8_t byte : content) {
                            ss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
                        }
                        field.value = ss.str();
                        m_result.header.rawFields.push_back(field);
                        fieldCount++;
                    } else if (isFirstLevelNested) {
                        m_result.header.hasOuterWrapper = true;
                        m_result.header.outerFieldNumber = fieldNum;
                    }
                }
            } else {
                break;
            }

        } else if (wireType == 5) {  // Fixed32
            if (pos + 4 <= m_headerData.size()) {
                uint32_t value = *reinterpret_cast<const uint32_t*>(&m_headerData[pos]);
                pos += 4;
                field.type = FieldType::Fixed32;
                field.numericValue = value;
                field.value = std::to_string(value);
                m_result.header.rawFields.push_back(field);
            } else {
                break;
            }

        } else if (wireType == 1) {  // Fixed64
            if (pos + 8 <= m_headerData.size()) {
                uint64_t value = *reinterpret_cast<const uint64_t*>(&m_headerData[pos]);
                pos += 8;
                field.type = FieldType::Fixed64;
                field.numericValue = value;
                field.value = std::to_string(value);
                m_result.header.rawFields.push_back(field);
            } else {
                break;
            }
        } else {
            pos++;  // 跳过未知 wireType
        }
    }

    ExtractKnownFields();
    ParseFixedHeaderFields();

    return true;
}

// ── 提取已知字段 ──

void PerfDataParser::ExtractKnownFields() {
    for (const auto& field : m_result.header.rawFields) {
        if (field.type != FieldType::String) continue;
        const std::string s = CleanString(field.value);
        if (s.empty()) continue;
        int fn = field.fieldNumber;

        switch (fn) {
            case 11: m_result.header.knownFields.osName = s; continue;
            case 13:
                if (m_result.header.knownFields.osVersion.empty())
                    m_result.header.knownFields.osVersion = s;
                continue;
            case 17: m_result.header.knownFields.architecture = s; continue;
            case 18: m_result.header.knownFields.gpuVendor = s; continue;
            case 19: m_result.header.knownFields.gpuModel = s; continue;
            case 20: m_result.header.knownFields.driverVersion = s; continue;
            case 30:
                if (m_result.header.knownFields.hardwareId1.empty())
                    m_result.header.knownFields.hardwareId1 = s;
                continue;
            case 32:
                if (m_result.header.knownFields.hardwareId2.empty())
                    m_result.header.knownFields.hardwareId2 = s;
                continue;
            case 23: case 39:
                if (m_result.header.knownFields.macAddress.empty())
                    m_result.header.knownFields.macAddress = s;
                continue;
            case 24: case 40:
                if (m_result.header.knownFields.cpuModel.empty())
                    m_result.header.knownFields.cpuModel = s;
                continue;
            case 9:
                m_result.header.knownFields.compression = s;
                continue;
        }

        // 后备: 内容匹配
        if (s.length() == 20 && std::all_of(s.begin(), s.end(), ::isdigit)) {
            if (m_result.header.knownFields.deviceId.empty())
                m_result.header.knownFields.deviceId = s;
        } else if (s.length() == 9 && std::all_of(s.begin(), s.end(), ::isdigit)) {
            if (m_result.header.knownFields.sessionId.empty())
                m_result.header.knownFields.sessionId = s;
        } else if (std::regex_match(s, std::regex(R"(^\d+\.\d+\.\d+\.\d+\.\d+$)"))) {
            if (m_result.header.knownFields.gameVersion.empty())
                m_result.header.knownFields.gameVersion = s;
        } else if (s.length() == 32 && std::all_of(s.begin(), s.end(),
                   [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
            if (m_result.header.knownFields.hardwareId1.empty())
                m_result.header.knownFields.hardwareId1 = s;
            else if (m_result.header.knownFields.hardwareId2.empty())
                m_result.header.knownFields.hardwareId2 = s;
        }
    }
}

void PerfDataParser::ParseFixedHeaderFields() {
    if (m_headerData.size() < 200) return;

    auto& known = m_result.header.knownFields;

    // deviceId: offset 0x05, 20 bytes
    if (m_headerData.size() >= 25) {
        std::string deviceId(m_headerData.begin() + 5, m_headerData.begin() + 25);
        if (deviceId.length() == 20 && std::all_of(deviceId.begin(), deviceId.end(), ::isdigit))
            known.deviceId = deviceId;
    }
    // sessionId: offset 0x1A, 9 bytes
    if (m_headerData.size() >= 0x1A + 9) {
        std::string sessionId(m_headerData.begin() + 0x1A, m_headerData.begin() + 0x1A + 9);
        if (std::all_of(sessionId.begin(), sessionId.end(), ::isdigit))
            known.sessionId = sessionId;
    }

    std::string headerStr(m_headerData.begin(), m_headerData.end());

    // 游戏版本
    std::regex verRegex(R"(\d+\.\d+\.\d+\.\d+\.\d+)");
    std::smatch match;
    if (std::regex_search(headerStr, match, verRegex))
        known.gameVersion = match.str();

    if (headerStr.find("Windows") != std::string::npos)
        known.osName = "Windows";

    std::regex osRegex(R"(10\.0\.\d+)");
    if (std::regex_search(headerStr, match, osRegex))
        known.osVersion = match.str();

    if (headerStr.find("x64") != std::string::npos)
        known.architecture = "x64";
    else if (headerStr.find("x86") != std::string::npos)
        known.architecture = "x86";

    // GPU vendor (后备)
    if (known.gpuVendor.empty()) {
        if (headerStr.find("AMD") != std::string::npos && headerStr.find("Ryzen") == std::string::npos)
            known.gpuVendor = "AMD";
        else if (headerStr.find("NVIDIA") != std::string::npos)
            known.gpuVendor = "NVIDIA";
        else if (headerStr.find("Intel") != std::string::npos)
            known.gpuVendor = "Intel";
    }

    // GPU model
    std::vector<std::regex> gpuPatterns = {
        std::regex(R"(AMD Radeon[^\x00\xa2]+)"),
        std::regex(R"(NVIDIA GeForce[^\x00\xa2]+)"),
        std::regex(R"(Intel[^\x00\xa2]+)")
    };
    for (const auto& pattern : gpuPatterns) {
        if (std::regex_search(headerStr, match, pattern)) {
            std::string gpuModel = match.str();
            gpuModel.erase(std::find_if(gpuModel.rbegin(), gpuModel.rend(),
                [](unsigned char ch) { return ch >= 32 && ch < 127; }).base(), gpuModel.end());
            if (gpuModel.length() > 5) { known.gpuModel = gpuModel; break; }
        }
    }

    // Driver version
    std::regex driverRegex(R"(\d{2}\.\d\.\d{5}\.\d{4})");
    if (std::regex_search(headerStr, match, driverRegex))
        known.driverVersion = match.str();

    // CPU model
    std::vector<std::regex> cpuPatterns = {
        std::regex(R"(AMD Ryzen[^\x00\x12]+Processor\s*)"),
        std::regex(R"(Intel\(R\) Core[^\x00\x12]+)"),
        std::regex(R"([^\x00]{10,50}Processor\s*)")
    };
    for (const auto& pattern : cpuPatterns) {
        if (std::regex_search(headerStr, match, pattern)) {
            std::string cpuModel = match.str();
            cpuModel.erase(std::find_if(cpuModel.rbegin(), cpuModel.rend(),
                [](unsigned char ch) { return ch >= 32 && ch < 127; }).base(), cpuModel.end());
            if (cpuModel.length() > 10) { known.cpuModel = cpuModel; break; }
        }
    }
}

// ── 解压缩 (miniz) ──

bool PerfDataParser::Decompress() {
    if (m_compressedData.empty()) {
        m_result.errorMessage = "No compressed data";
        return false;
    }

    mz_ulong decompressedSize = static_cast<mz_ulong>(m_compressedData.size() * 10);
    m_decompressedData.resize(decompressedSize);

    int result = mz_uncompress(m_decompressedData.data(), &decompressedSize,
                                m_compressedData.data(),
                                static_cast<mz_ulong>(m_compressedData.size()));

    if (result != MZ_OK) {
        m_result.errorMessage = "Decompression failed: " + std::to_string(result);
        return false;
    }

    m_decompressedData.resize(decompressedSize);
    m_result.content.size = decompressedSize;
    return true;
}

// ── 解析内容 ──

bool PerfDataParser::ParseContent() {
    if (m_decompressedData.empty()) return false;

    // 提取字符串
    std::string current;
    for (uint8_t b : m_decompressedData) {
        if (b >= 32 && b < 127) {
            current += static_cast<char>(b);
        } else {
            if (current.length() >= 3)
                m_result.content.strings.push_back(current);
            current.clear();
        }
    }

    ParseKeyValues();

    // 提取坐标
    std::string text(m_decompressedData.begin(), m_decompressedData.end());
    std::regex coordRegex(
        R"(\[(\d{2}:\d{2}:\d{2}\.\d+)\]\[(\d+)\]\[0x([A-Fa-f0-9]+)\]\(X=([-\d\.]+),Y=([-\d\.]+),Z=([-\d\.]+)\)\s*([\d\.]+s)?)"
    );
    auto coordBegin = std::sregex_iterator(text.begin(), text.end(), coordRegex);
    auto coordEnd = std::sregex_iterator();
    for (auto i = coordBegin; i != coordEnd; ++i) {
        std::smatch match = *i;
        CoordinateData coord;
        coord.timestamp = match[1].str();
        coord.frame = std::stoi(match[2].str());
        coord.address = match[3].str();
        coord.x = std::stof(match[4].str());
        coord.y = std::stof(match[5].str());
        coord.z = std::stof(match[6].str());
        coord.duration = match[7].str();
        m_result.content.coordinates.push_back(coord);
    }

    CategorizeStrings();
    return true;
}

void PerfDataParser::ParseKeyValues() {
    std::string text(m_decompressedData.begin(), m_decompressedData.end());
    std::vector<std::pair<std::string, std::regex>> patterns = {
        {"sdk_version", std::regex(R"(sdk_version\x07\x00(\d+))")},
        {"proj_name", std::regex(R"(proj_name\x0a\x00(\w+))")},
        {"proj_version", std::regex(R"(proj_version\x0d\x00([\d\.]+))")},
        {"engine_version", std::regex(R"(engine_version\x1b\x00([^\x00]+))")},
        {"match_id", std::regex(R"(match_id\x12\x00(\d+))")},
        {"time_zone_offset", std::regex(R"(time_zone_offset\x05\x00(\d+))")},
        {"locale", std::regex(R"(locale\x05\x00([\w-]+))")},
        {"language", std::regex(R"(language\x05\x00([\w-]+))")},
    };
    for (const auto& [key, pattern] : patterns) {
        std::smatch match;
        if (std::regex_search(text, match, pattern))
            m_result.content.keyValues[key] = match[1].str();
    }
}

void PerfDataParser::CategorizeStrings() {
    auto& cats = m_result.content.categories;
    for (const std::string& s : m_result.content.strings) {
        std::string sLower = s;
        std::transform(sLower.begin(), sLower.end(), sLower.begin(), ::tolower);

        if (sLower.find("delta") != std::string::npos || sLower.find("proj") != std::string::npos ||
            sLower.find("game") != std::string::npos || sLower.find("scene") != std::string::npos)
            cats.gameInfo.push_back(s);
        else if (sLower.find("ue4") != std::string::npos || sLower.find("engine") != std::string::npos)
            cats.engineInfo.push_back(s);
        else if (sLower.find("amd") != std::string::npos || sLower.find("gpu") != std::string::npos)
            cats.hardwareInfo.push_back(s);
        else if (s.find(":\\") != std::string::npos || s.find(".dll") != std::string::npos)
            cats.pathInfo.push_back(s);
        else if (sLower.find("config") != std::string::npos)
            cats.configParams.push_back(s);
        else if (sLower.find("loading") != std::string::npos)
            cats.loadingFlow.push_back(s);
        else if (sLower.find("frame") != std::string::npos || sLower.find("perf") != std::string::npos)
            cats.performanceMetrics.push_back(s);
        else if (s.length() == 32 && std::all_of(s.begin(), s.end(), ::isxdigit))
            cats.uuids.push_back(s);
        else if (std::regex_match(s, std::regex(R"(^\d{10,13}$)")))
            cats.timestamps.push_back(s);
        else if (s.length() > 3)
            cats.other.push_back(s);
    }

    auto removeDuplicates = [](std::vector<std::string>& vec) {
        std::sort(vec.begin(), vec.end());
        vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
    };
    removeDuplicates(cats.gameInfo);
    removeDuplicates(cats.engineInfo);
    removeDuplicates(cats.hardwareInfo);
    removeDuplicates(cats.pathInfo);
    removeDuplicates(cats.configParams);
    removeDuplicates(cats.loadingFlow);
    removeDuplicates(cats.performanceMetrics);
    removeDuplicates(cats.uuids);
    removeDuplicates(cats.timestamps);
    removeDuplicates(cats.other);
}

FileInfo PerfDataParser::ParseFilename(const std::string& filename) {
    FileInfo info;
    info.filename = filename;
    std::regex filenameRegex(R"(perf_data_(\d+)_(\d+))");
    std::smatch match;
    if (std::regex_search(filename, match, filenameRegex)) {
        info.timestampRaw = std::stoull(match[1].str());
        info.identifier = match[2].str();
    }
    return info;
}

ParseResult PerfDataParser::Parse() {
    if (!Load()) return m_result;
    if (!ParseHeader()) return m_result;
    if (Decompress()) ParseContent();

    size_t lastSlash = m_filePath.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos) ?
                           m_filePath.substr(lastSlash + 1) : m_filePath;
    m_result.fileInfo = ParseFilename(filename);
    m_result.success = true;
    return m_result;
}

// ── 重新序列化 Header ──

std::vector<uint8_t> PerfDataParser::ReserializeHeader(const KnownFields& modifiedFields) {
    std::vector<uint8_t> newHeader;

    for (const auto& field : m_result.header.rawFields) {
        std::vector<uint8_t> fieldBytes;
        int fn = field.fieldNumber;

        std::string newValue;
        bool shouldModify = false;

        if (field.type == FieldType::String) {
            switch (fn) {
                case 11: if (!modifiedFields.osName.empty()) { newValue = modifiedFields.osName; shouldModify = true; } break;
                case 13: if (!modifiedFields.osVersion.empty()) { newValue = modifiedFields.osVersion; shouldModify = true; } break;
                case 17: if (!modifiedFields.architecture.empty()) { newValue = modifiedFields.architecture; shouldModify = true; } break;
                case 18: if (!modifiedFields.gpuVendor.empty()) { newValue = modifiedFields.gpuVendor; shouldModify = true; } break;
                case 19: if (!modifiedFields.gpuModel.empty()) { newValue = modifiedFields.gpuModel; shouldModify = true; } break;
                case 20: if (!modifiedFields.driverVersion.empty()) { newValue = modifiedFields.driverVersion; shouldModify = true; } break;
                case 23: case 39: if (!modifiedFields.macAddress.empty()) { newValue = modifiedFields.macAddress; shouldModify = true; } break;
                case 24: case 40: if (!modifiedFields.cpuModel.empty()) { newValue = modifiedFields.cpuModel; shouldModify = true; } break;
                case 30: if (!modifiedFields.hardwareId1.empty()) { newValue = modifiedFields.hardwareId1; shouldModify = true; } break;
                case 32: if (!modifiedFields.hardwareId2.empty()) { newValue = modifiedFields.hardwareId2; shouldModify = true; } break;
                case 9:  if (!modifiedFields.compression.empty()) { newValue = modifiedFields.compression; shouldModify = true; } break;
            }

            // deviceId / sessionId / gameVersion / osVersion 通过内容匹配
            if (!shouldModify) {
                if (field.value.length() == 20 &&
                    std::all_of(field.value.begin(), field.value.end(), ::isdigit) &&
                    !modifiedFields.deviceId.empty()) {
                    newValue = modifiedFields.deviceId;
                    shouldModify = true;
                } else if (field.value.length() == 9 &&
                           std::all_of(field.value.begin(), field.value.end(), ::isdigit) &&
                           !modifiedFields.sessionId.empty()) {
                    newValue = modifiedFields.sessionId;
                    shouldModify = true;
                } else if (std::regex_match(field.value, std::regex(R"(^\d+\.\d+\.\d+\.\d+\.\d+$)")) &&
                           !modifiedFields.gameVersion.empty()) {
                    newValue = modifiedFields.gameVersion;
                    shouldModify = true;
                } else if (std::regex_match(field.value, std::regex(R"(^10\.0\.\d+$)")) &&
                           !modifiedFields.osVersion.empty()) {
                    newValue = modifiedFields.osVersion;
                    shouldModify = true;
                }
            }
        }

        // 序列化
        if (shouldModify) {
            fieldBytes = ProtobufParser::SerializeStringField(fn, newValue);
        } else {
            switch (field.type) {
                case FieldType::Varint:
                    fieldBytes = ProtobufParser::SerializeVarintField(fn, field.numericValue); break;
                case FieldType::String:
                    fieldBytes = ProtobufParser::SerializeStringField(fn, field.value); break;
                case FieldType::Bytes:
                    fieldBytes = ProtobufParser::SerializeBytesField(fn, field.value); break;
                case FieldType::Fixed32:
                    fieldBytes = ProtobufParser::SerializeFixed32Field(fn, static_cast<uint32_t>(field.numericValue)); break;
                case FieldType::Fixed64:
                    fieldBytes = ProtobufParser::SerializeFixed64Field(fn, field.numericValue); break;
            }
        }

        newHeader.insert(newHeader.end(), fieldBytes.begin(), fieldBytes.end());
    }

    // 外层嵌套包装
    if (m_result.header.hasOuterWrapper) {
        std::vector<uint8_t> wrappedHeader;
        uint64_t tag = (static_cast<uint64_t>(m_result.header.outerFieldNumber) << 3) | 2;
        auto tagBytes = ProtobufParser::EncodeVarint(tag);
        wrappedHeader.insert(wrappedHeader.end(), tagBytes.begin(), tagBytes.end());
        auto lengthBytes = ProtobufParser::EncodeVarint(newHeader.size());
        wrappedHeader.insert(wrappedHeader.end(), lengthBytes.begin(), lengthBytes.end());
        wrappedHeader.insert(wrappedHeader.end(), newHeader.begin(), newHeader.end());
        return wrappedHeader;
    }

    return newHeader;
}

} // namespace PerfMigration
