# GitHub Issue 评论模板 — 接触种子用户

> **目标对象：** 在公开 repo 上开了 Issue / Discussion / PR 描述了 AegisGate 能解决的痛点（LLM 成本爆炸 / prompt injection / 多 provider 路由 / 语义缓存）的开发者。
> **目标：** 友好提及 — 绝非产品推销。
> **长度目标：** 3-5 句。
> **请勿** 在不相关的 issue 上发。评论必须与 OP 在讨论的内容**直接相关**。

---

## 使用方法

1. **先把整个 issue 读完。** 如果线程里有人已经说过 "我们试过 X / 不考虑用 gateway"，**不要评论**，跳过这条。
2. **匹配技术上下文。** OP 在讲缓存就先讲缓存。OP 在讲 prompt injection 就先讲护栏。
3. **保持单条评论。** 不要在自己的评论下追加。如果对方想了解更多，他会回。
4. **披露你的身份。** "我是 AegisGate 的 maintainer" — 绝不要冒充中立用户。
5. **永远留拒绝出口** — 显式说"不感兴趣可不回复"。

---

## 模板（中文 / 3-5 句）

```markdown
@<OP_GITHUB_HANDLE> 你好，我是 [AegisGate](https://github.com/privonyx/loong-aegisgate) 的 maintainer 之一 — 一个挡在 OpenAI/Anthropic/DeepSeek 前面的开源网关，正好做 `<RELEVANT_CAPABILITY>`（也就是你上面在讲的事）。

如果想估一下对你的场景有没有帮助，可以本地跑一下 `aegisctl estimate --model <MODEL> --monthly-calls <ESTIMATE>` — 不需要装网关，只是一个读价格表的 CLI。

如果你试一下 [5 分钟 quickstart](https://github.com/privonyx/loong-aegisgate#try-in-5-minutes-docker) 我也乐意答疑或一起 debug。如果跟你不相关，不感兴趣可不回复，我也不会继续追。
```

---

## 必填占位符

| 占位符 | 示例 |
|---|---|
| `<OP_GITHUB_HANDLE>` | `alice-dev`（OP 的真实 GitHub handle）|
| `<RELEVANT_CAPABILITY>` | 任选：`语义缓存`、`模型路由`、`入站 prompt-injection 护栏`、`成本跟踪` |
| `<MODEL>` | OP 提到的模型；没提就省略示例 |
| `<ESTIMATE>` | 根据语境合理的月调用量（1k / 10k / 100k / 1M）|

---

## 变体：当 OP 没问"工具"时

如果 OP 只是在吐槽或问别的问题，**不要**直接丢链接。先回答他真正的问题，**仅当**他显式问"有没有这种工具"时再提 AegisGate。示例：

```markdown
@<OP_GITHUB_HANDLE> 你好，最简单的修法是 `<DIRECT_ANSWER_TO_THEIR_QUESTION>`。（顺便提一下我们在 [AegisGate](https://github.com/privonyx/loong-aegisgate) 缓存层写过类似的模式 — 如果你感兴趣可以聊，不感兴趣可不回复。）
```

---

## 反模式（这样做会被举报）

- ❌ 不要在 6 个月以上的旧 issue 上发，除非该 issue 仍开着且最近有活动
- ❌ 不要在已 closed（resolved / won't-fix）的 issue 上发
- ❌ 不要在没跑 `aegisctl estimate` 的情况下编造节省数字 — 杜撰会损害 AegisGate 信誉
- ❌ 不要 1 周内在 >2 个 issue 上发同样文本（GitHub 会模式识别为 spam）
- ❌ 不要省略 "不感兴趣可不回复" — 这是把 **outreach** 和 **spam** 区分开的关键

---

## 相关链接

- 邮件接触：[邮件模板](email-template_zh.md)
- 上线前预估：[`aegisctl estimate`](../../estimate_zh.md)
- 完整剧本：[种子用户接触剧本](../seed-user-playbook_zh.md)
- 仓库：https://github.com/privonyx/loong-aegisgate
