#ifdef AEGISGATE_ENABLE_REDIS

#include "cluster/redis_state_store.h"
#include "storage/redis_cache_store.h"
#include <hiredis/hiredis.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <optional>
#include <random>
#include <sstream>
#include <string>

namespace aegisgate {

namespace {

// Atomic token-bucket check-and-decrement. The whole read-modify-write runs
// inside one EVAL so concurrent cluster nodes sharing a key cannot over-admit
// (P0-C TOCTOU fix). State is stored as a JSON string ({"tokens","last_refill"})
// so rateLimitRemaining() can still read it with a plain GET.
const char* kRateLimitLua = R"LUA(
local key = KEYS[1]
local max_t = tonumber(ARGV[1])
local rate = tonumber(ARGV[2])
local cost = tonumber(ARGV[3])
local now = tonumber(ARGV[4])

local tokens = max_t
local last = now
local raw = redis.call('GET', key)
if raw then
    local ok, data = pcall(cjson.decode, raw)
    if ok and type(data) == 'table' then
        tokens = tonumber(data.tokens) or max_t
        last = tonumber(data.last_refill) or now
    end
end

local elapsed = (now - last) / 1000.0
tokens = math.min(max_t, tokens + elapsed * rate)

local allowed = 0
if tokens >= cost then
    tokens = tokens - cost
    allowed = 1
end

redis.call('SET', key, cjson.encode({tokens = tokens, last_refill = now}), 'EX', 600)
return allowed
)LUA";

// 预留的 Redis 端 ML 统计更新 Lua 脚本：当前实现走 C++ 端 JSON 读改写
// (见 mlReportOutcome)，此脚本尚未接线，[[maybe_unused]] 以保留供后续切换。
[[maybe_unused]] const char* kMLUpdateLua = R"LUA(
local key = KEYS[1]
local latency = tonumber(ARGV[1])
local success = tonumber(ARGV[2])
local alpha = tonumber(ARGV[3])

local data = redis.call('HMGET', key, 'avg_latency_ms', 'success_rate', 'sample_count')
local avg_lat = tonumber(data[1]) or 100.0
local srate = tonumber(data[2]) or 1.0
local count = tonumber(data[3]) or 0

if count == 0 then
    avg_lat = latency
    srate = success
else
    avg_lat = (1.0 - alpha) * avg_lat + alpha * latency
    srate = (1.0 - alpha) * srate + alpha * success
end
count = count + 1

redis.call('HMSET', key,
    'avg_latency_ms', tostring(avg_lat),
    'success_rate', tostring(srate),
    'sample_count', tostring(count))
redis.call('EXPIRE', key, 86400)
return 1
)LUA";

// TASK-20260711-02 / TASK-20260701-01 P0-C-BAK D3 Option C — atomic
// circuit-breaker failure recorder. Combines HINCRBY counter increment,
// conditional HSET state transition (HalfOpen->Open on any failure;
// Closed->Open when count reaches threshold), timestamp stamp, and TTL
// refresh in one indivisible EVAL. Eliminates the lost-update race that
// used to force computeCircuitState()'s C6 FIX to synthesize Open from
// (count >= threshold) at read time — the state field is now authoritative.
//
// Hash schema (matches kMLUpdateLua HMGET/HMSET family for redis-cli
// observability): state (0=Closed, 1=Open, 2=HalfOpen),
// failure_count, half_open_calls, last_failure_ms.
//
// Deployment migration: previous JSON encoding is replaced by hash fields.
// Redis key TTL 3600s guarantees old JSON keys expire naturally on
// single-node deployments; multi-node rolling upgrades must complete
// within TTL to avoid mixed format (CHANGELOG carries the warning).
const char* kCircuitFailureLua = R"LUA(
local key = KEYS[1]
local threshold = tonumber(ARGV[1])
local now_ms = tonumber(ARGV[2])
local ttl = tonumber(ARGV[3])

local count = redis.call('HINCRBY', key, 'failure_count', 1)
local state = tonumber(redis.call('HGET', key, 'state') or '0')

if state == 2 then
    redis.call('HSET', key, 'state', 1)
elseif state == 0 and count >= threshold then
    redis.call('HSET', key, 'state', 1)
end

redis.call('HSET', key, 'last_failure_ms', now_ms)
redis.call('EXPIRE', key, ttl)
return count
)LUA";

// 预留的 EVALSHA 脚本加载助手：当前 rate-limit/ML 路径未使用 SCRIPT LOAD，
// [[maybe_unused]] 以保留供后续接入 Lua 脚本时使用。
[[maybe_unused]] std::string loadScript(RedisCacheStore* /*redis*/, redisContext* ctx, const char* script) {
    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx, "SCRIPT LOAD %b", script, strlen(script)));
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        std::string err = reply ? reply->str : "null reply";
        if (reply) freeReplyObject(reply);
        throw std::runtime_error("SCRIPT LOAD failed: " + err);
    }
    std::string sha(reply->str, reply->len);
    freeReplyObject(reply);
    return sha;
}

}  // namespace

