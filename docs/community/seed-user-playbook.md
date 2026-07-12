# Seed User Playbook — Recruiting the First 3-5 Adopters

> **Audience:** AegisGate maintainers (you).
> **Goal:** Walk you end-to-end through finding, contacting, onboarding, and converting your first 3-5 seed users into named adopters and case-study material.
> **Time per seed user:** ~115 minutes spread across 2-3 weeks.
> **Mindset:** Reaching out is **human work**. The tools in this repo (templates, `aegisctl estimate`, the feedback Issue) are leverage — they are not automation. Do not delegate the human conversation to scripts.

---

## TL;DR — The 5-step flow

1. **Research** (~30 min/lead) — Identify 1 plausible seed user with concrete LLM pain.
2. **Outreach** (~10 min/lead) — Send 1 personalized email or 1 friendly Issue comment using the templates.
3. **Follow-up** (~5 min/lead) — One follow-up at 7 days; that's it.
4. **Feedback Collection** (~10 min/lead) — Send them the `seed_user_feedback` Issue link after they try the quickstart.
5. **Case Study Decision** (~60 min/lead) — If they consent, invite them to `ADOPTERS.md` and start an MVP-5 case-study draft.

The toolkit you'll be using throughout:

| Step | Tool | Path |
|------|------|------|
| 1 Research | (manual) | — |
| 2 Outreach | Email template | [`outreach/email-template.md`](outreach/email-template.md) |
| 2 Outreach | GitHub Issue comment template | [`outreach/github-issue-comment-template.md`](outreach/github-issue-comment-template.md) |
| 2 Ice-breaker | `aegisctl estimate` | [`docs/estimate.md`](../estimate.md) |
| 4 Feedback | `seed_user_feedback` Issue template | `.github/ISSUE_TEMPLATE/seed_user_feedback.yml` |
| 5 Adopt | `ADOPTERS.md` | [`/ADOPTERS.md`](../../ADOPTERS.md) |

---

## Step 1 — Research (Identify plausible seed users)

**Time budget:** 30 min per candidate. **Don't shortcut this step** — every minute saved here costs 5x in awkward outreach.

### What "plausible seed user" means

Someone who has, in the last 90 days, **publicly expressed** at least one of:

- **LLM cost pain** — tweeted/posted that gpt-4o or claude bills are blowing up
- **Prompt injection / safety pain** — opened an Issue / asked on Discord about jailbreaks
- **Multi-provider routing need** — discussing how to fail over from OpenAI to Anthropic
- **Caching pain** — asking about semantic cache or prompt-caching
- **Audit / compliance pain** — building B2B AI features needing logs / PII redaction

### Where to look

| Source | Signal | Effort |
|--------|--------|--------|
| GitHub `awesome-llm` / `awesome-langchain` ecosystems | Maintainers of LLM tooling repos | 10 min |
| GitHub Issues across major LLM SDKs (`openai-python`, `anthropic-sdk`, `langchain`) | "How do I cache / route / sanitize" questions | 15 min |
| X (Twitter) search for `gpt-4o cost` / `LLM gateway` / `prompt injection` | Live pain | 10 min |
| Hacker News "Show HN" + "Ask HN" (90-day window) | Builders shipping AI features | 10 min |
| Hacker News comments on AI-cost / LLM-safety threads | Engineers expressing real pain | 10 min |
| Reddit `r/LocalLLaMA`, `r/MachineLearning` | Niche audience but high signal | 10 min |
| Discord servers (LangChain / OpenAI / Anthropic community) | Real-time pain | 15 min |

### Research checklist (per candidate)

- [ ] **Name + role + company** (LinkedIn or their bio)
- [ ] **Specific public artifact** that shows the pain (tweet URL / Issue URL / HN comment URL)
- [ ] **Plausible model + monthly call estimate** for `aegisctl estimate`
- [ ] **Why AegisGate fits** their specific pain (1 sentence)
- [ ] **Best contact channel** (email > GitHub Issue comment > X DM > LinkedIn)

