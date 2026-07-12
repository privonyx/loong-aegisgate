# 接触邮件模板 — 种子用户

> **目标对象：** 工程负责人 / SRE / 在做 LLM 功能的平台工程师。
> **目标：** 围绕他们的 LLM 成本 / 安全痛点开启一次低压力对话。
> **长度目标：** 100-150 字 / 30 秒读完。
> **请勿** 把本模板用于冷启动批量发送。本模板用于**一对一**接触你已经判断"很可能需要 AegisGate"的人。

---

## 使用方法

1. **先做调研。** 发送前你应该知道**为什么**这个人需要 AegisGate（例如他在推上抱怨 LLM 成本爆了、提了关于 prompt injection 的 issue、或在做 AI 产品的公司任职）。不要盲发。
2. **填好所有占位符。** 任何带 `<...>` 的位置都必须替换成真实信息再发出。
3. **发送前先跑 `aegisctl estimate`**：用对方可能的场景（模型 + 月调用量）跑一次，把头条数字写进邮件 — 让消息具象。
4. **保留拒绝出口。** 最后一段 "如不感兴趣可不回复" 不可省略。这是友好接触和 spam 的分界线。

---

## 主题行（任选一个）

- `关于 <COMPANY> LLM 技术栈的小问题`
- `<COMPANY> 接 AegisGate 大约能省 ~$<MONTHLY_SAVINGS>/月？`
- `看到你写了 <SPECIFIC_TOPIC> — 我们做了个可能用得上的工具`

---

## 正文（中文 / 100-150 字）

```text
<RECIPIENT_NAME> 你好，

看到你 <SPECIFIC_THING_THEY_DID>，注意到你正在做 <USE_CASE>。

我是 AegisGate 的 maintainer 之一 — 一个开源网关，挡在
OpenAI / Anthropic / DeepSeek 前面，做工程团队反复重写的三件事：
语义缓存、模型路由、入站护栏（PII / prompt injection / 滥用检测）。

在打扰你看 demo 之前，我用 `aegisctl estimate` 跑了一个像你场景的预估
（<MODEL> @ 月 <MONTHLY_CALLS> 次调用）— 仅缓存 + 路由两项，
预计每月可省 **约 $<MONTHLY_SAVINGS>**，安全层是顺手送的。

如果你感兴趣，5 分钟 quickstart：
  https://github.com/privonyx/loong-aegisgate#try-in-5-minutes-docker

如果这跟你不相关 — 不感兴趣可不回复，我也不会再打扰。

谢谢，
<YOUR_NAME>
<YOUR_ROLE> · AegisGate maintainer
https://github.com/privonyx/loong-aegisgate
```

---

## 必填占位符（必须全部替换）

| 占位符 | 示例 |
|---|---|
| `<RECIPIENT_NAME>` | `张三` |
| `<SPECIFIC_THING_THEY_DID>` | `在推上发了 gpt-4o 成本爆炸的吐槽` |
| `<USE_CASE>` | `<COMPANY> 的智能客服` |
| `<COMPANY>` | `某某科技` |
| `<MODEL>` | `gpt-4o` |
| `<MONTHLY_CALLS>` | `100,000` |
| `<MONTHLY_SAVINGS>` | `aegisctl estimate` 算出来的数（保留 2 位有效数字）|
| `<SPECIFIC_TOPIC>` | 主题行中引用的话题 |
| `<YOUR_NAME>` / `<YOUR_ROLE>` | 你的真名 + 角色 |

---

## 反模式（这样做会被标 spam）

- ❌ 不要 BCC 多人群发
- ❌ 不要用脚本自动生成邮件 — `aegisctl estimate` 是给**你**读的 CLI，不是 `mailto:?body=` 自动化
- ❌ 14 天内未回复不要追问超过一次
- ❌ 不要省略 "如不感兴趣可不回复" — 这是把 **outreach** 和 **spam** 区分开的关键

---

## 相关链接

- 上线前预估：[`aegisctl estimate`](../../estimate_zh.md)
- 5 分钟 quickstart：[Quickstart](../../quickstart_zh.md)
- 完整剧本：[种子用户接触剧本](../seed-user-playbook_zh.md)
- 仓库：https://github.com/privonyx/loong-aegisgate
