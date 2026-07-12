# 5 分钟 quickstart

在你的笔记本上 5 分钟跑通 AegisGate — 自带 OpenAI key，启动一个带语义缓存和省钱看板的
AI 网关。

> 🇬🇧 **English:** [quickstart.md](quickstart.md)

---

## 概览

5 分钟内你会：

1. 一行 `docker run` 命令拉起 AegisGate
2. 从启动 banner 读取自动生成的 API key
3. 第一次 LLM 调用（缓存未命中）
4. 同样的请求第二次（缓存命中 — 秒级返回）
5. 查看 Savings 看板显示 `tokens_saved > 0`

---

## 前置依赖

- **Docker**（[安装](https://docs.docker.com/get-docker/)）
- **OpenAI API key**（[申请](https://platform.openai.com/api-keys)）
- **curl**（任意近期版本）

可选：

- `jq` 美化 JSON 输出
- 5 分钟专注时间

---

## Step 1 — 拉镜像并启动（~30 秒 + 镜像拉取时间）

本地构建镜像（首次）：

```bash
git clone https://github.com/privonyx/loong-aegisgate.git
cd aegisgate
docker build -t aegisgate:latest .
```

设置 OpenAI key 并启动 quickstart 容器：

```bash
export OPENAI_API_KEY=sk-...你的-openai-key...

docker run --rm -it \
  --name aegisgate-quickstart \
  -p 8080:8080 \
  -e OPENAI_API_KEY=$OPENAI_API_KEY \
  -v aegisgate-quickstart-data:/app/data \
  --entrypoint /usr/local/bin/quickstart-entrypoint.sh \
  aegisgate:latest
```

> 💡 **快捷方式：** 如果你 clone 了仓库，`bash scripts/quickstart.sh` 会替你跑同样的命令
> （需先 `export OPENAI_API_KEY`）。

---

## Step 2 — 从 banner 读取 API key（~5 秒）

容器启动会打印如下 banner：

```
╔══════════════════════════════════════════════════════════════════╗
║  ⚠️  AegisGate QUICKSTART MODE — development / demo ONLY  ⚠️    ║
║                                                                  ║
║  Quickstart API key (auto-generated):                            ║
║    AbCdEfGhIjKlMnOpQrStUvWxYz0123456789AbCdEfG
║                                                                  ║
║  Try it now (after server starts on :8080):                      ║
║    curl -H "Authorization: Bearer AbCdEfG..." \                  ║
║         http://localhost:8080/admin/api/savings/summary
║                                                                  ║
║  DO NOT use in production. See docs/quickstart.md                ║
╚══════════════════════════════════════════════════════════════════╝
```

复制 API key。也可以从持久化卷中读取：

```bash
docker exec aegisgate-quickstart cat /app/data/quickstart-key.txt
```

设置到 shell：

```bash
export QUICKSTART_KEY=AbCdEfGhIjKlMnOpQrStUvWxYz0123456789AbCdEfG
```

---

## Step 3 — 第一次 LLM 调用（缓存未命中，~1-3 秒）

打开另一个终端发起第一个请求：

```bash
time curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Authorization: Bearer $QUICKSTART_KEY" \
  -H "Content-Type: application/json" \
  -d '{"model":"gpt-4o-mini","messages":[{"role":"user","content":"Say hello in 5 words"}]}'
```

预期：标准 OpenAI 风格响应，延迟以 OpenAI 网络调用为主（通常 ~500ms – 3s）。

```json
{
  "id": "chatcmpl-...",
  "choices": [{"message": {"role": "assistant", "content": "Hello there, friend of mine!"}}],
  "usage": {"prompt_tokens": 14, "completion_tokens": 7, "total_tokens": 21}
}
```

---

## Step 4 — 同样的请求第二次（缓存命中，<10ms）

跑**完全相同**的请求：

```bash
time curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Authorization: Bearer $QUICKSTART_KEY" \
  -H "Content-Type: application/json" \
  -d '{"model":"gpt-4o-mini","messages":[{"role":"user","content":"Say hello in 5 words"}]}'
```

预期：**响应完全相同，速度快约 100 倍**（通常 <10ms）。AegisGate 识别到语义相同的请求，
直接从进程内语义缓存返回，未访问 OpenAI。

对比 Step 3 vs Step 4 的 `time` 输出 — 这个差距就是 AegisGate 为你创造的真实价值。

---

## Step 5 — 查看 Savings 看板（~10 秒）

查询节省汇总：

```bash
curl http://localhost:8080/admin/api/savings/summary \
  -H "Authorization: Bearer $QUICKSTART_KEY" | jq
```

预期输出：

```json
{
  "tokens_saved": 21,
  "cache_hits": 1,
  "cache_misses": 1,
  "hit_rate": 0.5,
  "cost_saved_usd": 0.000004,
  "since_iso": "2026-05-25T13:42:00Z"
}
```

完成了 — **5 步，5 分钟，真实数据**。多发几个请求看节省增长。

也可以访问完整 Admin UI：`http://localhost:8080/admin/`（用同样的 `QUICKSTART_KEY`
作为 Bearer token）。

---

## Step 6 — 预测你的节省（~5 秒，无需 Docker）

好奇 AegisGate 在你真实月流量下能省多少？跑接入前测算器即可，**不需要装 Docker**：

```bash
# 把数字换成你自己的月度流量和 token 大小
aegisctl estimate \
  --model gpt-4o \
  --monthly-calls 100000 \
  --avg-input-tokens 800 \
  --avg-output-tokens 200
```

样例输出（gpt-4o, 10万次/月, balanced 场景）：

```
  Cache hits (30%):           -$210.00
  Routing (20% to gpt-4o-mini): -$94.64
  Compression (10% on input):  -$28.00
  Estimated monthly savings:  -$332.64 (47.5%)
  Estimated annual savings:    $3991.68
```

测算器从 `config/models.yaml` 读取模型价格，DeepSeek / 通义 / 豆包都开箱即用。
试试 `--scenario conservative` 拿下限估算，或 `--output json` 输出机器可读报告。

📖 完整指南：[estimate_zh.md](estimate_zh.md)

---

## 常见问题

### `OPENAI_API_KEY not set` 警告

quickstart 仍会启动（方便你逛 admin UI），但 LLM 调用会失败。设置环境变量后重启：

```bash
docker rm -f aegisgate-quickstart
export OPENAI_API_KEY=sk-...
# 再跑 Step 1 的 docker run 命令
```

### 端口 8080 被占用

映射到其它端口：

```bash
docker run ... -p 18080:8080 ... aegisgate:latest
# 然后用 http://localhost:18080
```

### 看不到 banner 里的 key 了

banner 只打印一次。直接从持久化卷读：

```bash
docker exec aegisgate-quickstart cat /app/data/quickstart-key.txt
```

### `MUST NOT run in production` 报错

你设置了 `AEGISGATE_PRODUCTION=1`。quickstart entrypoint 作为安全护栏会 hard fail。
生产环境请用标准 entrypoint（不传 `--entrypoint`）：

```bash
docker run -p 8080:8080 \
  -v $(pwd)/config:/app/config:ro \
  -e AEGISGATE_API_KEY=$(openssl rand -base64 32) \
  aegisgate:latest config/aegisgate.yaml
```

### 缓存命中没变快

确保第二次请求与第一次**完全相同**（model / messages / 参数）。语义缓存 key 包含所有
请求字段。

---

## 从 quickstart 到生产

quickstart **不是**生产部署。准备转生产时：

| 关注点 | quickstart | 生产 |
|---|---|---|
| API key | 自动生成的开发 key | 强随机 key + secret manager |
| 认证 | 仅 Bearer token | 加 JWT / OIDC / SCIM（见 `config/aegisgate.yaml`）|
| 存储 | 容器内 SQLite | PostgreSQL（`-DENABLE_PG=ON`）|
| 缓存 | 进程内 | Redis（`-DENABLE_REDIS=ON`）|
| 可观测 | 基础 Prometheus | OpenTelemetry + tracing backend |
| 多租户 / RBAC | 关闭 | 开启（见完整 config 的 `rbac:` 段）|
| 绑定地址 | `0.0.0.0:8080` | 反向代理 + TLS 终结后 |

quickstart 容器在设置 `AEGISGATE_PRODUCTION=1` 时会**hard fail**，强制你显式切换到生产
entrypoint。生产指引见 README 的 "Deployment" 段。

---

## 下一步

- 阅读 [README_zh.md](../README_zh.md) 了解完整功能
- 浏览 [docs/](.) 看架构、安全、运维指南
- 在你自己的代码里安装 SDK：

  ```bash
  # Python
  pip install aegisgate

  # Node.js / TypeScript
  npm install @aegisgate/sdk
  ```

  ```python
  from aegisgate import AegisGateClient
  client = AegisGateClient(api_key="sk-xxx", base_url="http://localhost:8080")
  print(client.chat("你好").content)
  ```

  ```typescript
  import { AegisGateClient } from '@aegisgate/sdk';
  const client = new AegisGateClient({ apiKey: 'sk-xxx', baseUrl: 'http://localhost:8080' });
  console.log((await client.chat('你好！')).content);
  ```

  两个包当前发布在 `0.1.0`（Beta）版本 — 完整 API 文档见
  [sdk/python](../sdk/python/) 和 [sdk/nodejs](../sdk/nodejs/)。

- 通过 [GitHub Issues](https://github.com/privonyx/loong-aegisgate/issues) 分享你的节省故事或报告问题

---

> **我们为什么做这个 quickstart：** 任何 AI 网关如果用户花超过 5 分钟还跑不起来，就会在
> 证明价值之前丢用户。AegisGate 的理念是"在第一小时让你看到省钱，而不是第一个月"。
