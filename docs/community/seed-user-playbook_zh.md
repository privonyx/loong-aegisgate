# 种子用户接触剧本 — 招募前 3-5 个早期采用者

> **目标读者：** AegisGate 的 maintainer（你本人）。
> **目标：** 端到端走完"找人 → 接触 → 上手 → 留下反馈 → 转化为 named adopter / case study"的全流程。
> **每个种子用户预算：** ~115 分钟，分散在 2-3 周内。
> **心态：** 接触是**人的工作**。本仓库的工具（模板 / `aegisctl estimate` / feedback Issue）是杠杆 — 不是自动化。请勿把人对人的对话委托给脚本。

---

## TL;DR — 5 步流程

1. **Research（调研）**（每个候选人 ~30 min）— 找出 1 个有具体 LLM 痛点的潜在种子用户。
2. **Outreach（接触）**（每个候选人 ~10 min）— 用模板发 1 封个性化邮件 / 1 条友好 Issue 评论。
3. **Follow-up（跟进）**（每个候选人 ~5 min）— 7 天后跟进 1 次，仅 1 次。
4. **Feedback Collection（反馈收集）**（每个候选人 ~10 min）— 等他们试过 quickstart 后给 `seed_user_feedback` Issue 链接。
5. **Case Study Decision（案例研究决策）**（每个 case study ~60 min）— 如果同意，邀请他们入 `ADOPTERS.md` + 起草 MVP-5 case study。

全流程用到的工具：

| 步骤 | 工具 | 路径 |
|------|------|------|
| 1 调研 | （手工）| — |
| 2 接触 | 邮件模板 | [`outreach/email-template_zh.md`](outreach/email-template_zh.md) |
| 2 接触 | GitHub Issue 评论模板 | [`outreach/github-issue-comment-template_zh.md`](outreach/github-issue-comment-template_zh.md) |
| 2 破冰素材 | `aegisctl estimate` | [`docs/estimate_zh.md`](../estimate_zh.md) |
| 4 反馈 | `seed_user_feedback` Issue 模板 | `.github/ISSUE_TEMPLATE/seed_user_feedback.yml` |
| 5 加入 | `ADOPTERS.md` | [`/ADOPTERS.md`](../../ADOPTERS.md) |

---

## Step 1 — Research（识别潜在种子用户）

**时间预算：** 每个候选人 30 min。**不要省略此步** — 这里省的每分钟会在尴尬接触时还回来 5 倍。

### "潜在种子用户"的定义

最近 90 天内**公开**表达过以下任一痛点的人：

- **LLM 成本痛** — 在推/博客上吐槽 gpt-4o 或 claude 账单爆炸
- **Prompt injection / 安全痛** — 在 Issue / Discord 上问 jailbreak 怎么防
- **多 provider 路由需求** — 在讨论 OpenAI 失败如何 fall over 到 Anthropic
- **缓存痛** — 在问语义缓存或 prompt caching 怎么做
- **审计 / 合规痛** — 在做 B2B AI 功能需要日志 / PII 脱敏

### 去哪找

| 来源 | 信号 | 投入 |
|--------|--------|--------|
| GitHub `awesome-llm` / `awesome-langchain` 生态 | LLM 工具仓库的 maintainer | 10 min |
| 主流 LLM SDK 的 Issues（`openai-python` / `anthropic-sdk` / `langchain`）| "怎么 cache / route / sanitize" 类问题 | 15 min |
| X（推）搜索 `gpt-4o cost` / `LLM gateway` / `prompt injection` | 实时痛点 | 10 min |
| Hacker News "Show HN" + "Ask HN"（90 天窗口）| 在做 AI 功能的 builder | 10 min |
| HN AI 成本 / LLM 安全 帖子的评论区 | 表达真实痛的工程师 | 10 min |
| Reddit `r/LocalLLaMA` / `r/MachineLearning` | 小众但信号高 | 10 min |
| Discord 服务器（LangChain / OpenAI / Anthropic 社区）| 实时痛点 | 15 min |

### 调研 checklist（每个候选人）

- [ ] **姓名 + 角色 + 公司**（LinkedIn 或他们的 bio）
- [ ] **能说明痛点的具体公开材料**（推链接 / Issue 链接 / HN 评论链接）
- [ ] **用于 `aegisctl estimate` 的合理模型 + 月调用量**
- [ ] **AegisGate 为什么适合**他们的具体痛点（1 句话）
- [ ] **最佳接触渠道**（邮件 > GitHub Issue 评论 > X DM > LinkedIn）

