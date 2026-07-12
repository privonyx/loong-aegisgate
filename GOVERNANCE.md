# AegisGate Project Governance

This document describes the governance model for the AegisGate project. It defines roles, responsibilities, decision-making processes, and contribution guidelines that ensure the project's long-term health and community trust.

## Table of Contents

- [Project Roles](#project-roles)
- [Decision Making](#decision-making)
- [Release Process](#release-process)
- [Code Review Requirements](#code-review-requirements)
- [Dispute Resolution](#dispute-resolution)
- [Current Maintainers](#current-maintainers)
- [Amendments](#amendments)

---

## Project Roles

### Maintainer

Maintainers have full commit access and are responsible for the project's overall direction, quality, and community health.

**Responsibilities:**
- Set project roadmap and priorities
- Review and merge pull requests
- Cut releases and manage versioning
- Enforce code quality and security standards
- Mentor committers and contributors
- Respond to security vulnerability reports (per [SECURITY.md](SECURITY.md))
- Mediate disputes within the community

**Becoming a Maintainer:**
A Maintainer is nominated by an existing Maintainer and approved by a supermajority (2/3) vote of all current Maintainers. Candidates should demonstrate sustained, high-quality contributions over at least 6 months and a strong understanding of the project's architecture and goals.

**Removal:**
A Maintainer may step down voluntarily or be removed by a supermajority (2/3) vote of the remaining Maintainers for reasons including prolonged inactivity (>6 months without contribution or review), repeated violations of the Code of Conduct, or sustained failure to uphold project quality standards.

### Committer

Committers have write access to the repository and can merge pull requests within their area of expertise, subject to review requirements.

**Responsibilities:**
- Review pull requests in their area of expertise
- Merge approved pull requests that meet review requirements
- Triage issues and label them appropriately
- Contribute code, documentation, and tests regularly

**Becoming a Committer:**
A Committer is nominated by a Maintainer after demonstrating consistent, high-quality contributions (typically 10+ merged PRs over 3+ months). Approval requires consent from at least 2 Maintainers with no objections within 7 days.

**Removal:**
A Committer may step down voluntarily or be removed by any 2 Maintainers for prolonged inactivity (>12 months) or Code of Conduct violations.

### Contributor

Contributors submit pull requests, report issues, participate in discussions, and help improve documentation. Contributors do not have direct write access to the repository.

**Responsibilities:**
- Follow the [Contributing Guide](CONTRIBUTING.md) and [Code of Conduct](CODE_OF_CONDUCT.md)
- Submit well-tested, well-documented pull requests
- Engage constructively in issue discussions and code reviews

**Becoming a Contributor:**
Anyone who submits a pull request, opens a meaningful issue, or contributes to project discussions is considered a Contributor. No formal nomination is required.

### User

Users deploy and use AegisGate in their applications. They are encouraged to participate in the community through discussions, bug reports, and feature requests.

**Engagement channels:**
- [GitHub Discussions](https://github.com/privonyx/loong-aegisgate/discussions) for questions and ideas
- [GitHub Issues](https://github.com/privonyx/loong-aegisgate/issues) for bug reports
- [Discord](https://discord.gg/aegisgate) for real-time community chat

---

## Decision Making

### Lazy Consensus (Minor Changes)

Most decisions are made through **lazy consensus**: a proposal is considered approved if no Maintainer or Committer objects within a reasonable review period.

**Applies to:**
- Bug fixes and minor improvements
- Documentation updates
- Dependency version bumps (patch/minor)
- Test additions and improvements
- Refactoring that does not change public API
- Configuration option additions (backward-compatible)

**Process:**
1. Author opens a pull request with a clear description
2. At least one reviewer approves (see [Code Review Requirements](#code-review-requirements))
3. If no objections are raised within 48 hours of approval, the PR may be merged

### Voting (Major Changes)

Significant changes require an explicit vote from Maintainers.

**Applies to:**
- Public API changes (breaking or additive)
- New pipeline stages or major architectural changes
- New external dependencies added to `vcpkg.json`
- Edition system changes (community vs. enterprise feature boundaries)
- Changes to the governance document
- Major version releases
- New Committer or Maintainer nominations
- Removal of features or deprecation of APIs

**Process:**
1. Author opens a GitHub Discussion or Issue tagged `[RFC]` with a detailed proposal
2. Discussion period of at least 7 days to gather feedback
3. A Maintainer calls for a vote after the discussion period
4. Each Maintainer casts one of: **+1** (approve), **0** (abstain), **-1** (veto with written rationale)
5. The proposal passes with a **simple majority** of voting Maintainers and **no unresolved vetoes**
6. A veto must include a written explanation and a constructive alternative. The proposer may revise and re-submit.

### Emergency Changes

Security fixes and critical bug fixes that affect production deployments may bypass the normal review timeline:

1. The fix is submitted as a PR marked `[EMERGENCY]`
2. A single Maintainer review and approval is sufficient
3. The Maintainer must document the rationale for the expedited merge
4. A follow-up review by a second Maintainer must happen within 72 hours

---

## Release Process

### Who Can Cut Releases

Only **Maintainers** may create releases. The release process:

1. Ensure all CI checks pass on the release branch
2. Update `CHANGELOG.md` with all changes since the last release
3. Update version numbers in `CMakeLists.txt`, `vcpkg.json`, and SDK package files
4. Create a signed Git tag following [Semantic Versioning](https://semver.org/) (see [VERSIONING.md](VERSIONING.md))
5. Push the tag to trigger the release CI workflow
6. Publish release notes on GitHub Releases
7. Notify the community via Discord and GitHub Discussions

### Release Types

| Type | Version Bump | Approval | Frequency |
|------|-------------|----------|-----------|
| Patch (x.y.Z) | Bug fixes, security patches | 1 Maintainer | As needed |
| Minor (x.Y.0) | New features, non-breaking changes | 2 Maintainers | ~Monthly |
| Major (X.0.0) | Breaking API changes | Supermajority (2/3) Maintainer vote | As needed, infrequent |

### Release Candidates

For minor and major releases, a release candidate (e.g., `v1.1.0-rc.1`) should be published at least 7 days before the final release to allow community testing.

---

## Code Review Requirements

All code changes must be reviewed before merging. Review requirements vary by change type:

### Bug Fixes and Documentation

- **1 approval** from a Maintainer or Committer
- Author must not be the sole reviewer
- All CI checks must pass

### New Features and Enhancements

- **2 approvals**, at least one from a Maintainer
- All CI checks must pass, including new tests covering the feature
- Documentation must be updated if the change affects user-facing behavior
- Performance benchmarks should be run if the change affects the request pipeline

### Breaking Changes

- **2 Maintainer approvals** required
- Migration guide must be included
- Deprecation notice in the previous release (per [VERSIONING.md](VERSIONING.md))
- All CI checks must pass

### Security-Sensitive Changes

Changes touching authentication, authorization, cryptography, input validation, or guardrails:

- **2 Maintainer approvals** required
- At least one reviewer must have security expertise
- Security test cases must be included
- Changes must be documented in [SECURITY.md](SECURITY.md) if they affect the security posture

### General Review Guidelines

- Reviews should be completed within **5 business days** of request
- Reviewers should focus on correctness, security, performance, and maintainability
- Stylistic preferences are enforced by `.clang-format` and `.clang-tidy`; do not block PRs on style issues that pass the linter
- Reviewers must test the change locally or verify CI results before approving

---

## Dispute Resolution

When disagreements arise that cannot be resolved through normal discussion:

### Step 1: Direct Discussion

The parties involved should attempt to resolve the disagreement through direct, respectful discussion on the relevant GitHub Issue or Pull Request. Allow at least 48 hours for asynchronous communication.

### Step 2: Mediator

If direct discussion fails, either party may request a Maintainer (not involved in the dispute) to mediate. The mediator will:

- Listen to both sides
- Summarize the positions
- Propose a compromise or make a recommendation

### Step 3: Maintainer Vote

If mediation fails, the issue is escalated to a Maintainer vote:

- Each Maintainer casts a vote with written rationale
- Simple majority decides, with abstentions not counting toward the total
- The decision is final for that particular issue

### Code of Conduct Violations

Disputes involving Code of Conduct violations follow the enforcement process defined in [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). The Maintainer team serves as the enforcement body.

---

## Current Maintainers

<!-- Maintainer entries: add your GitHub handle, name, and area of focus -->

| GitHub Handle | Name | Focus Area | Since |
|---------------|------|------------|-------|
| *TBD* | *TBD* | Project Lead | 2026-01 |
| *TBD* | *TBD* | Core Pipeline & Performance | 2026-01 |
| *TBD* | *TBD* | Security & Guardrails | 2026-01 |

> **Note**: This table will be populated as the project's governance structure is formalized. If you are interested in becoming a Maintainer, please reach out via [GitHub Discussions](https://github.com/privonyx/loong-aegisgate/discussions).

---

## Amendments

This governance document may be amended through the [Voting (Major Changes)](#voting-major-changes) process. Proposed amendments must be submitted as a pull request modifying this file, with a discussion period of at least 14 days before a vote is called.

---

*This document is effective as of v1.0.0 GA (April 2026). It is inspired by governance models from the Apache Software Foundation, the Rust project, and the CNCF.*