If you can't fill in all 5 fields, **drop the candidate**. Move on.

### Anti-patterns

- ❌ Picking from a stars-list without reading their actual recent activity
- ❌ Sending to anyone with "AI" in their bio — too generic
- ❌ Skipping the `aegisctl estimate` pre-run — your number must be specific to their case

---

## Step 2 — Outreach (Send the first message)

**Time budget:** 10 min per candidate (after research is done).

### Decide channel

| Their public artifact | Best channel | Template |
|----------------------|--------------|----------|
| Tweet / blog / public talk | Email (look up via their company website / GitHub commits) | [`outreach/email-template.md`](outreach/email-template.md) |
| GitHub Issue (still open, < 6 months old) | GitHub Issue comment | [`outreach/github-issue-comment-template.md`](outreach/github-issue-comment-template.md) |
| HN comment / Reddit post | Email if they have a public one; otherwise pass | [`outreach/email-template.md`](outreach/email-template.md) |
| Discord message | Discord DM with the same email body, condensed | (manual; adapt email-template.md) |

### Run `aegisctl estimate` first

Before sending **any** outreach, run:

```bash
aegisctl estimate --model <THEIR_MODEL> --monthly-calls <THEIR_ESTIMATE>
```

Quote the headline number in your outreach. Numbers earn replies; vague claims do not.

### Message checklist

- [ ] Subject line / opening references **the specific public artifact** you researched
- [ ] One sentence on what AegisGate does (not three)
- [ ] One concrete number from `aegisctl estimate` ("~$X/mo savings")
- [ ] Link to the [5-min quickstart](../quickstart.md), not the homepage
- [ ] **Mandatory** opt-out line ("If this isn't relevant, no need to reply.")
- [ ] Sign with your real name + role + repo URL `https://github.com/privonyx/loong-aegisgate`

### Send and log

After sending, log the outreach (for your own tracking — do **not** commit a real outreach log to the repo, see SR1 in the design spec):

```
| Date       | Lead         | Channel       | Source artifact          | aegisctl number | Result        |
|------------|--------------|---------------|--------------------------|-----------------|---------------|
| 2026-05-27 | Alice / ACME | email         | her HN comment           | ~$420/mo        | sent          |
```

Keep this **outside the repo** (your own Notion / Logseq / local markdown). The SR1 boundary in the design spec forbids real PII in `docs/community/`.

---

## Step 3 — Follow-up (One nudge, then move on)

**Time budget:** 5 min per candidate.

### The 7-day rule

If they haven't replied within 7 days of your initial outreach, send **one** follow-up of 2-3 sentences:

```text
Hi <NAME>, following up on my note last week about <SPECIFIC_TOPIC>. No pressure — if AegisGate isn't a fit, just say so and I'll close the loop. Otherwise, the 5-min quickstart is at https://github.com/privonyx/loong-aegisgate#try-in-5-minutes-docker.
```

### After the follow-up

- **If they reply with interest** → Step 4.
- **If they reply with "not interested"** → thank them politely, log them as `closed:not_interested`, **do not** contact again.
- **If they don't reply** → log as `closed:no_response` after 14 total days, move on.

### Anti-patterns

- ❌ More than one follow-up — that's spam
- ❌ Adding new pitches in the follow-up — that's escalation
- ❌ Cross-channel pressure (email then DM then comment on their issue) — that's harassment

---

## Step 4 — Feedback Collection (Use the Issue template)

**Time budget:** 10 min per candidate.

### The flow

1. They reply with interest after Step 2 or Step 3.
2. You walk them through the [5-min quickstart](../quickstart.md) (offer to hop on a call if they want; a 15-min call here is gold).
3. After they get something working, send them the structured feedback link:

   ```text
   When you've had a chance to play with it, I'd love your raw reactions:
   https://github.com/privonyx/loong-aegisgate/issues/new?template=seed_user_feedback.yml
   It takes ~5 min and the structured fields really help us prioritize.
   ```

