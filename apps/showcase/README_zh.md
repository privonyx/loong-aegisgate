# AegisGate Showcase App（落地参考 Demo）

> 大模型 → **AegisGate** → 应用：用一个可运行的参考 Demo，把 AI 网关的**省钱**与**合规**价值做到「肉眼可见、当场震撼」。

[English](./README.md) | 简体中文

## 这是什么

一个**通用骨架 + 可插拔场景插件**的演示应用，所有大模型调用统一经 AegisGate：

- **旗舰场景 · AI 漫剧创作台**：杀手锏双引擎——**创作引擎**（分集大纲/剧本/分镜批量生成，主打缓存与 Token 省钱）+ **合规引擎**（发布前一键合规预审，主打不下架）。
- **验证场景 · 电商导购助手**：同一套插件接口换业务，证明骨架通用（运行时与路由零改动）。

四大网关价值在 UI 中可视：🛡️ 护栏拦截、💰 语义缓存省量、🔀 智能路由（应答模型可见）、📊 观测聚合。

## 架构

```
React 前端（引导式创作台 + 双核心价值面板）
        │  /api（仅经 BFF，前端从不持有网关 Key）
        ▼
Express 后端（BFF）
  ├─ 场景注册表 + Function-Calling 运行时（每轮经网关，抽取价值事件）
  ├─ AegisGate 客户端（fetch 直连，捕获响应头：tokens-saved / 缓存 / 应答模型）
  └─ 观测聚合（省量 / 缓存命中率 / 护栏拦截 / 按引擎拆分）
        │  OpenAI 兼容 /v1/chat/completions（Bearer AegisGate Key）
        ▼
AegisGate 网关（护栏 / 缓存 / 路由 / 观测；Provider Key 配在此侧）
        ▼
真实大模型（OpenAI / Claude / Qwen ……）
```

## 快速开始

### 前置
- 已运行的 AegisGate 网关（仓库根 `docker compose up -d`，并在网关侧配置好 Provider Key）。
- Node.js ≥ 22（手动开发模式需要）。

### 方式 A：Docker 一键起
```bash
# 1. 启动网关（仓库根目录）
docker compose up -d

# 2. 配置 Demo 环境变量
cp apps/showcase/.env.example apps/showcase/.env
# 编辑 .env，填入 AEGISGATE_API_KEY（由网关签发）

# 3. 启动 Demo
cd apps/showcase && docker compose up --build
# 访问 http://localhost:5173
```

### 方式 B：手动开发
```bash
cp apps/showcase/.env.example apps/showcase/.env   # 填入 AEGISGATE_API_KEY

cd apps/showcase/backend && npm install && npm run dev   # :8090
cd apps/showcase/frontend && npm install && npm run dev   # :5173（已代理 /api → :8090）
```

## 价值演示脚本（4 步当场震撼）
1. 选「AI 漫剧创作台」→ **生成分集大纲**（首次，无缓存）。
2. 再次以相似主题生成 / 复用角色设定 → **缓存命中，省钱计数器跳动**。
3. 切到 **一键合规预审**，粘贴越界文本（如露骨暴力）→ **🛡️ 拦截 + 审计条目 +1**。
4. 切换主/备模型（`.env` 的 `SHOWCASE_MODEL` / `SHOWCASE_FALLBACK_MODEL`）→ 价值面板「应答模型分布」可见路由变化。

## 新增一个场景插件

在 `backend/src/scenarios/<your>/` 下声明同一套契约即可，运行时/路由/前端零改动：

```ts
export function yourPlugin(model): ScenarioPlugin {
  return {
    id: 'your',
    meta: { name, description, icon, accentColor },
    systemPrompt: '……',
    tools: [/* ToolDefinition：spec + 纯函数 handler（SR-3 无副作用） */],
    uiSteps: [/* generate（模型生成）/ tool（直调）/ guard（合规预审） */],
    dataset: yourDataset,
    guardrail: { ruleFile: 'your.yaml' },
    model,
  };
}
```
然后在 `backend/src/scenarios/index.ts` 注册；护栏规则放 `apps/showcase/config/rules/your.yaml`（复用网关 `regex_match` / `keyword_contains` / `length_check` + `block` / `warn` / `modify` 格式）并挂载到网关。

## 环境变量
| 变量 | 说明 |
|------|------|
| `AEGISGATE_BASE_URL` | 网关地址（默认 `http://localhost:8080`） |
| `AEGISGATE_API_KEY` | 网关签发的 Demo Key（**仅后端持有**） |
| `SHOWCASE_MODEL` | 主模型（需支持 Function-Calling） |
| `SHOWCASE_FALLBACK_MODEL` | 备用模型（演示路由 / 降级，可选） |
| `PORT` | 后端端口（默认 8090） |
| `SHOWCASE_CORS_ORIGIN` | 允许的前端来源 |

## 安全约定
- **SR-1**：Provider Key 配在 AegisGate 侧，Demo 仅持 AegisGate Key 且**只在后端**；前端从不接触任何 Key。
- **SR-2**：后端对所有请求做入参校验（zod），非法/越权返回 4xx。
- **SR-3**：工具 handler 为纯函数，无文件写 / 命令执行 / 任意网络副作用。
- **SR-4**：护栏拦截只回传原因/编码，不回显被拦截原文。

## 测试
```bash
cd apps/showcase/backend && npm test    # 运行时 / 客户端 / 观测 / 场景 / 安全 SR
cd apps/showcase/frontend && npm test   # 价值面板 / App 交互
```

> 说明：因 `real_only`（仅连真实模型），端到端联调为**手动验收**（需真实网关与 Provider Key），不进 CI；单测通过可注入的 AegisGate 客户端 mock 接缝覆盖逻辑。