RedisStateStore::RedisStateStore(RedisCacheStore* redis) : redis_(redis) {}

int64_t RedisStateStore::nowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string RedisStateStore::uniqueId() const {
    static thread_local std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::ostringstream oss;
    oss << nowMs() << "-" << rng();
    return oss.str();
}

bool RedisStateStore::initialize() {
    if (!redis_ || !redis_->isHealthy()) {
        spdlog::error("RedisStateStore: Redis not available");
        return false;
    }

    try {
        auto val = redis_->get("__aegisgate_ping__");
        (void)val;
    } catch (...) {
        spdlog::error("RedisStateStore: Redis connectivity check failed");
        return false;
    }

    spdlog::info("RedisStateStore: initialized (Lua scripts loaded on demand)");
    return true;
}

// --- Rate Limiter ---

bool RedisStateStore::rateLimitAllow(const std::string& key, double cost,
                                       double max_tokens, double refill_rate) {
    try {
        std::string rkey = std::string(kRateLimitPrefix) + key;
        auto now = nowMs();

        // evalInt() passes the key verbatim as KEYS[1] while get()/set() add
        // the RedisCacheStore namespace prefix. Prefix here so the EVAL and the
        // subsequent rateLimitRemaining() GET address the same physical key.
        std::string physical_key = std::string(RedisCacheStore::kNamespacePrefix) + rkey;

        // Single atomic EVAL: no cross-node read-modify-write race (P0-C).
        auto res = redis_->evalInt(
            kRateLimitLua, physical_key,
            {std::to_string(max_tokens), std::to_string(refill_rate),
             std::to_string(cost), std::to_string(now)});

        if (!res) {
            // EVAL unavailable (Redis down / not initialized). Preserve the
            // documented fail-open posture rather than blocking traffic.
            spdlog::warn("RedisStateStore::rateLimitAllow: EVAL unavailable — allowing");
            return true;
        }
        return *res == 1;
    } catch (const std::exception& e) {
        spdlog::warn("RedisStateStore::rateLimitAllow failed: {} — allowing", e.what());
        return true;
    }
}

double RedisStateStore::rateLimitRemaining(const std::string& key,
                                             double max_tokens, double refill_rate) {
    try {
        std::string rkey = std::string(kRateLimitPrefix) + key;
        auto stored = redis_->get(rkey);
        if (!stored) return max_tokens;

        auto j = nlohmann::json::parse(*stored, nullptr, false);
        if (j.is_discarded()) return max_tokens;

        double tokens = j.value("tokens", max_tokens);
        int64_t last_refill = j.value("last_refill", nowMs());
        double elapsed = static_cast<double>(nowMs() - last_refill) / 1000.0;
        return std::min(max_tokens, tokens + elapsed * refill_rate);
    } catch (...) {
        return max_tokens;
    }
}

// --- Circuit Breaker ---
//
// TASK-20260711-02 / TASK-20260701-01 P0-C-BAK D3 Option C: circuit-breaker
// state is stored as a Redis hash (state, failure_count, half_open_calls,
// last_failure_ms) rather than a JSON blob. Aligns with the kMLUpdateLua
// HMGET/HMSET family and lets operators inspect state directly with
// `redis-cli HGETALL aegisgate:cb:<model>`. cbRecordFailure runs an atomic
// Lua EVAL so concurrent multi-node writes do not lose updates and the
// state field is authoritatively set to Open when the counter reaches
// threshold (previously the state field stayed at 0 with Open only
// implicit via computeCircuitState()'s C6 FIX).

namespace {
// Read a raw hash field via EXECUTE(HGET); returns std::optional<int64_t>.
std::optional<int64_t> hgetInt(RedisCacheStore* redis,
                                const std::string& physical_key,
                                const char* field) {
    auto raw = redis->executeRaw(std::string("HGET ") + physical_key + " " + field);
    if (!raw) return std::nullopt;
    try {
        return std::stoll(*raw);
    } catch (...) {
        return std::nullopt;
    }
}
} // namespace

void RedisStateStore::cbRecordSuccess(const std::string& model) {
    try {
        std::string rkey = std::string(kCircuitPrefix) + model;
        std::string physical_key =
            std::string(RedisCacheStore::kNamespacePrefix) + rkey;
        // Reset via DEL — a missing key is naturally interpreted as
        // (state=Closed, failure_count=0, half_open_calls=0) by cbGetState.
        // Simpler than HMSET-with-zero and free of stale field residue.
        redis_->executeRaw("DEL " + physical_key);
    } catch (const std::exception& e) {
        spdlog::warn("RedisStateStore::cbRecordSuccess failed: {}", e.what());
    }
}

