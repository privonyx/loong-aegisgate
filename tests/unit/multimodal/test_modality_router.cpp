#include "multimodal/modality_router.h"
#include <gtest/gtest.h>
#include <atomic>
#include <nlohmann/json.hpp>

using namespace aegisgate;

namespace {

class FakeHandler : public ModalityHandler {
public:
    FakeHandler(Modality m, std::string prov, double cost,
                std::string body = R"({"ok":true})", int status = 200)
        : modality_(m),
          provider_(std::move(prov)),
          cost_(cost),
          body_(std::move(body)),
          status_(status) {}

    ProxyResponse handle(const ProxyRequest& req, const std::string& key) override {
        ++call_count_;
        last_endpoint_ = req.endpoint;
        last_api_key_ = key;
        ProxyResponse r;
        r.http_status = status_;
        r.body = body_;
        return r;
    }
    Modality modality() const override { return modality_; }
    std::string provider() const override { return provider_; }
    double estimateCost(const ProxyRequest&) const override { return cost_; }
    std::string name() const override { return provider_ + "_" + modalityToString(modality_); }

    size_t callCount() const { return call_count_.load(); }
    const std::string& lastApiKey() const { return last_api_key_; }
    const std::string& lastEndpoint() const { return last_endpoint_; }

private:
    Modality modality_;
    std::string provider_;
    double cost_;
    std::string body_;
    int status_;
    std::atomic<size_t> call_count_{0};
    std::string last_endpoint_;
    std::string last_api_key_;
};

ProxyRequest req(const std::string& endpoint, const std::string& body = "{}") {
    ProxyRequest r;
    r.endpoint = endpoint;
    r.raw_body = body;
    return r;
}

} // namespace

TEST(ModalityRouterTest, RegistersAndCountsHandlers) {
    ModalityRouter router;
    EXPECT_EQ(router.handlerCount(Modality::Embedding), 0u);
    router.registerHandler(std::make_unique<FakeHandler>(Modality::Embedding, "openai", 1.0));
    EXPECT_EQ(router.handlerCount(Modality::Embedding), 1u);
}

TEST(ModalityRouterTest, RegisteredProviders) {
    ModalityRouter router;
    router.registerHandler(std::make_unique<FakeHandler>(Modality::Embedding, "openai", 1.0));
    router.registerHandler(std::make_unique<FakeHandler>(Modality::Embedding, "voyage", 0.5));
    auto provs = router.registeredProviders(Modality::Embedding);
    ASSERT_EQ(provs.size(), 2u);
    EXPECT_EQ(provs[0], "openai");
    EXPECT_EQ(provs[1], "voyage");
}

TEST(ModalityRouterTest, RoutesToSingleHandler_N1) {
    ModalityRouter router;
    auto h = std::make_unique<FakeHandler>(Modality::Embedding, "openai", 1.0);
    auto* raw = h.get();
    router.registerHandler(std::move(h));

    auto resp = router.route(Modality::Embedding, req("/v1/embeddings"), "sk-test");
    EXPECT_EQ(resp.http_status, 200);
    EXPECT_EQ(raw->callCount(), 1u);
    EXPECT_EQ(raw->lastApiKey(), "sk-test");
    EXPECT_EQ(raw->lastEndpoint(), "/v1/embeddings");
}

TEST(ModalityRouterTest, UnregisteredModalityReturns504) {
    ModalityRouter router;
    auto resp = router.route(Modality::ImageGen, req("/v1/images/generations"), "sk");
    EXPECT_EQ(resp.http_status, 504);
    auto j = nlohmann::json::parse(resp.body);
    EXPECT_EQ(j["error"]["code"], "AEGIS-1601");
    EXPECT_EQ(j["error"]["modality"], "image_gen");
}

TEST(ModalityRouterTest, UnknownModalityReturns504) {
    ModalityRouter router;
    auto resp = router.route(Modality::Unknown, req("/garbage"), "sk");
    EXPECT_EQ(resp.http_status, 504);
}

TEST(ModalityRouterTest, CheapestStrategy_PicksLowestCost) {
    ModalityRouter router;
    auto expensive = std::make_unique<FakeHandler>(Modality::Embedding, "expensive", 5.0);
    auto cheap = std::make_unique<FakeHandler>(Modality::Embedding, "cheap", 0.5);
    auto* cheap_raw = cheap.get();
    router.registerHandler(std::move(expensive));
    router.registerHandler(std::move(cheap));
    router.setRoutingPolicy(Modality::Embedding, {RoutingPolicy::Strategy::Cheapest});

    router.route(Modality::Embedding, req("/v1/embeddings"), "sk");
    EXPECT_EQ(cheap_raw->callCount(), 1u);
}

