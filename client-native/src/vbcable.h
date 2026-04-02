#pragma once
#include "json_helpers.h"
#include <functional>

namespace MDShare {
namespace VBCable {

/** 发送回调类型 */
using SendCallback = std::function<void(const std::string& type, const json& data)>;

/** 检测 VBCable 是否已安装 */
json Check();

/** 检测默认录制设备是否为 CABLE Output，并返回当前默认设备名 */
json CheckDefaultRecording();

/** 将默认录制设备设置为 CABLE Output */
json SetDefaultRecordingToCable();

/** 下载并安装 VBCable（需在工作线程调用） */
void Install(SendCallback sendFn);

/** 卸载 VBCable（需在工作线程调用） */
void Uninstall(SendCallback sendFn);

/** Fire-and-forget 卸载（关闭/崩溃时调用） */
void TryUninstallFireAndForget();

} // namespace VBCable
} // namespace MDShare
