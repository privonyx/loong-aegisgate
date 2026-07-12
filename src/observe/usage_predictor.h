#pragma once
#include "storage/persistent_store.h"
#include <string>
#include <vector>

namespace aegisgate {

struct DailyAggregate {
    std::string date;
    double total_cost = 0.0;
    int request_count = 0;
};

struct UsagePrediction {
    std::vector<DailyAggregate> historical;
    std::vector<DailyAggregate> predicted;
    double daily_trend = 0.0;
    double r_squared = 0.0;
    std::string budget_exhaustion_date;
};

class UsagePredictor {
public:
    explicit UsagePredictor(PersistentStore* store);

    UsagePrediction predict(const std::string& tenant_id,
                             int history_days = 30,
                             int forecast_days = 7) const;

    UsagePrediction predictBudgetExhaustion(const std::string& tenant_id,
                                             double budget,
                                             int history_days = 30) const;

private:
    struct LinearFit {
        double slope = 0.0;
        double intercept = 0.0;
        double r_squared = 0.0;
    };

    LinearFit fitLinear(const std::vector<DailyAggregate>& data) const;
    std::vector<DailyAggregate> aggregateDaily(const std::string& tenant_id,
                                                int days) const;
    static std::string dateOffset(const std::string& base_date, int days);
    static std::string todayDate();

    PersistentStore* store_;
};

}  // namespace aegisgate