TEST(ModalityRouterTest, RoundRobinStrategy_AlternatesHandlers) {
    ModalityRouter router;
    auto a = std::make_unique<FakeHandler>(Modality::Embedding, "a", 1.0);
    auto b = std::make_unique<FakeHandler>(Modality::Embedding, "b", 1.0);
    auto* a_raw = a.get();
    auto* b_raw = b.get();
    router.registerHandler(std::move(a));
    router.registerHandler(std::move(b));
    router.setRoutingPolicy(Modality::Embedding, {RoutingPolicy::Strategy::RoundRobin});

    for (int i = 0; i < 6; ++i) router.route(Modality::Embedding, req("/v1/embeddings"), "sk");
    EXPECT_EQ(a_raw->callCount(), 3u);
    EXPECT_EQ(b_raw->callCount(), 3u);
}

TEST(ModalityRouterTest, FastestP99Strategy_StubReturnsFront_N1Stub) {
    ModalityRouter router;
    auto first = std::make_unique<FakeHandler>(Modality::Embedding, "first", 5.0);
    auto second = std::make_unique<FakeHandler>(Modality::Embedding, "second", 0.5);
    auto* first_raw = first.get();
    router.registerHandler(std::move(first));
    router.registerHandler(std::move(second));
    router.setRoutingPolicy(Modality::Embedding, {RoutingPolicy::Strategy::FastestP99});

    router.route(Modality::Embedding, req("/v1/embeddings"), "sk");
    EXPECT_EQ(first_raw->callCount(), 1u);
}

TEST(ModalityRouterTest, SelectHandlerReturnsNullForEmpty) {
    ModalityRouter router;
    EXPECT_EQ(router.selectHandler(Modality::Embedding, req("/v1/embeddings")), nullptr);
}

TEST(ModalityRouterTest, SelectHandlerN1ShortCircuitsFront) {
    ModalityRouter router;
    auto h = std::make_unique<FakeHandler>(Modality::Embedding, "only", 1.0);
    auto* raw = h.get();
    router.registerHandler(std::move(h));
    EXPECT_EQ(router.selectHandler(Modality::Embedding, req("/v1/embeddings")), raw);
}

TEST(ModalityRouterTest, RouteCarriesEndpointAndKey) {
    ModalityRouter router;
    auto h = std::make_unique<FakeHandler>(Modality::Moderation, "openai", 0.0);
    auto* raw = h.get();
    router.registerHandler(std::move(h));

    auto resp = router.route(Modality::Moderation, req("/v1/moderations"), "sk-xyz");
    EXPECT_EQ(resp.http_status, 200);
    EXPECT_EQ(raw->lastEndpoint(), "/v1/moderations");
    EXPECT_EQ(raw->lastApiKey(), "sk-xyz");
}

TEST(ModalityRouterTest, IsolatedPerModality) {
    ModalityRouter router;
    auto emb = std::make_unique<FakeHandler>(Modality::Embedding, "e", 1.0);
    auto img = std::make_unique<FakeHandler>(Modality::ImageGen, "i", 1.0);
    auto* emb_raw = emb.get();
    auto* img_raw = img.get();
    router.registerHandler(std::move(emb));
    router.registerHandler(std::move(img));

    router.route(Modality::Embedding, req("/v1/embeddings"), "sk");
    EXPECT_EQ(emb_raw->callCount(), 1u);
    EXPECT_EQ(img_raw->callCount(), 0u);

    router.route(Modality::ImageGen, req("/v1/images/generations"), "sk");
    EXPECT_EQ(emb_raw->callCount(), 1u);
    EXPECT_EQ(img_raw->callCount(), 1u);
}

// Mutation test (D7+P1#1): if selectHandler ever returns nullptr in the
// happy path (e.g., bug introduced in the N=1 short-circuit), this test
// FAILs because route() will degrade to a 504.
TEST(ModalityRouterTest, MutationGuard_RouteDoesNotSilently504_WhenHandlerExists) {
    ModalityRouter router;
    router.registerHandler(std::make_unique<FakeHandler>(Modality::Embedding, "p", 1.0));
    auto resp = router.route(Modality::Embedding, req("/v1/embeddings"), "sk");
    EXPECT_EQ(resp.http_status, 200) << "selectHandler must not return nullptr when N>=1";
}