void RedisStateStore::cbRecordFailure(const std::string& model,
                                        int failure_threshold) {
    try {
        std::string rkey = std::string(kCircuitPrefix) + model;
        std::string physical_key =
            std::string(RedisCacheStore::kNamespacePrefix) + rkey;
        auto now = nowMs();
        // Single atomic EVAL: HINCRBY count + conditional HSET state=Open
        // + timestamp stamp + TTL refresh, all indivisible so concurrent
        // multi-node writes do not lose updates (mirrors kRateLimitLua
        // P0-C atomic fix).
        auto res = redis_->evalInt(
            kCircuitFailureLua, physical_key,
            {std::to_string(failure_threshold),
             std::to_string(now),
             std::to_string(3600)});
        if (!res) {
            spdlog::warn("RedisStateStore::cbRecordFailure: EVAL unavailable — no-op");
        }
    } catch (const std::exception& e) {
        spdlog::warn("RedisStateStore::cbRecordFailure failed: {}", e.what());
    }
}

// C6: 熔断状态判定纯逻辑（不依赖 redis 连接）。
CircuitState computeCircuitState(int state, int failure_count,
                                 int64_t last_failure_ms, int64_t now_ms,
                                 int failure_threshold, int reset_timeout_s) {
    if (state == 1) {  // 显式 Open
        auto elapsed_s = (now_ms - last_failure_ms) / 1000;
        if (elapsed_s >= reset_timeout_s) return CircuitState::HalfOpen;
        return CircuitState::Open;
    }
    if (state == 2) return CircuitState::HalfOpen;
    if (failure_count >= failure_threshold) {
        // C6 FIX: Closed 累积到阈值等价于逻辑 Open，同样受 reset_timeout 支配，
        // 否则永停 Open 只能靠 key TTL 恢复（cbRecordFailure 已写 last_failure_ms）。
        auto elapsed_s = (now_ms - last_failure_ms) / 1000;
        if (elapsed_s >= reset_timeout_s) return CircuitState::HalfOpen;
        return CircuitState::Open;
    }
    return CircuitState::Closed;
}

CircuitState RedisStateStore::cbGetState(const std::string& model,
                                           int failure_threshold,
                                           int reset_timeout_s) {
    try {
        std::string rkey = std::string(kCircuitPrefix) + model;
        std::string physical_key =
            std::string(RedisCacheStore::kNamespacePrefix) + rkey;
        // Three separate HGETs — read-side atomicity is intentionally
        // relaxed. If another node's HINCRBY interleaves, the worst-case
        // outcome is a one-tick stale read of the aggregate state, which
        // the very next cbAllowRequest / caller iteration will correct.
        // We retain computeCircuitState() as defense-in-depth so a rare
        // partially-written state (state=0 with count>=threshold before
        // HSET fires) still resolves to Open.
        auto state_i = hgetInt(redis_, physical_key, "state");
        if (!state_i) return CircuitState::Closed;  // key absent -> Closed
        auto count_i = hgetInt(redis_, physical_key, "failure_count");
        auto last_i = hgetInt(redis_, physical_key, "last_failure_ms");

        return computeCircuitState(
            static_cast<int>(*state_i),
            count_i ? static_cast<int>(*count_i) : 0,
            last_i ? *last_i : 0,
            nowMs(),
            failure_threshold, reset_timeout_s);
    } catch (...) {
        return CircuitState::Closed;
    }
}

bool RedisStateStore::cbAllowRequest(const std::string& model,
                                       int failure_threshold,
                                       int reset_timeout_s,
                                       int half_open_max) {
    try {
        auto state = cbGetState(model, failure_threshold, reset_timeout_s);

        if (state == CircuitState::Closed) return true;
        if (state == CircuitState::Open) return false;

        // HalfOpen path: increment half_open_calls and cap at half_open_max.
        std::string rkey = std::string(kCircuitPrefix) + model;
        std::string physical_key =
            std::string(RedisCacheStore::kNamespacePrefix) + rkey;
        auto ho_before = hgetInt(redis_, physical_key, "half_open_calls");
        int ho_calls = ho_before ? static_cast<int>(*ho_before) : 0;
        if (ho_calls < half_open_max) {
            // HSET the transition explicitly (also flips state=2 in case
            // the field is still stored as Open — HalfOpen is our current
            // effective state).
            redis_->executeRaw("HSET " + physical_key + " state 2");
            redis_->executeRaw("HINCRBY " + physical_key + " half_open_calls 1");
            redis_->executeRaw("EXPIRE " + physical_key + " 3600");
            return true;
        }
        return false;
    } catch (...) {
        return true;
    }
}

