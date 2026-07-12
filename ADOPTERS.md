# AegisGate Adopters

> A list of organizations and projects using AegisGate in production or evaluation.
>
> Repo: https://github.com/privonyx/loong-aegisgate

This list is **opt-in only**. Each adopter listed here has explicitly approved the entry on a public PR.

---

## Adopters

_Be the first adopter — open a PR to add your name. We're actively looking for early users to learn from._

| Organization | Use case | Approx. scale | Edition | Linked feedback Issue |
|--------------|----------|---------------|---------|----------------------|
| _Your name here?_ | _e.g., customer support chat_ | _e.g., 100k calls/mo_ | _community / enterprise_ | _#xxx_ |

---

## How to add yourself

If you (or your team) is using AegisGate in any capacity — production, staging, or even an evaluation that informed a real decision — we'd love to hear about it. Adding yourself to this list helps the next person evaluating AegisGate trust that real teams use it.

### Step 1 — Tell us about your usage

Open a [`seed_user_feedback` Issue](https://github.com/privonyx/loong-aegisgate/issues/new?template=seed_user_feedback.yml) with the structured fields filled in (scenario, monthly calls, install method, pain points, etc.). This takes ~5 minutes.

If you'd rather skip the structured Issue, you can DM us on [Discord](https://discord.gg/aegisgate) or open a Discussion. The structured Issue is preferred because it helps future evaluators see real reactions.

### Step 2 — Open a PR with your row

Once you're comfortable being listed publicly, open a PR adding a row to the table above. The PR description should include:

```markdown
## Add <YOUR_ORG> to ADOPTERS

- Organization: <YOUR_ORG>
- Use case: <SHORT_DESCRIPTION_1_LINE>
- Approx. scale: <CALLS_PER_MONTH>
- Edition: <community | enterprise>
- Linked feedback Issue: #<ISSUE_NUMBER>
- Approved by: @<YOUR_GITHUB_HANDLE> (this PR is your written approval)
```

A maintainer will review within ~3 days, ask any quick clarifying questions, and merge.

### Step 3 — Pricing context (so you know what you're agreeing to)

AegisGate is **open-core**:

- **Community edition** — Apache-2.0, free, full feature parity for self-hosted use. The `community` row in this table is the default.
- **Enterprise edition** — paid, includes SSO / multi-tenant control plane / SLA / managed cloud. If you're evaluating enterprise features, [contact us](https://github.com/privonyx/loong-aegisgate/discussions).

Adding yourself to `ADOPTERS.md` does **not** imply any commercial relationship — it's just a public statement that you find AegisGate useful.

---

## What we ask in return (nothing required)

If you're listed here, we'd be grateful (but not require) any of the following:

1. **A short case study** (~600 words, you edit, we draft) — even an internal-only retrospective shared with us privately is gold.
2. **A quote** for the README — even one sentence about a specific result.
3. **A talk / blog mention** — if you've already written about your AI infra, a one-line "we used AegisGate" mention helps a lot.

None of the above is a condition for being listed.

---

## How we use this list

- We point new evaluators here when they ask "is anyone actually using this?"
- We use the linked feedback Issues to prioritize the roadmap.
- We may quote (with explicit per-quote consent) numbers from your feedback Issue in MVP-5 case-study blog posts.

We will **never**:

- Add an organization without their explicit, recent, written approval on a PR
- Quote numbers from your feedback Issue without per-quote consent
- Share your contact info with anyone outside the AegisGate maintainer group

---

## Related

- Seed user playbook (for maintainers): [docs/community/seed-user-playbook.md](docs/community/seed-user-playbook.md)
- Feedback Issue template: [`.github/ISSUE_TEMPLATE/seed_user_feedback.yml`](.github/ISSUE_TEMPLATE/seed_user_feedback.yml)
- Pre-flight savings estimator: [`aegisctl estimate`](docs/estimate.md)
- 5-min quickstart: [docs/quickstart.md](docs/quickstart.md)
- README CTA: "Used AegisGate? Tell us" links here.
