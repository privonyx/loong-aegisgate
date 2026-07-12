#include "auth/session_manager.h"
#include "storage/sqlite_persistent_store.h"
#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>
#include <thread>
#include <chrono>

using namespace aegisgate;

class SessionManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_config_path_ = "/tmp/test_session_mgr_config.yaml";
        {
            std::ofstream ofs(tmp_config_path_);
            ofs << "session:\n"
                << "  absolute_timeout: 3600\n"
                << "  idle_timeout: 1800\n"
                << "  max_concurrent: 3\n";
        }
        config_.loadFromFile(tmp_config_path_);

        store_ = std::make_unique<SQLitePersistentStore>(":memory:");
        store_->initialize();

        mgr_ = std::make_unique<SessionManager>(store_.get(), &config_);
    }

    void TearDown() override {
        mgr_.reset();
        store_.reset();
        std::remove(tmp_config_path_.c_str());
    }

    Session insertSessionWithTimestamps(const std::string& id,
                                         const std::string& user_id,
                                         const std::string& created_at,
                                         const std::string& last_active_at,
                                         const std::string& expires_at) {
        Session s;
        s.id = id;
        s.user_id = user_id;
        s.tenant_id = "t1";
        s.ip_address = "127.0.0.1";
        s.user_agent = "test-agent";
        s.auth_method = "sso";
        s.mfa_verified = false;
        s.created_at = created_at;
        s.last_active_at = last_active_at;
        s.expires_at = expires_at;
        store_->insertSession(s);
        return s;
    }

    std::string tmp_config_path_;
    Config config_;
    std::unique_ptr<SQLitePersistentStore> store_;
    std::unique_ptr<SessionManager> mgr_;
};

TEST_F(SessionManagerTest, CreateSession_ReturnsValidSession) {
    auto s = mgr_->createSession("u1", "t1", "sso", "10.0.0.1", "Mozilla/5.0");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->user_id, "u1");
    EXPECT_EQ(s->tenant_id, "t1");
    EXPECT_EQ(s->auth_method, "sso");
    EXPECT_EQ(s->ip_address, "10.0.0.1");
    EXPECT_EQ(s->user_agent, "Mozilla/5.0");
    EXPECT_FALSE(s->mfa_verified);
    EXPECT_EQ(s->id.size(), 32u);
    EXPECT_FALSE(s->created_at.empty());
    EXPECT_FALSE(s->last_active_at.empty());
    EXPECT_FALSE(s->expires_at.empty());
}

TEST_F(SessionManagerTest, GetSession_Valid) {
    auto created = mgr_->createSession("u1", "t1", "sso", "10.0.0.1", "agent");
    ASSERT_TRUE(created.has_value());

    auto fetched = mgr_->getSession(created->id);
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->id, created->id);
    EXPECT_EQ(fetched->user_id, "u1");
}

TEST_F(SessionManagerTest, GetSession_NotFound) {
    auto result = mgr_->getSession("nonexistent-session-id");
    EXPECT_FALSE(result.has_value());
}

TEST_F(SessionManagerTest, GetSession_Expired) {
    insertSessionWithTimestamps("expired-sess", "u1",
                                "2020-01-01T00:00:00Z",
                                "2020-01-01T00:00:00Z",
                                "2020-01-01T01:00:00Z");

    auto result = mgr_->getSession("expired-sess");
    EXPECT_FALSE(result.has_value());
}

TEST_F(SessionManagerTest, TouchSession_UpdatesLastActive) {
    auto created = mgr_->createSession("u1", "t1", "sso", "10.0.0.1", "agent");
    ASSERT_TRUE(created.has_value());
    std::string original_last_active = created->last_active_at;

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_TRUE(mgr_->touchSession(created->id));

    auto raw = store_->getSession(created->id);
    ASSERT_TRUE(raw.has_value());
    EXPECT_GE(raw->last_active_at, original_last_active);
}

TEST_F(SessionManagerTest, DeleteSession_ThenGetFails) {
    auto created = mgr_->createSession("u1", "t1", "sso", "10.0.0.1", "agent");
    ASSERT_TRUE(created.has_value());

    EXPECT_TRUE(mgr_->deleteSession(created->id));
    EXPECT_FALSE(mgr_->getSession(created->id).has_value());
}

