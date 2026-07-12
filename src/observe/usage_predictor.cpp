#include "observe/usage_predictor.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>

namespace aegisgate {

UsagePredictor::UsagePredictor(PersistentStore* store) : store_(store) {}

std::string UsagePredictor::todayDate() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm buf{};
    gmtime_r(&tt, &buf);
    char date_buf[16];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &buf);
    return date_buf;
}

std::string UsagePredictor::dateOffset(const std::string& base_date, int days) {
    struct tm buf{};
    strptime(base_date.c_str(), "%Y-%m-%d", &buf);
    buf.tm_mday += days;
    auto tt = timegm(&buf);
    gmtime_r(&tt, &buf);
    char date_buf[16];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &buf);
    return date_buf;
}

std::vector<DailyAggregate> UsagePredictor::aggregateDaily(
    const std::string& tenant_id, int days) const {
    if (!store_) return {};

    auto today = todayDate();
    auto from = dateOffset(today, -days);
    auto from_ts = from + "T00:00:00Z";
    auto to_ts = today + "T23:59:59Z";

    auto records = store_->queryCostsByDateRange(tenant_id, from_ts, to_ts);

    std::map<std::string, DailyAggregate> daily;
    for (const auto& rec : records) {
        auto date = rec.timestamp.substr(0, 10);
        auto& agg = daily[date];
        agg.date = date;
        agg.total_cost += rec.total_cost;
        agg.request_count++;
    }

    std::vector<DailyAggregate> result;
    result.reserve(daily.size());
    for (auto& [_, agg] : daily) {
        result.push_back(std::move(agg));
    }
    std::sort(result.begin(), result.end(),
              [](const DailyAggregate& a, const DailyAggregate& b) {
                  return a.date < b.date;
              });
    return result;
}

UsagePredictor::LinearFit UsagePredictor::fitLinear(
    const std::vector<DailyAggregate>& data) const {
    int n = static_cast<int>(data.size());
    if (n < 3) return {};

    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
    for (int i = 0; i < n; ++i) {
        double x = static_cast<double>(i);
        double y = data[static_cast<size_t>(i)].total_cost;
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }

    double denom = n * sum_x2 - sum_x * sum_x;
    if (std::abs(denom) < 1e-12) return {};

    double slope = (n * sum_xy - sum_x * sum_y) / denom;
    double intercept = (sum_y - slope * sum_x) / n;
    double mean_y = sum_y / n;

    double ss_tot = 0, ss_res = 0;
    for (int i = 0; i < n; ++i) {
        double y = data[static_cast<size_t>(i)].total_cost;
        double y_pred = slope * static_cast<double>(i) + intercept;
        ss_tot += (y - mean_y) * (y - mean_y);
        ss_res += (y - y_pred) * (y - y_pred);
    }

    double r2 = (ss_tot > 0) ? 1.0 - ss_res / ss_tot : 0.0;
    return {slope, intercept, std::max(0.0, r2)};
}

UsagePrediction UsagePredictor::predict(const std::string& tenant_id,
                                         int history_days,
                                         int forecast_days) const {
    UsagePrediction result;
    result.historical = aggregateDaily(tenant_id, history_days);

    if (result.historical.size() < 3) {
        return result;
    }

    auto fit = fitLinear(result.historical);
    result.daily_trend = fit.slope;
    result.r_squared = fit.r_squared;

    if (forecast_days > 0) {
        int n = static_cast<int>(result.historical.size());
        auto last_date = result.historical.back().date;

        for (int d = 1; d <= forecast_days; ++d) {
            DailyAggregate pred;
            pred.date = dateOffset(last_date, d);
            double x = static_cast<double>(n + d - 1);
            pred.total_cost = std::max(0.0, fit.slope * x + fit.intercept);
            pred.request_count = 0;
            result.predicted.push_back(std::move(pred));
        }
    }

    return result;
}

UsagePrediction UsagePredictor::predictBudgetExhaustion(
    const std::string& tenant_id, double budget, int history_days) const {
    auto result = predict(tenant_id, history_days, 365);

    double accumulated = 0.0;
    for (const auto& h : result.historical) {
        accumulated += h.total_cost;
    }

    for (const auto& p : result.predicted) {
        accumulated += p.total_cost;
        if (accumulated >= budget) {
            result.budget_exhaustion_date = p.date;
            break;
        }
    }

    result.predicted.clear();
    return result;
}

}  // namespace aegisgate
