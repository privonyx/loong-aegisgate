#pragma once
#include "gateway/router.h"
#include <memory>
#include <string>
#include <vector>

namespace aegisgate {

struct ABVariant {
    std::string model;
    int weight = 1;
};

struct ABExperiment {
    std::string name;
    std::vector<ABVariant> variants;
    bool enabled = true;
    std::string tenant_id;
};

class ABTestRouter : public Router {
public:
    ABTestRouter(std::unique_ptr<Router> base_router,
                 std::vector<ABExperiment> experiments);

    std::string selectModel(RequestContext& ctx,
                             const ConnectorRegistry& registry) override;

    // TASK-20260703-02 C12：转发 outcome 反馈给内层路由（学习型路由才能更新统计）。
    void reportOutcome(const std::string& model, double latency_ms,
                       bool success) override {
        if (base_) base_->reportOutcome(model, latency_ms, success);
    }

private:
    std::string assignVariant(const ABExperiment& exp,
                               const std::string& request_id) const;
    std::unique_ptr<Router> base_;
    std::vector<ABExperiment> experiments_;
};

}  // namespace aegisgate
