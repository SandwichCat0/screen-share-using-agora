#pragma once
/**
 * dll_extractor.h — 从嵌入资源中提取 Agora DLLs 到临时目录
 *
 * 构建时将 Agora SDK 所有 DLL 打包为 ZIP 嵌入 RC 资源 (IDR_AGORA_DEPS_ZIP)。
 * 运行时在首次调用 Agora API 前，解压到 %TEMP%\MDShareNative\ 并
 * 通过 SetDllDirectory 使延迟加载的 agora_rtc_sdk.dll 可被找到。
 */

namespace DllExtractor {

/// 确保 Agora DLLs 已解压并设置了 DLL 搜索路径。
/// 幂等：首次调用执行解压，后续调用立即返回 true。
bool EnsureExtracted();

/// 清理临时目录 %TEMP%\MDShareNative\  
/// 应在窗口关闭、Agora 已释放后调用。
void Cleanup();

} // namespace DllExtractor
