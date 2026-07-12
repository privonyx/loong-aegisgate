# Outreach GitHub Issue Comment Template

> **Audience:** Developers who opened an Issue / Discussion / PR on a public repo describing a pain that AegisGate solves (LLM cost spikes, prompt injection, multi-provider routing, semantic cache).
> **Goal:** Friendly mention — never product pitch.
> **Length target:** 3–5 sentences.
> **DO NOT** post this on unrelated issues. The comment must be **directly relevant** to what the OP is discussing.

---

## How to use

1. **Read the entire issue thread first.** If anything in the thread says "we already tried X / not interested in gateways", **do not comment**. Move on.
2. **Match the technical context.** If the OP is talking about caching, lead with caching. If they're talking about prompt injection, lead with the guard layer.
3. **Stay one comment.** Do not reply to your own comment to add more. If they want to learn more they will reply.
4. **Disclose your role.** "I help maintain AegisGate" — never pretend to be a neutral user.
5. **Always offer the door out** — make it explicit that you don't expect a reply.

---

## Template (English / 3–5 sentences)

```markdown
Hi @<OP_GITHUB_HANDLE>, I help maintain [AegisGate](https://github.com/privonyx/loong-aegisgate) — an open-source gateway in front of OpenAI/Anthropic/DeepSeek that handles `<RELEVANT_CAPABILITY>` (the thing you're describing above).

If you want to ballpark whether it would help your numbers, you can run `aegisctl estimate --model <MODEL> --monthly-calls <ESTIMATE>` locally — no install of the gateway required, just a CLI that reads our pricing table.

Happy to answer questions or help debug if you try the [5-min quickstart](https://github.com/privonyx/loong-aegisgate#try-in-5-minutes-docker). Feel free to ignore if not relevant — won't follow up.
```

---

## Required placeholders

| Placeholder | Example |
|---|---|
| `<OP_GITHUB_HANDLE>` | `alice-dev` (the OP's actual GitHub handle) |
| `<RELEVANT_CAPABILITY>` | One of: `semantic cache`, `model routing`, `inbound prompt-injection guardrails`, `cost tracking` |
| `<MODEL>` | The OP's model if they mentioned it; otherwise omit the example |
| `<ESTIMATE>` | A reasonable monthly call count given context (1k / 10k / 100k / 1M) |

---

## Variant: when OP didn't ask for a tool

If the OP is just venting or asking a different question, **do not** drop the link. Instead, answer their actual question first, **then** mention AegisGate only if they explicitly ask "is there a tool for this?". Example:

```markdown
Hi @<OP_GITHUB_HANDLE>, the simplest fix here is `<DIRECT_ANSWER_TO_THEIR_QUESTION>`. (FWIW we wrote up a similar pattern in [AegisGate](https://github.com/privonyx/loong-aegisgate)'s caching layer — happy to share if you want, otherwise no need to reply.)
```

---

## Anti-patterns (will get you reported)

- ❌ Don't post on issues older than 6 months unless the issue is still open with recent activity
- ❌ Don't post on issues already closed (resolved or won't-fix)
- ❌ Don't post `aegisctl estimate` numbers without running it first — fabricated savings claims will damage AegisGate's reputation
- ❌ Don't post the same comment text on >2 issues in 1 week (GitHub will pattern-match it as spam)
- ❌ Don't omit "feel free to ignore if not relevant" — it's the difference between **outreach** and **spam**

---

## Related

- Email outreach: [Email Template](email-template.md)
- Pre-flight savings: [`aegisctl estimate`](../../estimate.md)
- Full playbook: [Seed User Playbook](../seed-user-playbook.md)
- Repo: https://github.com/privonyx/loong-aegisgate
