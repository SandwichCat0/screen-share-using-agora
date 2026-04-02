#pragma once
#include "json_helpers.h"

namespace MDShare {
namespace ProcessMgr {

/** 获取进程列表（按内存降序，最多200个） */
json GetProcessList();

/** 安全结束进程（保护系统关键进程） */
json KillProcess(int pid);

} // namespace ProcessMgr
} // namespace MDShare