// --- Abuse Detector ---

void RedisStateStore::abuseRecordRejection(const std::string& key,
                                              int window_seconds) {
    try {
        std::string rkey = std::string(kAbusePrefix) + key;
        auto now = nowMs();
        auto uid = uniqueId();

        auto stored = redis_->get(rkey);
        nlohmann::json arr = nlohmann::json::array();
        if (stored) {
            arr = nlohmann::json::parse(*stored, nullptr, false);
            if (arr.is_discarded() || !arr.is_array()) arr = nlohmann::json::array();
        }

        auto cutoff = now - static_cast<int64_t>(window_seconds) * 1000;
        nlohmann::json filtered = nlohmann::json::array();
        for (const auto& ts : arr) {
            if (ts.is_number() && ts.get<int64_t>() >= cutoff) {
                filtered.push_back(ts);
            }
        }
        filtered.push_back(now);

        redis_->set(rkey, filtered.dump(),
                     std::chrono::seconds(window_seconds * 2));
    } catch (const std::exception& e) {
        spdlog::warn("RedisStateStore::abuseRecordRejection failed: {}", e.what());
    }
}

int RedisStateStore::abuseGetCount(const std::string& key, int window_seconds) {
    try {
        std::string rkey = std::string(kAbusePrefix) + key;
        auto stored = redis_->get(rkey);
        if (!stored) return 0;

        auto arr = nlohmann::json::parse(*stored, nullptr, false);
        if (arr.is_discarded() || !arr.is_array()) return 0;

        auto cutoff = nowMs() - static_cast<int64_t>(window_seconds) * 1000;
        int count = 0;
        for (const auto& ts : arr) {
            if (ts.is_number() && ts.get<int64_t>() >= cutoff) count++;
        }
        return count;
    } catch (...) {
        return 0;
    }
}

bool RedisStateStore::abuseIsBlocked(const std::string& key) {
    try {
        std::string rkey = std::string(kAbuseBlockPrefix) + key;
        return redis_->exists(rkey);
    } catch (...) {
        return false;
    }
}

void RedisStateStore::abuseSetBlocked(const std::string& key, int duration_seconds) {
    try {
        std::string rkey = std::string(kAbuseBlockPrefix) + key;
        redis_->set(rkey, "1", std::chrono::seconds(duration_seconds));
    } catch (const std::exception& e) {
        spdlog::warn("RedisStateStore::abuseSetBlocked failed: {}", e.what());
    }
}

// --- ML Router Stats ---

void RedisStateStore::mlReportOutcome(const std::string& model,
                                        double latency_ms, bool success) {
    try {
        std::string rkey = std::string(kMLStatsPrefix) + model;
        auto stored = redis_->get(rkey);

        double avg_lat = 100.0;
        double srate = 1.0;
        int count = 0;

        if (stored) {
            auto j = nlohmann::json::parse(*stored, nullptr, false);
            if (!j.is_discarded()) {
                avg_lat = j.value("avg_latency_ms", 100.0);
                srate = j.value("success_rate", 1.0);
                count = j.value("sample_count", 0);
            }
        }

        double sv = success ? 1.0 : 0.0;
        if (count == 0) {
            avg_lat = latency_ms;
            srate = sv;
        } else {
            avg_lat = (1.0 - kEmaAlpha) * avg_lat + kEmaAlpha * latency_ms;
            srate = (1.0 - kEmaAlpha) * srate + kEmaAlpha * sv;
        }
        count++;

        nlohmann::json j;
        j["avg_latency_ms"] = avg_lat;
        j["success_rate"] = srate;
        j["sample_count"] = count;
        redis_->set(rkey, j.dump(), std::chrono::seconds(86400));
    } catch (const std::exception& e) {
        spdlog::warn("RedisStateStore::mlReportOutcome failed: {}", e.what());
    }
}

RedisStateStore::MLStats RedisStateStore::mlGetStats(const std::string& model) {
    try {
        std::string rkey = std::string(kMLStatsPrefix) + model;
        auto stored = redis_->get(rkey);
        if (!stored) return {};

        auto j = nlohmann::json::parse(*stored, nullptr, false);
        if (j.is_discarded()) return {};

        return {
            j.value("avg_latency_ms", 100.0),
            j.value("success_rate", 1.0),
            j.value("sample_count", 0)
        };
    } catch (...) {
        return {};
    }
}

}  // namespace aegisgate

#endif  // AEGISGATE_ENABLE_REDIS
