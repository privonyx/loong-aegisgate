#pragma once
#include <string>

namespace aegisgate {

struct CrashHandlerConfig {
    std::string crash_dir = "logs";  // 崩溃日志目录（相对运行目录）
    std::string version;             // AEGISGATE_VERSION
};

// 安装致命信号 + std::terminate 处理器。应在 main 早期调用一次
// （确保 crash_dir 已存在）。处理器写出 <crash_dir>/crash-<pid>-<epoch>.log
// 与 stderr，内容为信号/异常信息 + backtrace + 版本 + 时间 + pid。
//
// async-signal-safe：信号处理器内只用 open/write/close/raise/signal/time/
// backtrace/backtrace_symbols_fd 与自实现整数转字符串，不触碰堆分配。
void installCrashHandler(const CrashHandlerConfig& config);

}  // namespace aegisgate