如果上面 5 项有填不出的，**放弃这个候选人**。

### 反模式

- ❌ 从 stars 列表里挑人却没读他最近的实际活动
- ❌ 给所有 bio 里有 "AI" 的人发 — 太泛
- ❌ 跳过 `aegisctl estimate` 预跑 — 你给的数字必须针对他的具体场景

---

## Step 2 — Outreach（发出第一条消息）

**时间预算：** 每个候选人 10 min（调研做完之后）。

### 选渠道

| 他们的公开材料 | 最佳渠道 | 模板 |
|--------------|--------|------|
| 推文 / 博客 / 公开演讲 | 邮件（从公司网站 / GitHub commits 找）| [`outreach/email-template_zh.md`](outreach/email-template_zh.md) |
| GitHub Issue（仍开着，<6 个月）| GitHub Issue 评论 | [`outreach/github-issue-comment-template_zh.md`](outreach/github-issue-comment-template_zh.md) |
| HN 评论 / Reddit 帖子 | 邮件（如有公开）；否则放弃 | [`outreach/email-template_zh.md`](outreach/email-template_zh.md) |
| Discord 消息 | Discord DM 用同样邮件正文，压缩版 | （手工；改自 email-template_zh.md）|

### 发送前先跑 `aegisctl estimate`

发**任何** outreach 前，先跑：

```bash
aegisctl estimate --model <THEIR_MODEL> --monthly-calls <THEIR_ESTIMATE>
```

把头条数字写进 outreach。数字带来回复；模糊声明不会。

### 消息 checklist

- [ ] 主题行 / 开头引用了你**调研到的具体公开材料**
- [ ] 一句话说明 AegisGate 是什么（不要三句）
- [ ] 一个来自 `aegisctl estimate` 的具体数字（"约 $X / 月节省"）
- [ ] 链接到 [5 分钟 quickstart](../quickstart_zh.md)，不要链接到主页
- [ ] **必须**包含拒绝出口（"如不感兴趣可不回复"）
- [ ] 用真名 + 角色 + 仓库 URL `https://github.com/privonyx/loong-aegisgate` 签名

### 发送后记录

发送后记录这次 outreach（仅供你自己跟踪 — 实际接触日志**不要 commit 进仓库**，详见 design spec SR1）：

```
| 日期       | 候选人        | 渠道          | 来源材料                | aegisctl 数字 | 结果 |
|------------|---------------|---------------|------------------------|---------------|------|
| 2026-05-27 | Alice / ACME  | 邮件          | 她的 HN 评论            | ~$420/mo      | 已发 |
```

把这份日志放在**仓库外**（你的 Notion / Logseq / 本地 markdown）。design spec 的 SR1 边界禁止真实 PII 进 `docs/community/`。

---

## Step 3 — Follow-up（跟进 1 次，然后翻篇）

**时间预算：** 每个候选人 5 min。

### 7 天规则

如果初次 outreach 后 7 天内没回复，发**1 次** 2-3 句的跟进：

```text
<NAME> 你好，跟进一下我上周关于 <SPECIFIC_TOPIC> 的邮件。无压力 — 如果 AegisGate 不合适，告诉我一声我就 close 这个线索。否则 5 分钟 quickstart 在 https://github.com/privonyx/loong-aegisgate#try-in-5-minutes-docker
```

### 跟进后

- **回复有兴趣** → Step 4。
- **回复"不感兴趣"** → 礼貌道谢，标记为 `closed:not_interested`，**不要**再联系。
- **完全不回** → 14 天后标记为 `closed:no_response`，翻篇。

### 反模式

- ❌ 跟进超过 1 次 — 那是 spam
- ❌ 跟进时加新的推销点 — 那是升级
- ❌ 跨渠道施压（邮件 → DM → 评论他的 issue）— 那是骚扰

---

## Step 4 — Feedback Collection（用 Issue 模板）

**时间预算：** 每个候选人 10 min。

### 流程

1. Step 2 或 Step 3 后他们回复有兴趣。
2. 你带他走一遍 [5 分钟 quickstart](../quickstart_zh.md)（如果他愿意，约 15 min 通话非常值）。
3. 他们跑通后，发结构化 feedback 链接：

   ```text
   等你玩过有感觉了，我很想听听你的真实反馈：
   https://github.com/privonyx/loong-aegisgate/issues/new?template=seed_user_feedback.yml
   大概 5 分钟，结构化字段对我们排优先级很有帮助。
   ```

