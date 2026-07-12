#include "observe/crash_handler.h"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {

std::string makeTmpDir() {
    char tmpl[] = "/tmp/aegis_crash_test_XXXXXX";
    char* d = mkdtemp(tmpl);
    return d ? std::string(d) : std::string();
}

// 读取 dir 下第一个 crash-*.log 内容；无则返回空。
std::string readCrashLog(const std::string& dir) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const auto name = entry.path().filename().string();
        if (name.rfind("crash-", 0) == 0 && entry.path().extension() == ".log") {
            std::ifstream ifs(entry.path());
            return std::string(std::istreambuf_iterator<char>(ifs),
                               std::istreambuf_iterator<char>());
        }
    }
    return {};
}

// fork 子进程：安装崩溃处理器后执行 crashFn（应崩溃，不返回）。
// 返回子进程退出 status（供 WIFSIGNALED/WTERMSIG 判定）。
template <typename Fn>
int runCrashingChild(const std::string& dir, Fn crashFn) {
    pid_t pid = fork();
    if (pid == 0) {
        // 子进程：屏蔽 stderr 崩溃噪声，避免污染测试输出。
        FILE* devnull = std::freopen("/dev/null", "w", stderr);
        (void)devnull;
        aegisgate::installCrashHandler({dir, "9.9.9-test"});
        crashFn();
        _exit(0);  // 不可达：crashFn 会以信号终止进程
    }
    EXPECT_GE(pid, 0);
    int status = 0;
    EXPECT_EQ(waitpid(pid, &status, 0), pid);
    return status;
}

}  // namespace

TEST(CrashHandlerTest, FatalSignalWritesCrashLog) {
    const std::string dir = makeTmpDir();
    ASSERT_FALSE(dir.empty());

    const int status = runCrashingChild(dir, [] { raise(SIGSEGV); });

    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGSEGV);

    const std::string log = readCrashLog(dir);
    EXPECT_NE(log.find("=== AegisGate crash ==="), std::string::npos);
    EXPECT_NE(log.find("SIGSEGV"), std::string::npos);
    EXPECT_NE(log.find("9.9.9-test"), std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(CrashHandlerTest, UncaughtExceptionWritesCrashLog) {
    const std::string dir = makeTmpDir();
    ASSERT_FALSE(dir.empty());

    // 在线程里抛未捕获异常 → std::terminate。用线程而非直接 throw，是因为
    // gtest 在测试体外包了 try/catch，直接 throw 会被框架捕获、到不了
    // set_terminate；逃逸出线程入口函数的异常则直接触发 std::terminate。
    const int status = runCrashingChild(dir, [] {
        std::thread t([] { throw std::runtime_error("boom-xyz"); });
        t.join();
    });

    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGABRT);

    const std::string log = readCrashLog(dir);
    EXPECT_NE(log.find("terminate"), std::string::npos);
    EXPECT_NE(log.find("boom-xyz"), std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(CrashHandlerTest, CrashLogContainsBacktrace) {
    const std::string dir = makeTmpDir();
    ASSERT_FALSE(dir.empty());

    const int status = runCrashingChild(dir, [] { raise(SIGSEGV); });
    ASSERT_TRUE(WIFSIGNALED(status));

    const std::string log = readCrashLog(dir);
    EXPECT_NE(log.find("backtrace:"), std::string::npos);
    // backtrace_symbols_fd 至少输出地址（如 [0x...]）。
    EXPECT_NE(log.find("0x"), std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}
