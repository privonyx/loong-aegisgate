# Outreach Email Template — Seed Users

> **Audience:** Engineering leaders / SRE / Platform engineers building LLM features.
> **Goal:** Open a low-pressure conversation around their LLM cost / safety pain.
> **Length target:** 100–150 words. Read in < 30 seconds.
> **DO NOT** use this for cold mass mailing. This template is for **1-to-1 contact** with people who plausibly need AegisGate.

---

## How to use

1. **Research first.** Before sending, you should know **why** this person needs AegisGate (e.g., they tweeted about LLM costs, opened an issue about prompt injection, or work at a company shipping AI features). Do not send blind.
2. **Personalize the bracketed placeholders.** Never send with `<...>` left in the body.
3. **Run `aegisctl estimate`** with their plausible scenario (model + monthly calls) **before** sending, and quote the headline number in your email — it makes the message concrete.
4. **Keep the door open.** The last paragraph (the "no need to reply" line) is non-negotiable. It is what separates a friendly outreach from spam.

---

## Subject line (pick one)

- `Quick question about <COMPANY>'s LLM stack`
- `<COMPANY> + AegisGate — could save you ~$<MONTHLY_SAVINGS>/mo?`
- `Saw your post on <SPECIFIC_TOPIC> — built a tool that might help`

---

## Body (English / 100–150 words)

```text
Hi <RECIPIENT_NAME>,

I came across <SPECIFIC_THING_THEY_DID> and noticed you're working on <USE_CASE>.

I'm one of the maintainers of AegisGate — an open-source gateway that sits in
front of OpenAI / Anthropic / DeepSeek and handles three things engineering
teams keep re-inventing: semantic cache, model routing, and inbound guardrails
(PII / prompt injection / abuse).

Before bothering you with a demo, I ran our `aegisctl estimate` against a
scenario that looks like yours (<MODEL> @ <MONTHLY_CALLS> calls/mo) — it
projects roughly **$<MONTHLY_SAVINGS> / month** in savings from caching +
routing alone, on top of the safety layer.

If that sounds interesting, the 5-minute quickstart is at:
  https://github.com/privonyx/loong-aegisgate#try-in-5-minutes-docker

If this isn't relevant — no need to reply. I won't follow up.

Thanks,
<YOUR_NAME>
<YOUR_ROLE> · AegisGate maintainer
https://github.com/privonyx/loong-aegisgate
```

---

## Required placeholders (must all be filled)

| Placeholder | Example |
|---|---|
| `<RECIPIENT_NAME>` | `Alice` |
| `<SPECIFIC_THING_THEY_DID>` | `your tweet about gpt-4o costs blowing up` |
| `<USE_CASE>` | `customer support chat at <COMPANY>` |
| `<COMPANY>` | `Acme Inc` |
| `<MODEL>` | `gpt-4o` |
| `<MONTHLY_CALLS>` | `100,000` |
| `<MONTHLY_SAVINGS>` | The number `aegisctl estimate` printed (round to 2 sig figs) |
| `<SPECIFIC_TOPIC>` | A topic referenced in the subject line variant |
| `<YOUR_NAME>` / `<YOUR_ROLE>` | your real name and role |

---

## Anti-patterns (will get you flagged as spam)

- ❌ Don't BCC multiple recipients
- ❌ Don't auto-generate the email from a script — `aegisctl estimate` is a CLI for **you** to read, not a `mailto:?body=` automation
- ❌ Don't follow up more than once if they don't reply within 14 days
- ❌ Don't omit the "no need to reply" line — it's what makes this **outreach** instead of **spam**

---

## Related

- Pre-flight savings: [`aegisctl estimate`](../../estimate.md)
- 5-min quickstart: [Quickstart](../../quickstart.md)
- Full playbook: [Seed User Playbook](../seed-user-playbook.md)
- Repo: https://github.com/privonyx/loong-aegisgate
