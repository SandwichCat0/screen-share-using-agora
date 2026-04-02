#pragma once
/**
 * perf_proto.h — Protobuf 编解码工具
 */

#include "perf_types.h"
#include <vector>
#include <tuple>
#include <cstdint>

namespace PerfMigration {

class ProtobufParser {
public:
    static std::tuple<uint64_t, size_t> ParseVarint(const std::vector<uint8_t>& data, size_t pos);
    static bool IsZlibHeader(const std::vector<uint8_t>& data, size_t pos);
    static int FindZlibStart(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> EncodeVarint(uint64_t value);
    static std::vector<uint8_t> SerializeStringField(int fieldNumber, const std::string& value);
    static std::vector<uint8_t> SerializeVarintField(int fieldNumber, uint64_t value);
    static std::vector<uint8_t> SerializeBytesField(int fieldNumber, const std::string& hexValue);
    static std::vector<uint8_t> SerializeFixed32Field(int fieldNumber, uint32_t value);
    static std::vector<uint8_t> SerializeFixed64Field(int fieldNumber, uint64_t value);
};

} // namespace PerfMigration
