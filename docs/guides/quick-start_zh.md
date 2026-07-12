# AegisGate 快速开始

本指南帮助你在本机编译、配置并发起第一次 API 调用。默认网关地址为 `http://127.0.0.1:8080`（与 `config/aegisgate.yaml` 中 `server.port` 一致）。

## 前置条件

- **编译器**：支持 C++17（GCC 11+ 或 Clang 14+）
- **CMake**：3.20 及以上
- **[vcpkg](https://github.com/microsoft/vcpkg)**：用于拉取 Drogon、yaml-cpp、RE2 等依赖（仓库根目录 `vcpkg.json` 为清单模式）

可选：

- **ONNX 语义嵌入**：需要单独下载 ONNX Runtime 与模型，见项目根目录 `README.md` 中「Optional: ONNX Neural Embedder」；未启用时网关可使用哈希嵌入降级运行。

## 获取源码并编译

```bash
git clone https://github.com/privonyx/loong-aegisgate.git
cd aegisgate

cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

构建产物：

- 网关：`build/src/aegisgate`（或你指定的 `build` 目录）
- 管理 CLI：`build/aegisctl`（若目标已启用）

运行测试（可选）：

```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
cd build && ctest --output-on-failure
```

## 最小配置说明

主配置：`config/aegisgate.yaml`  
模型与上游：`config/models.yaml`（由主配置中 `models_config` 指定）

**1. 设置网关 API Key（推荐环境变量）**

```bash
export AEGISGATE_API_KEY="sk-your-gateway-key"
```

在 `aegisgate.yaml` 中保持：

```yaml
auth:
  enabled: true
  api_keys:
    - "${AEGISGATE_API_KEY}"
```

**2. 在 `config/models.yaml` 中保留至少一个 Provider**

例如仅启用 OpenAI（将密钥放在环境变量中，勿写入仓库）：

```yaml
providers:
  - name: openai
    type: openai
    base_url: "https://api.openai.com/v1"
    api_keys:
      - key: "${OPENAI_API_KEY}"
        weight: 1
    models:
      - id: "gpt-4o-mini"
        cost_per_1k_input: 0.00015
        cost_per_1k_output: 0.0006
        max_tokens: 128000
    timeout_ms: 30000
    max_retries: 2
```

```bash
export OPENAI_API_KEY="sk-..."
```

**3.（可选）管理员 Key**

访问 `/admin/reload`、`/admin/logs/stream` 等需配置：

```yaml
auth:
  admin_key: "${AEGISGATE_ADMIN_KEY}"
```

## 启动网关

```bash
./build/src/aegisgate config/aegisgate.yaml
```

确认健康检查：

```bash
curl -s http://127.0.0.1:8080/health | jq .
```

## 第一次 Chat 调用（curl）

```bash
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer ${AEGISGATE_API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "gpt-4o-mini",
    "messages": [{"role": "user", "content": "用一句话介绍 AegisGate"}],
    "max_tokens": 256
  }' | jq .
```

列出模型：

```bash
curl -s http://127.0.0.1:8080/v1/models \
  -H "Authorization: Bearer ${AEGISGATE_API_KEY}" | jq .
```

## 使用官方 SDK

### Python

```bash
pip install -e /path/to/aegisgate/sdk/python
# 或 uv pip install -e ...
```

```python
from aegisgate import AegisGateClient

client = AegisGateClient(
    api_key="sk-your-gateway-key",
    base_url="http://127.0.0.1:8080",
)
r = client.chat("你好", model="gpt-4o-mini")
print(r.content)
```

### Node.js

```bash
npm install @aegisgate/sdk
```

```typescript
import { AegisGateClient } from '@aegisgate/sdk';

const client = new AegisGateClient({
  apiKey: 'sk-your-gateway-key',
  baseUrl: 'http://127.0.0.1:8080',
});
const res = await client.chat('你好', { model: 'gpt-4o-mini' });
console.log(res.content);
```

### Go

```bash
go get github.com/privonyx/loong-aegisgate-go
```

```go
client := aegisgate.NewClient(
    aegisgate.WithBaseURL("http://127.0.0.1:8080"),
    aegisgate.WithAPIKey("sk-your-gateway-key"),
)
resp, err := client.ChatCompletions(ctx, &aegisgate.ChatCompletionsRequest{
    Model: "gpt-4o-mini",
    Messages: []aegisgate.Message{{Role: "user", Content: "你好"}},
})
```

## CLI 速览（aegisctl）

```bash
export AEGISGATE_URL="http://127.0.0.1:8080"
export AEGISGATE_API_KEY="sk-your-gateway-key"

./build/aegisctl health
./build/aegisctl models
./build/aegisctl chat "ping"
./build/aegisctl config validate config/aegisgate.yaml
```

管理接口需使用**管理员** Key：`./build/aegisctl --api-key "$AEGISGATE_ADMIN_KEY" logs tail`。

## 配置智能路由（可选）

### 启用 ML 路由

在 `config/aegisgate.yaml` 中添加：

```yaml
routing:
  type: ml                # basic | cost_aware | ml
  ml:
    cost_weight: 0.4      # 成本权重
    quality_weight: 0.35  # 质量权重
    latency_weight: 0.25  # 延迟权重
```

ML 路由会根据各模型的实际成本、历史成功率和响应延迟动态选择最优模型。

### 启用 A/B 测试

对比两个模型的性价比：

```yaml
routing:
  type: cost_aware
  ab_tests:
    - name: "gpt4o-vs-deepseek"
      variants:
        - model: "gpt-4o-mini"
          weight: 50
        - model: "deepseek-chat"
          weight: 50
      enabled: true
```

发送不带 `model` 字段的请求时，50% 流量会路由到 gpt-4o-mini，50% 路由到 deepseek-chat。分配结果可在成本分析面板中对比。

### 使用 quality_tier

客户端可通过 `extra` 字段请求特定价位的模型：

```bash
# 经济模式：选最便宜的可用模型
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer ${AEGISGATE_API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{
    "messages": [{"role": "user", "content": "你好"}],
    "extra": {"quality_tier": "economy"}
  }' | jq .

# 高级模式：选最贵（通常质量最好）的模型
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer ${AEGISGATE_API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{
    "messages": [{"role": "user", "content": "写一篇专业分析报告"}],
    "extra": {"quality_tier": "premium"}
  }' | jq .
```

## 落地参考 Demo（Showcase App）

想看完整链路——**大模型 → AegisGate → 应用**？仓库在 [`apps/showcase`](../../apps/showcase/README_zh.md) 提供了一个可运行的参考 Demo，把网关价值（🛡️ 护栏拦截、💰 语义缓存省量、🔀 智能路由、📊 观测聚合）做到肉眼可见，并以 AI 漫剧创作台作为旗舰场景。

**前置**

- 已运行的 AegisGate 网关（见上文[启动网关](#启动网关)），并在网关侧配置好 Provider Key。
- Node.js ≥ 22（仅手动开发模式需要）。

**启动（Docker）**

```bash
cp apps/showcase/.env.example apps/showcase/.env
# 编辑 .env，填入 AEGISGATE_API_KEY（由网关签发）
cd apps/showcase && docker compose up --build
# 访问 http://localhost:5173
```

**启动（手动开发）**

```bash
cp apps/showcase/.env.example apps/showcase/.env   # 填入 AEGISGATE_API_KEY

cd apps/showcase/backend && npm install && npm run dev    # :8090
cd apps/showcase/frontend && npm install && npm run dev   # :5173（已代理 /api → :8090）
```

Demo 仅持有 AegisGate Key（从不接触任何 Provider Key），前端从不接触 Key。价值演示脚本、场景插件、环境变量与安全约定见 [Showcase README](../../apps/showcase/README_zh.md)。

## 数据、配置与升级（分发包）

> 本节针对用 `scripts/package.sh` 产出的分发包（`tar.gz`）部署的场景。源码编译方式同样适用其中的路径说明。

**数据与日志默认落点（均相对运行目录，可在 `config/aegisgate.yaml` 配置）**

| 内容 | 默认路径 | 配置项 |
|---|---|---|
| SQLite 数据库 | `data/aegisgate.db` | `storage.sqlite.path` |
| 应用日志 | `logs/aegisgate.log` | `logging.file` |
| 审计日志 | `logs/audit.log` | `audit.log_path` |

`data/`、`logs/`、`models/` 都在运行时按需创建/下载，**不随分发包打包**，因此升级时不会被覆盖。

**配置文件（首启自动生成，永不被升级覆盖）**

分发包内的配置以 `*.example` 出厂模板形式提供（如 `config/aegisgate.yaml.example`、`config/models.yaml.example`、`config/rules/*.yaml.example`）。`start.sh` 首次启动时，会对**缺失**的真实配置从同名模板生成；已存在的则原样保留。真实配置（`config/*.yaml`）**从不在分发包内**。

**升级（解压即替换，零顾虑）**

直接把新版本解压覆盖到当前部署目录即可，无需先删除旧目录：

```bash
tar xzf aegisgate-<新版本>-<os>-<arch>.tar.gz --strip-components=1 -C <部署目录>
```

- 会更新：`aegisgate` / `aegisctl` / `lib/` / `web/` / `scripts/` / `start.sh` 等程序文件，以及 `config/*.example` 出厂模板。
- 不会动：你的 `config/*.yaml`、`config/rules/*.yaml`，以及 `data/`、`logs/`、`models/`。
- 想对照新版默认配置：比较 `config/aegisgate.yaml` 与 `config/aegisgate.yaml.example`。

**以生产档位运行（Redis + PostgreSQL + OpenTelemetry + Guard）**

默认档位是 community（内存缓存 + SQLite），适合单机/试用。若分发包由生产档位构建（`scripts/build.sh -t Release`，Redis/PG/OTel/Guard 全部编入），可用 **prod 档位**运行——分发包已携带 `config/aegisgate.prod.yaml.example`：

```bash
# 方式一：旗标
./start.sh --profile prod

# 方式二：环境变量
AEGISGATE_PROFILE=prod ./start.sh
```

`start.sh` 会首启 seeding 生成 `config/aegisgate.prod.yaml`（已存在则保留），并以该配置启动。请先备好 Redis/PostgreSQL/OTel Collector（参考 `scripts/setup-prod-deps.sh`）并在配置/环境变量中填好连接信息。

> ⚠️ **fail-closed（默认行为）：** 生产档位默认 `storage.strict_backends: true`——若 YAML 请求 `redis`/`postgres` 但二进制未编入对应后端、或后端启动时不可达，进程将**拒绝启动**（非零退出 + 清晰日志），而非静默回退 memory。需要「后端故障时仍降级可用」时，显式设 `storage.strict_backends: false`。

可用 `scripts/smoke-prod.sh` 一键验证各后端真实生效：

```bash
bash scripts/smoke-prod.sh --mode real --cmake-log build/build.log
```

## 下一步

- [架构指南](./architecture_zh.md) — 系统全景、流程图、时序图
- [成本优化指南](./cost-optimization_zh.md) — 省钱策略详解
- [管理 API 参考](./admin-api_zh.md) — Admin REST API 完整参考
- [错误码参考](./error-codes_zh.md) — 理解 `AEGIS-xxxx` 与重试策略
- [安全最佳实践](./security-best-practices_zh.md) — Key、TLS、护栏规则
- [性能调优](./performance-tuning_zh.md) — 缓存、限流、线程
- [故障排查](./troubleshooting_zh.md) — 常见启动与上游问题

更完整的特性说明见项目根目录 `README.md`。
