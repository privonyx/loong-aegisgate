#include <gtest/gtest.h>
#include "core/config.h"
#include <fstream>
#include <cstdio>

using namespace aegisgate;

namespace {

std::string writeTempYaml(const std::string& content) {
    static int counter = 0;
    auto path = "/tmp/aegisgate_test_config_" + std::to_string(counter++) + ".yaml";
    std::ofstream f(path);
    f << content;
    return path;
}

} // namespace

TEST(ConfigValidateTest, ValidConfigNoIssues) {
    auto path = writeTempYaml(R"(
server:
  host: 0.0.0.0
  port: 8080
  threads: 4
logging:
  level: info
models_config: config/models.yaml
)");
    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(path));
    auto issues = cfg.validate();
    int errors = 0;
    for (const auto& i : issues) {
        if (i.severity == Config::ValidationIssue::Error) errors++;
    }
    EXPECT_EQ(errors, 0);
    std::remove(path.c_str());
}

TEST(ConfigValidateTest, InvalidPort) {
    auto path = writeTempYaml(R"(
server:
  port: 99999
)");
    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(path));
    auto issues = cfg.validate();
    bool found = false;
    for (const auto& i : issues) {
        if (i.field == "server.port" && i.severity == Config::ValidationIssue::Error) {
            found = true;
        }
    }
    EXPECT_TRUE(found) << "Expected error for invalid port";
    std::remove(path.c_str());
}

TEST(ConfigValidateTest, InvalidLogLevel) {
    auto path = writeTempYaml(R"(
logging:
  level: verbose
)");
    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(path));
    auto issues = cfg.validate();
    bool found = false;
    for (const auto& i : issues) {
        if (i.field == "logging.level") found = true;
    }
    EXPECT_TRUE(found) << "Expected error for invalid log level";
    std::remove(path.c_str());
}

TEST(ConfigValidateTest, TlsEnabledWithoutCert) {
    auto path = writeTempYaml(R"(
tls:
  enabled: true
  cert: ""
  key: ""
)");
    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(path));
    auto issues = cfg.validate();
    bool found = false;
    for (const auto& i : issues) {
        if (i.field == "tls" && i.severity == Config::ValidationIssue::Error) {
            found = true;
        }
    }
    EXPECT_TRUE(found) << "Expected error for TLS without cert";
    std::remove(path.c_str());
}

TEST(ConfigValidateTest, AuthEnabledNoKeys) {
    auto path = writeTempYaml(R"(
auth:
  enabled: true
)");
    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(path));
    auto issues = cfg.validate();
    bool found = false;
    for (const auto& i : issues) {
        if (i.field == "auth") found = true;
    }
    EXPECT_TRUE(found) << "Expected warning for auth without keys";
    std::remove(path.c_str());
}

TEST(ConfigValidateTest, NotLoaded) {
    Config cfg;
    auto issues = cfg.validate();
    ASSERT_FALSE(issues.empty());
    EXPECT_EQ(issues[0].severity, Config::ValidationIssue::Error);
}
