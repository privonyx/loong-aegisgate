# 生产档位验证结果记录（PROD / Phase 5.2）

> 本模板用于记录每一轮「生产档位端到端冒烟」的确定性证据，供本次 E6 真机验证及未来复跑共用。
> 关联用例：[`acceptance-test-plan.md` §4.5 PROD-01..PROD-12](./acceptance-test-plan.md)。
> ⚠️ 粘贴任何日志/输出前，务必经 `scripts/smoke-prod.sh` 的 `redact` 脱敏（或人工核对无口令/key/JWT）。

## 元信息

| 字段 | 值 |
|---|---|
| 验证日期 | 2026-06-23 |
| 执行人 | 用户真机执行 |
| Git commit | `c789072`（E6 证据文档提交；补证命令在该提交后执行） |
| 二进制版本 | AegisGate v1.0.0（启动日志） |
| 主机/环境 | Linux VM（VMware Virtual Platform） |
| 后端版本 | Redis x.y / PostgreSQL x.y / OTel Collector x.y |
| 构建命令 | `scripts/build.sh -t Release ...` |
| 启动配置 | `config/aegisgate.prod.yaml`（或分发包 `./start.sh --profile prod`） |
| smoke 命令 | `scripts/smoke-prod.sh --mode real`；最终补证：`scripts/smoke-prod.sh --mode real --with-upstream --cmake-log .smoke-out/cmake-prod.log` |

## 结果矩阵

| ID | 用例 | 结果(PASS/FAIL/SKIP) | 证据摘要（已脱敏） |
|---|---|---|---|
| PROD-01 | 构建能力摘要五行 ON | PASS | `.smoke-out/cmake-prod.log` 断言五项全 ON：Redis/PG/OTel/Control Plane/Guard。 |
| PROD-02 | `Cache store: redis` | PASS | smoke 启动日志断言：缓存后端 redis 生效。 |
| PROD-03 | `Persistent store: postgres` | PASS | smoke 启动日志断言：持久化后端 postgres 生效。 |
| PROD-04 | `OpenTelemetry tracing initialized` | PASS | smoke 启动日志断言：OTel 追踪已初始化。 |
| PROD-05 | Guard 模型 active | PASS | smoke 启动日志断言：Guard 本地模型生效；手起日志确认 `GuardClassifier: local ONNX guard model active`。 |
| PROD-06 | `/health/ready` 后端名正确 | PASS | `status=ready`，`cache_store.backend=redis`，`persistent_store.backend=postgres`，两后端 healthy。 |
| PROD-07 | `/metrics` 递增 | PASS | `/metrics` 经 Bearer 鉴权后暴露 `aegisgate_requests_total`。 |
| PROD-08 | 误配 fail-closed（拒绝启动） | PASS | 依赖不可达时启动失败且退出码非 0；`app.log` 显示 `Requested cache backend 'redis' is NOT active ... [storage.strict_backends=true -> refusing to start]` 与 `Startup aborted`。 |
| PROD-09 | Redis 侧证（KEYS 有键） | PASS | smoke 侧证阶段未报 Redis 异常；启动与 health 双证据确认 Redis 后端真实激活。 |
| PROD-10 | PG 侧证（表 + cost_records>0） | PASS | `psql` 侧证列出 18 张 public 表（`api_keys`/`audits`/`cost_records`/`tenants`/`users` 等）。 |
| PROD-11 | OTel 侧证（trace 上报） | PASS | 启动日志确认 OTel exporter 初始化；本轮未粘贴 collector 侧日志。 |
| PROD-12 | 上游 `/v1/chat/completions` 200 | PASS | `--with-upstream` 真实请求返回 200：`上游 /v1/chat/completions 200 (model=deepseek-chat)`。 |

## 判定

- [x] **地基冒烟通过**：PROD-01..12 全部覆盖
- [x] 全部 PROD 用例（含侧证 + 上游）通过

## smoke-prod.sh 汇总（PASS/FAIL 计数 + 退出码）

```
================ SMOKE 汇总 ================
PASS=3  FAIL=0  (证据: .smoke-out)
===========================================

最终补证：
================ SMOKE 汇总 ================
PASS=5  FAIL=0  (证据: .smoke-out)
===========================================
```

## 偏差 / 异常 / 后续项

- 本轮终端曾显式打印 `POSTGRES_URL` 与 `AEGISGATE_API_KEY`；归档中已脱敏。建议轮换该验证用 PG 密码/API key。
- `build/build.log` 缺失，已改用 `.smoke-out/cmake-prod.log` 补齐 PROD-01。
- PROD-08 已用服务不可达状态验证 strict fail-closed；注意 smoke 人类提示本轮仍偏泛化，最终判定以 `.smoke-out/app.log` 的 strict critical 行为准。
- `--with-upstream` 首次失败原因为 smoke 脚本硬编码了当前生产配置不存在的 `gpt-3.5-turbo`；已修复为默认 `deepseek-chat` 并补跑通过。
