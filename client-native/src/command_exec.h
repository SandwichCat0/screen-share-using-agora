#pragma once
#include "json_helpers.h"
#include <string>

namespace MDShare {
namespace CommandExec {

/** 安全执行命令（白名单机制） */
json Execute(const std::string& command);

} // namespace CommandExec
} // namespace MDShare