4. They submit a `seed_user_feedback` Issue. The 6 fields are: scenario / monthly calls / actual savings % / install method / pain points / case-study consent.

5. You read the Issue, label it, and respond within 24 hours.

### Why structured feedback (not chat / not email reply)

- The 6 fields map to MVP-5 case-study data: scenario + estimate vs actual = the narrative.
- Public Issues are searchable (future seed users see real reactions, not marketing).
- The `case_study_consent` checkbox makes Step 5 explicit, not awkward.

### Anti-patterns

- ❌ Asking them to fill the Issue **before** they've tried the quickstart — they have nothing to say
- ❌ Skipping the structured Issue and just chatting on Discord — you'll lose the data for MVP-5

---

## Step 5 — Case Study Decision (When to invite them to ADOPTERS.md)

**Time budget:** 60 min per case study (across 2-3 sessions).

### When to invite

After the seed user submits `seed_user_feedback` with `case_study_consent: yes`, **and** they've used AegisGate for ≥ 30 days, **and** they have at least one concrete number to share (e.g., `actual savings: 35% on caching`).

### What to invite them to

1. **`ADOPTERS.md`** — A PR adding their company name (with their explicit approval on the PR description). They review and merge themselves.
2. **A short case-study draft** (~600 words) — You draft, they edit. Use the `seed_user_feedback` Issue as the data source.
3. **MVP-5 case-study blog** (when applicable) — A longer narrative with `aegisctl estimate` projection vs actual numbers. The blog is the strategic deliverable that earns the next 10x users.

### The `ADOPTERS.md` PR template

```markdown
## Add <COMPANY> to ADOPTERS

- Company: <COMPANY>
- Use case: <SHORT_DESCRIPTION>
- Linked feedback Issue: #<ISSUE_NUMBER>
- Approved by (commenter on this PR): @<THEIR_GITHUB_HANDLE>
```

### Anti-patterns

- ❌ Listing a company in `ADOPTERS.md` without their explicit, recent, written consent
- ❌ Quoting numbers in a case study without `case_study_consent: yes` in the feedback Issue
- ❌ Pressuring a "Used AegisGate? Tell us" reply into a case study — let it stay as feedback if they're not ready

---

## Operational checklist (per week)

While running this playbook over 2-3 weeks per cohort:

- [ ] **Monday:** Allocate 30 min to research 1-2 new candidates (Step 1)
- [ ] **Tuesday-Wednesday:** Send outreach to candidates from Monday (Step 2)
- [ ] **Each Thursday:** Send Step-3 follow-ups to last week's non-responders
- [ ] **Throughout:** Respond to any `seed_user_feedback` Issues within 24 hours
- [ ] **End of cohort (week 3):** Tally results — aim for 3-5 successful conversations from ~15-20 outreach messages

A 20% reply rate is good. A 10% reply rate is normal. Below 5%, your research (Step 1) needs sharpening.

---

## When to escalate to MVP-5

You're ready to write the MVP-5 case-study blog when **all** of these are true:

- [ ] At least 1 adopter listed in `ADOPTERS.md` with `case_study_consent: yes`
- [ ] At least 1 adopter has shared concrete numbers (`actual_savings_pct` filled in their feedback Issue)
- [ ] The `aegisctl estimate` projection vs actual gap is < 30% (validates the estimator) **OR** > 50% (interesting story either way)

Then `/van` MVP-5 and use the feedback Issues + ADOPTERS list as your data source.

---

## Related

- Pre-flight savings: [`aegisctl estimate`](../estimate.md)
- 5-min quickstart: [Quickstart](../quickstart.md)
- Email template: [`outreach/email-template.md`](outreach/email-template.md)
- GH Issue comment template: [`outreach/github-issue-comment-template.md`](outreach/github-issue-comment-template.md)
- Adopters list: [`/ADOPTERS.md`](../../ADOPTERS.md)
- Repo: https://github.com/privonyx/loong-aegisgate
- README CTA: "Used AegisGate? Tell us" → `seed_user_feedback` Issue
