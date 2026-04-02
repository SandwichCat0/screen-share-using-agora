#pragma once
/**
 * perf_parser.h — PerfData 文件解析器
 * 解析 protobuf header, 解压 zlib 内容, 重新序列化 header
 */

#include "perf_types.h"
#include <string>
#include <vector>
#include <memory>

namespace PerfMigration {

class PerfDataParser {
public:
    explicit PerfDataParser(const std::string& filePath);
    ~PerfDataParser();

    bool Load();
    bool ParseHeader();
    bool Decompress();
    bool ParseContent();
    ParseResult Parse();

    const ParseResult& GetResult() const { return m_result; }

    // 重新序列化头部 (替换硬件字段)
    std::vector<uint8_t> ReserializeHeader(const KnownFields& modifiedFields);

    // 获取压缩数据 (与重序列化的 header 拼接)
    const std::vector<uint8_t>& GetCompressedData() const { return m_compressedData; }

    const std::vector<RawField>& GetRawFields() const { return m_result.header.rawFields; }

private:
    std::string m_filePath;
    std::vector<uint8_t> m_rawData;
    std::vector<uint8_t> m_headerData;
    std::vector<uint8_t> m_compressedData;
    std::vector<uint8_t> m_decompressedData;
    ParseResult m_result;

    void ExtractKnownFields();
    void ParseFixedHeaderFields();
    void ParseKeyValues();
    void CategorizeStrings();
    FileInfo ParseFilename(const std::string& filename);
};

} // namespace PerfMigration