4. 他们提交 `seed_user_feedback` Issue。6 字段：场景 / 月调用量 / 实际节省 % / 安装方式 / 痛点 / 是否同意 case study。

5. 你 24 小时内读 Issue / 加 label / 回复。

### 为什么用结构化反馈（不用聊天 / 不用邮件回信）

- 6 个字段对应 MVP-5 case study 数据：场景 + 预估 vs 实际 = narrative。
- 公开 Issue 可搜索（未来的种子用户看到的是真实反馈，不是营销）。
- `case_study_consent` checkbox 让 Step 5 显式，不尴尬。

### 反模式

- ❌ 在他们试 quickstart **之前**就让他们填 Issue — 他们没东西可写
- ❌ 跳过结构化 Issue 只在 Discord 上聊 — 你会丢失 MVP-5 的数据

---

## Step 5 — Case Study Decision（什么时候邀请进 ADOPTERS.md）

**时间预算：** 每个 case study 60 min（分 2-3 次 session 完成）。

### 何时邀请

种子用户提交了 `seed_user_feedback`，**并且** `case_study_consent: yes`，**并且** 用 AegisGate ≥ 30 天，**并且** 至少有 1 个具体数字（如 `actual savings: 35% on caching`）。

### 邀请什么

1. **`ADOPTERS.md`** — 一个 PR 加上他们公司名字（PR 描述里要有他们的明确批准）。让他们自己 review 自己 merge。
2. **一篇短 case study 草稿**（~600 字）— 你起草，他们改。数据来源是 `seed_user_feedback` Issue。
3. **MVP-5 case study 博客**（如适用）— 更长的 narrative，含 `aegisctl estimate` 预估 vs 实际数字。博客是赢得下一个 10x 用户的战略交付物。

### `ADOPTERS.md` PR 模板

```markdown
## Add <COMPANY> to ADOPTERS

- Company: <COMPANY>
- Use case: <SHORT_DESCRIPTION>
- Linked feedback Issue: #<ISSUE_NUMBER>
- Approved by (commenter on this PR): @<THEIR_GITHUB_HANDLE>
```

### 反模式

- ❌ 在 `ADOPTERS.md` 列公司名字而没有他们明确、最近、书面的同意
- ❌ feedback Issue 没有 `case_study_consent: yes` 就在 case study 里引用数字
- ❌ 把 "Used AegisGate? Tell us" 的回复硬挤成 case study — 如果他们还没准备好，让 feedback 留作 feedback

---

## 操作 checklist（每周）

每个 cohort 跑 2-3 周时：

- [ ] **周一：** 30 min 调研 1-2 个新候选人（Step 1）
- [ ] **周二-周三：** 给周一的候选人发 outreach（Step 2）
- [ ] **每周四：** 给上周没回复的人发 Step 3 跟进
- [ ] **持续：** 24 小时内回复任何 `seed_user_feedback` Issue
- [ ] **cohort 末（第 3 周）：** 统计结果 — 目标：~15-20 条 outreach 中有 3-5 个成功对话

20% 回复率算好。10% 算正常。低于 5% 说明你的调研（Step 1）需要打磨。

---

## 何时升级到 MVP-5

以下条件**全部**满足时可以开始写 MVP-5 case study 博客：

- [ ] `ADOPTERS.md` 中至少有 1 个 adopter，且 `case_study_consent: yes`
- [ ] 至少 1 个 adopter 在 feedback Issue 中填了具体数字（`actual_savings_pct`）
- [ ] `aegisctl estimate` 预估 vs 实际差距 < 30%（验证估算器）**或** > 50%（无论哪种都是好故事）

然后 `/van` MVP-5，把 feedback Issues + ADOPTERS 列表作为数据源。

---

## 相关链接

- 上线前预估：[`aegisctl estimate`](../estimate_zh.md)
- 5 分钟 quickstart：[Quickstart](../quickstart_zh.md)
- 邮件模板：[`outreach/email-template_zh.md`](outreach/email-template_zh.md)
- GitHub Issue 评论模板：[`outreach/github-issue-comment-template_zh.md`](outreach/github-issue-comment-template_zh.md)
- Adopters 列表：[`/ADOPTERS.md`](../../ADOPTERS.md)
- 仓库：https://github.com/privonyx/loong-aegisgate
- README CTA："Used AegisGate? Tell us" → `seed_user_feedback` Issue
