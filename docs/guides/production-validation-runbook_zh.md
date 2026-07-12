<!-- AUTO-LANG: zh; en counterpart: production-validation-runbook.md -->
# 生产环境验证 Runbook（Phase 5.2）

[English](./production-validation-runbook.md)

一份可重复、可复制粘贴的命令清单，把 AegisGate 从源码推进到「已验证的生产档位部署」
（Redis + PostgreSQL + OpenTelemetry + Guard + Control Plane），产出确定性证据证明每个后端
**真实生效**——而非静默回退 memory/sqlite。验收用例见
[`acceptance-test-plan.md` §4.5 PROD-*](../acceptance/acceptance-test-plan.md)。

## 概述

验证分两层（创意 C1 / plan E4）：
- **Layer 1（每 PR / 阻塞）：** 原生构建 + Redis/PG service container + `smoke-prod.sh --mode ci`，落在 `.github/workflows/ci-prod-smoke.yml`。
- **Layer 2（nightly / 按需 / 不阻塞）：** Docker Compose 全栈 + 真实 Guard 模型 + OTel 真 export 侧证（见末节）。

本 runbook 是手动 / 真机路径（深度等同 Layer 2），每个发布版跑一次。

## 前置条件

- 一台带构建工具链且 `vcpkg` 可用的 Linux 主机（见 `scripts/build.sh -t Release`）。
- vcpkg 依赖与（可选）Guard 模型下载所需的网络。
- 能本地运行（或可达）Redis、PostgreSQL、OTel Collector。
- 一个上游 LLM 供应商 key（用于 `--with-upstream` 全链路检查）。

## 步骤一 — 构建（生产档位）

```bash
# 安装 vcpkg 从源码编译 libpq 所需的构建工具。build.sh 会先检查这些工具，
# 缺失时会在长时间 vcpkg 构建前直接退出。
sudo apt-get update
sudo apt-get install -y autoconf automake libtool autoconf-archive bison flex

# 构建全能力二进制（Redis + PG + OTel + Guard + Control Plane）。
# tee 输出以便 smoke-prod.sh 断言能力摘要。
bash scripts/build.sh -t Release 2>&1 | tee build/build.log
```

确认能力摘要出现五行 `[ON ]`：`ENABLE_REDIS`、`ENABLE_PG`、`ENABLE_OPENTELEMETRY`、
`ENABLE_CONTROL_PLANE`、`ENABLE_GUARD_MODEL`。

## 步骤二 — 部署后端依赖

```bash
# 安装/启动 Redis、PostgreSQL、OTel Collector（含加固默认：PG 随机口令、
# 凭据文件 chmod 600）。
sudo bash scripts/setup-prod-deps.sh
```

如缺 Guard 模型则下载（PROD-05 `active` 需要）：

```bash
bash scripts/download_guard_model.sh models/guard
```

## 步骤三 — 配置

导出敏感值（切勿入库）并使用生产配置模板：

```bash
export AEGISGATE_API_KEY="your-gateway-key"
export AEGISGATE_ADMIN_JWT_SECRET="$(openssl rand -hex 32)"
export POSTGRES_URL="postgres://aegisgate:<口令>@127.0.0.1:5432/aegisgate"
export REDIS_PASSWORD="<redis 口令，如有>"   # 无口令则不设
# config/aegisgate.prod.yaml 已请求 redis + postgres + otel + guard。
# 或直接读入 setup-prod-deps.sh 生成的凭据（已用 export，子进程可继承）：
#   source ~/.aegisgate_prod_creds   # 注入 POSTGRES_URL / PG_PASSWORD
```

> ⚠️ `POSTGRES_URL` 等**必须 `export`** 为环境变量——网关经 `getenv` 读取，
> 仅 `VAR=...` 的 shell 局部变量子进程看不到，会解析为空 → PG 连接失败 →
> strict 模式拒绝启动。

> 保持 `storage.strict_backends: true`（默认）。验证时**不要**设为 `false`——
> 你要的就是「误配立即响亮失败」。

## 步骤四 — 启动

```bash
# 源码树：
./build/src/aegisgate config/aegisgate.prod.yaml
# 或分发包：
#   ./start.sh --profile prod        （或：AEGISGATE_PROFILE=prod ./start.sh）
```

观察启动日志出现 `Cache store: redis`、`Persistent store: postgres`、
`OpenTelemetry tracing initialized`、`GuardClassifier: local ONNX guard model active`。

## 步骤五 — 运行冒烟证据包

```bash
bash scripts/smoke-prod.sh --mode real --with-upstream \
  --config config/aegisgate.prod.yaml \
  --binary build/src/aegisgate \
  --cmake-log build/build.log \
  --host 127.0.0.1 --port 8080
```

退出码 `0` = 全部断言 PASS。所有输出与 `--out` 证据文件均经脱敏 + `chmod 600`；
默认 `.smoke-out/` 目录已 git-ignore。

## 步骤六 — 侧证（real 模式）

```bash
redis-cli KEYS '*' | head          # 有键写入
psql "$POSTGRES_URL" -c '\dt'      # 有表
psql "$POSTGRES_URL" -c 'SELECT count(*) FROM cost_records;'   # 产生流量后 > 0
# OTel：在 Collector/Tempo 查 aegisgate 的 trace（必要时把 sample_ratio 调到 1.0）。
```

## fail-closed 行为（G1）

`storage.strict_backends: true`（默认）下，若 YAML 请求 `redis`/`postgres` 但二进制未编入
对应后端、或后端启动时不可达，进程将**拒绝启动**（critical 日志 + 非零退出），而非静默
回退 memory。验证 PROD-08：停掉 Redis 再启动，启动必须以清晰信息中止。仅当你明确需要
「降级但保持可用」时才设 `storage.strict_backends: false`。

## 记录结果

复制 [`prod-validation-result-template.md`](../acceptance/prod-validation-result-template.md)，
用脱敏证据填 PROD-01..PROD-12 矩阵，并附上 `smoke-prod.sh` 汇总块。

## Layer 2 — nightly 全栈（不阻塞）

更重的检查（真实 Guard 模型 active + 真实 OTel export 到 Tempo），用 Docker Compose 全栈
定时或 `workflow_dispatch` 跑：

```bash
docker compose up -d redis postgres tempo aegisgate
docker compose logs aegisgate | grep -E "Cache store|Persistent store|guard model active|tracing initialized"
```

这些不阻塞 PR；失败仅告警，次个工作日排查。
