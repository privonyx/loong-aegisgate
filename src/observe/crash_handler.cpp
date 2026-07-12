#include "observe/crash_handler.h"

#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <string>
#include <typeinfo>

namespace aegisgate {
namespace {

constexpr int kMaxFrames = 64;

// 文件作用域静态缓冲：install 时填充，处理器只读（避免信号上下文堆分配）。
char g_path_prefix[PATH_MAX];   // "<crash_dir>/crash-<pid>-"
size_t g_path_prefix_len = 0;
char g_version[64];
size_t g_version_len = 0;
char g_pid_str[32];
size_t g_pid_len = 0;
volatile sig_atomic_t g_in_crash = 0;  // 重入守卫

// signal-safe：uint64 -> 十进制 ascii，写入 out（不含 NUL），返回长度。
size_t u64toa(uint64_t v, char* out) {
    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    char tmp[20];
    size_t n = 0;
    while (v > 0) {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    for (size_t i = 0; i < n; ++i) {
        out[i] = tmp[n - 1 - i];
    }
    return n;
}

// signal-safe：循环 write 直到写完/出错。
void safe_write(int fd, const char* s, size_t n) {
    if (fd < 0) return;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, s + off, n - off);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            break;
        }
        off += static_cast<size_t>(w);
    }
}

void safe_write_cstr(int fd, const char* s) {
    safe_write(fd, s, std::strlen(s));
}

// strsignal 非 async-signal-safe，禁用；自带 switch。
const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        case SIGBUS:  return "SIGBUS";
        default:      return "UNKNOWN";
    }
}

// signal-safe：写一份完整崩溃报告到 fd（reasonLine 不含换行）。
void writeReport(int fd, const char* reasonLine, uint64_t epoch,
                 void** frames, int nframes) {
    safe_write_cstr(fd, "=== AegisGate crash ===\n");
    safe_write_cstr(fd, "version: ");
    safe_write(fd, g_version, g_version_len);
    safe_write_cstr(fd, "\nreason: ");
    safe_write_cstr(fd, reasonLine);
    safe_write_cstr(fd, "\ntime: ");
    char numbuf[24];
    safe_write(fd, numbuf, u64toa(epoch, numbuf));
    safe_write_cstr(fd, "\npid: ");
    safe_write(fd, g_pid_str, g_pid_len);
    safe_write_cstr(fd, "\nbacktrace:\n");
    if (nframes > 0) {
        backtrace_symbols_fd(frames, nframes, fd);  // 不分配堆，signal-safe
    }
}

// signal-safe：落盘崩溃日志（每崩溃独立文件）+ stderr。
void emitCrash(const char* reasonLine, uint64_t epoch, void** frames, int nframes) {
    char path[PATH_MAX + 40];
    size_t n = g_path_prefix_len;
    std::memcpy(path, g_path_prefix, g_path_prefix_len);
    n += u64toa(epoch, path + n);
    std::memcpy(path + n, ".log", 4);
    n += 4;
    path[n] = '\0';

    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    writeReport(fd, reasonLine, epoch, frames, nframes);
    if (fd >= 0) close(fd);

    writeReport(STDERR_FILENO, reasonLine, epoch, frames, nframes);
}

void crashSignalHandler(int sig, siginfo_t* /*info*/, void* /*ctx*/) {
    if (g_in_crash) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_in_crash = 1;

    uint64_t epoch = static_cast<uint64_t>(time(nullptr));
    void* frames[kMaxFrames];
    int nframes = backtrace(frames, kMaxFrames);

    // 构造 "signal: SIGSEGV (11)"（栈缓冲，不分配堆）。
    char reason[80];
    size_t r = 0;
    const char prefix[] = "signal: ";
    std::memcpy(reason, prefix, sizeof(prefix) - 1);
    r += sizeof(prefix) - 1;
    const char* sn = signalName(sig);
    size_t snl = std::strlen(sn);
    std::memcpy(reason + r, sn, snl);
    r += snl;
    reason[r++] = ' ';
    reason[r++] = '(';
    r += u64toa(static_cast<uint64_t>(sig), reason + r);
    reason[r++] = ')';
    reason[r] = '\0';

    emitCrash(reason, epoch, frames, nframes);

    // 恢复默认并重抛：产生 core dump + 正确退出码。
    signal(sig, SIG_DFL);
    raise(sig);
}

// 不在信号上下文（std::terminate 调用链）→ 允许 std::string/typeid 等。
void crashTerminateHandler() {
    if (g_in_crash) {
        std::abort();
    }
    g_in_crash = 1;

    uint64_t epoch = static_cast<uint64_t>(time(nullptr));
    void* frames[kMaxFrames];
    int nframes = backtrace(frames, kMaxFrames);

    std::string reason = "terminate: uncaught exception";
    try {
        auto e = std::current_exception();
        if (e) std::rethrow_exception(e);
    } catch (const std::exception& ex) {
        reason += " (";
        reason += typeid(ex).name();
        reason += "): ";
        reason += ex.what();
    } catch (...) {
        reason += " (unknown type)";
    }

    emitCrash(reason.c_str(), epoch, frames, nframes);

    // → SIGABRT → 信号处理器因 g_in_crash 守卫直接重抛默认，不重复写。
    std::abort();
}

}  // namespace

void installCrashHandler(const CrashHandlerConfig& config) {
    // 版本（截断到缓冲容量）。
    g_version_len = std::min(config.version.size(), sizeof(g_version) - 1);
    std::memcpy(g_version, config.version.data(), g_version_len);
    g_version[g_version_len] = '\0';

    // pid 字符串。
    g_pid_len = u64toa(static_cast<uint64_t>(getpid()), g_pid_str);
    g_pid_str[g_pid_len] = '\0';

    // 前缀 "<crash_dir>/crash-<pid>-"（install 时构造，非信号上下文）。
    std::string prefix = config.crash_dir;
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';
    prefix += "crash-";
    prefix.append(g_pid_str, g_pid_len);
    prefix += '-';
    g_path_prefix_len = std::min(prefix.size(), sizeof(g_path_prefix) - 1);
    std::memcpy(g_path_prefix, prefix.data(), g_path_prefix_len);
    g_path_prefix[g_path_prefix_len] = '\0';

    // 预热 backtrace：首次调用可能 lazy-load libgcc（含 malloc），
    // 在此非信号上下文先触发，确保信号处理器内的 backtrace 不再分配堆。
    void* warm[1];
    (void)backtrace(warm, 1);

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crashSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);

    std::set_terminate(crashTerminateHandler);
}

}  // namespace aegisgate
