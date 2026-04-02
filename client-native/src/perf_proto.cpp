/**
 * perf_proto.cpp — Protobuf varint/field 编解码
 * 直接移植自 日志迁移/receiver/ProtobufParser.cpp
 */

#include "perf_proto.h"

namespace PerfMigration {

std::tuple<uint64_t, size_t> ProtobufParser::ParseVarint(const std::vector<uint8_t>& data, size_t pos) {
    uint64_t result = 0;
    int shift = 0;
    while (pos < data.size()) {
        uint8_t b = data[pos];
        result |= static_cast<uint64_t>(b & 0x7F) << shift;
        pos++;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return std::make_tuple(result, pos);
}

bool ProtobufParser::IsZlibHeader(const std::vector<uint8_t>& data, size_t pos) {
    if (pos + 1 >= data.size()) return false;
    return data[pos] == 0x78 && data[pos + 1] == 0x9C;
}

int ProtobufParser::FindZlibStart(const std::vector<uint8_t>& data) {
    for (size_t i = 0; i + 1 < data.size(); i++) {
        if (IsZlibHeader(data, i)) return static_cast<int>(i);
    }
    return -1;
}

std::vector<uint8_t> ProtobufParser::EncodeVarint(uint64_t value) {
    std::vector<uint8_t> result;
    while (value > 0x7F) {
        result.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    result.push_back(static_cast<uint8_t>(value));
    return result;
}

std::vector<uint8_t> ProtobufParser::SerializeStringField(int fieldNumber, const std::string& value) {
    std::vector<uint8_t> result;
    uint64_t tag = (static_cast<uint64_t>(fieldNumber) << 3) | 2;
    auto tagBytes = EncodeVarint(tag);
    result.insert(result.end(), tagBytes.begin(), tagBytes.end());
    auto lengthBytes = EncodeVarint(value.length());
    result.insert(result.end(), lengthBytes.begin(), lengthBytes.end());
    result.insert(result.end(), value.begin(), value.end());
    return result;
}

std::vector<uint8_t> ProtobufParser::SerializeVarintField(int fieldNumber, uint64_t value) {
    std::vector<uint8_t> result;
    uint64_t tag = (static_cast<uint64_t>(fieldNumber) << 3) | 0;
    auto tagBytes = EncodeVarint(tag);
    result.insert(result.end(), tagBytes.begin(), tagBytes.end());
    auto valueBytes = EncodeVarint(value);
    result.insert(result.end(), valueBytes.begin(), valueBytes.end());
    return result;
}

std::vector<uint8_t> ProtobufParser::SerializeBytesField(int fieldNumber, const std::string& hexValue) {
    std::vector<uint8_t> result;
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hexValue.length(); i += 2) {
        std::string byteStr = hexValue.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16));
        bytes.push_back(byte);
    }
    uint64_t tag = (static_cast<uint64_t>(fieldNumber) << 3) | 2;
    auto tagBytes = EncodeVarint(tag);
    result.insert(result.end(), tagBytes.begin(), tagBytes.end());
    auto lengthBytes = EncodeVarint(bytes.size());
    result.insert(result.end(), lengthBytes.begin(), lengthBytes.end());
    result.insert(result.end(), bytes.begin(), bytes.end());
    return result;
}

std::vector<uint8_t> ProtobufParser::SerializeFixed32Field(int fieldNumber, uint32_t value) {
    std::vector<uint8_t> result;
    uint64_t tag = (static_cast<uint64_t>(fieldNumber) << 3) | 5;
    auto tagBytes = EncodeVarint(tag);
    result.insert(result.end(), tagBytes.begin(), tagBytes.end());
    result.push_back(static_cast<uint8_t>(value & 0xFF));
    result.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    result.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    result.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    return result;
}

std::vector<uint8_t> ProtobufParser::SerializeFixed64Field(int fieldNumber, uint64_t value) {
    std::vector<uint8_t> result;
    uint64_t tag = (static_cast<uint64_t>(fieldNumber) << 3) | 1;
    auto tagBytes = EncodeVarint(tag);
    result.insert(result.end(), tagBytes.begin(), tagBytes.end());
    for (int i = 0; i < 8; i++) {
        result.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
    return result;
}

} // namespace PerfMigration
