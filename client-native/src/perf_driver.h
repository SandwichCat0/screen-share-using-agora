#pragma once
/**
 * perf_driver.h — FsUtil 驱动通信
 * 移植自 receiver/DriverComm, 排除 PAC 代理相关功能
 * 新增: AES-256-CBC 端口鉴权 + 消息会话令牌
 */

#include "fsutil_defs.h"
#include <string>
#include <cstdint>

namespace PerfMigration {

class DriverComm {
public:
    DriverComm();
    ~DriverComm();

    // 驱动安装/加载
    bool InstallDriver(const std::string& sysFilePath, bool autoStart = true);
    bool LoadDriver();
    bool UnloadDriver();
    bool IsDriverInstalled();
    bool IsDriverLoaded();
    bool RemoveDriver();
    bool EnsureDriverReady(const std::string& sysFilePath);
    // 从 DLL 资源中提取 fsutil.sys 并返回临时路径
    std::string ExtractDriverFromResource();

    // 通信 (Connect 现在会执行 AES 鉴权握手)
    bool Connect();
    void Disconnect();
    bool IsConnected() const { return m_port != INVALID_HANDLE_VALUE; }

    // 时间戳伪造
    HRESULT ClearAllFakeEntries();
    HRESULT AttachToVolume(const std::string& volumeLetter);
    HRESULT AddFakeTimestampEntry(const std::string& filePath,
                                   int64_t creationTime,
                                   int64_t lastAccessTime,
                                   int64_t lastWriteTime,
                                   int64_t changeTime,
                                   int64_t expirationTime = 0);
    HRESULT GetFakeEntryCount(ULONG* count);

    const std::string& GetLastError() const { return m_lastError; }

private:
    HANDLE m_port;
    std::string m_lastError;
    UCHAR m_sessionToken[32];   // 连接握手后派生的会话令牌

    bool CreateFilterRegistryKeys(const std::string& sysDestPath, bool autoStart);

    // 鉴权辅助
    bool BuildAuthContext(UCHAR* outEncrypted, ULONG outSize);
    void DeriveSessionToken(const FSU_AUTH_CONTEXT* ctx);
    void StampMessage(PCOMMAND_MESSAGE msg);  // 将 m_sessionToken 写入消息头
};

} // namespace PerfMigration