TEST_F(SessionManagerTest, EnforceLimit_EvictsOldest) {
    insertSessionWithTimestamps("s1", "u1", "2025-01-01T00:00:00Z",
                                "2025-01-01T00:00:00Z", "2099-12-31T23:59:59Z");
    insertSessionWithTimestamps("s2", "u1", "2025-01-02T00:00:00Z",
                                "2025-01-02T00:00:00Z", "2099-12-31T23:59:59Z");
    insertSessionWithTimestamps("s3", "u1", "2025-01-03T00:00:00Z",
                                "2025-01-03T00:00:00Z", "2099-12-31T23:59:59Z");

    EXPECT_EQ(store_->countSessionsByUser("u1"), 3);

    auto s4 = mgr_->createSession("u1", "t1", "sso", "10.0.0.1", "agent");
    ASSERT_TRUE(s4.has_value());

    EXPECT_FALSE(store_->getSession("s1").has_value());

    EXPECT_TRUE(store_->getSession("s2").has_value());
    EXPECT_TRUE(store_->getSession("s3").has_value());
    EXPECT_TRUE(store_->getSession(s4->id).has_value());
    EXPECT_EQ(store_->countSessionsByUser("u1"), 3);
}

TEST_F(SessionManagerTest, DeleteExpiredSessions) {
    insertSessionWithTimestamps("exp1", "u1", "2020-01-01T00:00:00Z",
                                "2020-01-01T00:00:00Z", "2020-01-01T01:00:00Z");
    insertSessionWithTimestamps("exp2", "u1", "2020-02-01T00:00:00Z",
                                "2020-02-01T00:00:00Z", "2020-02-01T01:00:00Z");
    insertSessionWithTimestamps("valid", "u1", "2025-01-01T00:00:00Z",
                                "2025-01-01T00:00:00Z", "2099-12-31T23:59:59Z");

    auto deleted = mgr_->deleteExpiredSessions();
    EXPECT_EQ(deleted, 2);

    EXPECT_FALSE(store_->getSession("exp1").has_value());
    EXPECT_FALSE(store_->getSession("exp2").has_value());
    EXPECT_TRUE(store_->getSession("valid").has_value());
}

TEST_F(SessionManagerTest, SetMfaVerified) {
    auto created = mgr_->createSession("u1", "t1", "sso", "10.0.0.1", "agent");
    ASSERT_TRUE(created.has_value());
    EXPECT_FALSE(created->mfa_verified);

    EXPECT_TRUE(mgr_->setMfaVerified(created->id, true));

    auto fetched = store_->getSession(created->id);
    ASSERT_TRUE(fetched.has_value());
    EXPECT_TRUE(fetched->mfa_verified);
}

TEST_F(SessionManagerTest, ListUserSessions) {
    mgr_->createSession("u1", "t1", "sso", "10.0.0.1", "agent-1");
    mgr_->createSession("u1", "t1", "sso", "10.0.0.2", "agent-2");
    mgr_->createSession("u2", "t1", "api_key", "10.0.0.3", "agent-3");

    auto u1_sessions = mgr_->listUserSessions("u1");
    EXPECT_EQ(u1_sessions.size(), 2u);

    auto u2_sessions = mgr_->listUserSessions("u2");
    EXPECT_EQ(u2_sessions.size(), 1u);
}

TEST_F(SessionManagerTest, DeleteAllUserSessions) {
    mgr_->createSession("u1", "t1", "sso", "10.0.0.1", "agent-1");
    mgr_->createSession("u1", "t1", "sso", "10.0.0.2", "agent-2");
    mgr_->createSession("u2", "t1", "api_key", "10.0.0.3", "agent-3");

    auto deleted = mgr_->deleteAllUserSessions("u1");
    EXPECT_EQ(deleted, 2);
    EXPECT_TRUE(mgr_->listUserSessions("u1").empty());
    EXPECT_EQ(mgr_->listUserSessions("u2").size(), 1u);
}
