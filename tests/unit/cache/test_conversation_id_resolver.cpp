#include "cache/conversation_id_resolver.h"
#include "aegisgate/types.h"
#include <gtest/gtest.h>

using namespace aegisgate;

namespace {

ChatRequest makeReq(std::vector<std::pair<std::string, std::string>> msgs,
                    std::optional<std::string> client_id = std::nullopt) {
    ChatRequest req;
    for (auto& [r, c] : msgs) req.messages.emplace_back(r, c);
    if (client_id) {
        std::map<std::string, std::string> m;
        m["conversation_id"] = *client_id;
        req.metadata = std::move(m);
    }
    return req;
}

} // namespace

TEST(ConversationIdResolverTest, PrefersClientProvidedId) {
    ConversationIdResolver resolver;
    auto req = makeReq({{"user", "hi"}}, "client-abc-123");
    EXPECT_EQ(resolver.resolve(req), "client-abc-123");
}

TEST(ConversationIdResolverTest, FallsBackToHistoryHashWhenNoClientId) {
    ConversationIdResolver resolver;
    auto req = makeReq({{"user", "first turn"},
                        {"assistant", "ok"},
                        {"user", "second turn"}});
    auto id = resolver.resolve(req);
    EXPECT_FALSE(id.empty());
    EXPECT_EQ(id.rfind("hist-", 0), 0u); // starts with "hist-"
}

TEST(ConversationIdResolverTest, FirstTurnFallbackIsStable) {
    ConversationIdResolver resolver;
    auto req = makeReq({{"user", "hello world"}});
    auto id = resolver.resolve(req);
    EXPECT_EQ(id.rfind("first-", 0), 0u);
    EXPECT_EQ(resolver.resolve(req), id);
}

TEST(ConversationIdResolverTest, EmptyMessagesProducesEmptyId) {
    ConversationIdResolver resolver;
    ChatRequest req;
    EXPECT_EQ(resolver.resolve(req), "");
}

TEST(ConversationIdResolverTest, MultiTenantClientIdNotCollideAtResolverLayer) {
    ConversationIdResolver resolver;
    auto req_a = makeReq({{"user", "x"}}, "shared-id");
    auto req_b = makeReq({{"user", "x"}}, "shared-id");
    EXPECT_EQ(resolver.resolve(req_a), resolver.resolve(req_b));
}

TEST(ConversationIdResolverTest, ClientIdLongerThan256Truncated) {
    ConversationIdResolver resolver;
    std::string long_id(300, 'x');
    auto req = makeReq({{"user", "hi"}}, long_id);
    auto id = resolver.resolve(req);
    EXPECT_LE(id.size(), 256u);
    EXPECT_FALSE(id.empty());
}

TEST(ConversationIdResolverTest, ClientIdSanitizesControlChars) {
    ConversationIdResolver resolver;
    std::string with_ctrl = "id\n\twith\x01ctrl";
    auto req = makeReq({{"user", "hi"}}, with_ctrl);
    auto id = resolver.resolve(req);
    EXPECT_EQ(id.find('\n'), std::string::npos);
    EXPECT_EQ(id.find('\t'), std::string::npos);
    EXPECT_EQ(id.find('\x01'), std::string::npos);
}

TEST(ConversationIdResolverTest, HistoryHashStableAcrossCalls) {
    ConversationIdResolver resolver;
    auto req1 = makeReq({{"user", "q1"}, {"assistant", "a1"}, {"user", "q2"}});
    auto req2 = makeReq({{"user", "q1"}, {"assistant", "a1"}, {"user", "q2"}});
    EXPECT_EQ(resolver.resolve(req1), resolver.resolve(req2));
}

TEST(ConversationIdResolverTest, HistoryHashChangesOnDifferentHistory) {
    ConversationIdResolver resolver;
    auto req1 = makeReq({{"user", "q1"}, {"assistant", "a1"}, {"user", "q2"}});
    auto req2 = makeReq({{"user", "q1-DIFFERENT"}, {"assistant", "a1"}, {"user", "q2"}});
    EXPECT_NE(resolver.resolve(req1), resolver.resolve(req2));
}

TEST(ConversationIdResolverTest, OnlyAssistantMessagesFallbackToFirstTurn) {
    ConversationIdResolver resolver;
    auto req = makeReq({{"assistant", "hi"}});
    auto id = resolver.resolve(req);
    EXPECT_EQ(id.rfind("first-", 0), 0u);
}

TEST(ConversationIdResolverTest, MetadataWithUnrelatedKeysIgnored) {
    ConversationIdResolver resolver;
    ChatRequest req;
    req.messages.emplace_back("user", "hi");
    std::map<std::string, std::string> m;
    m["session"] = "s1"; // not conversation_id
    req.metadata = m;
    auto id = resolver.resolve(req);
    EXPECT_EQ(id.rfind("first-", 0), 0u);
}

TEST(ConversationIdResolverTest, EmptyClientIdFallbackToHash) {
    ConversationIdResolver resolver;
    auto req = makeReq({{"user", "q1"}, {"assistant", "a1"}, {"user", "q2"}}, "");
    auto id = resolver.resolve(req);
    EXPECT_EQ(id.rfind("hist-", 0), 0u);
}
