# Security Policy

## Supported Versions

| Version | Supported          |
|---------|--------------------|
| 1.0.x   | :white_check_mark: |
| 0.9.x   | :white_check_mark: (security fixes only) |
| < 0.9   | :x:                |

We actively support the latest minor release. Security patches are backported to the previous minor release on a best-effort basis.

## Reporting a Vulnerability

**Please do NOT report security vulnerabilities through public GitHub issues, discussions, or pull requests.**

Instead, please report them via one of the following channels:

### Option 1: GitHub Private Vulnerability Reporting

Use [GitHub's private vulnerability reporting](https://github.com/privonyx/loong-aegisgate/security/advisories/new) to submit a report directly. This is the preferred method.

### Option 2: Email

Send an email to **security@aegisgate.dev** with the following information:

- Type of vulnerability (e.g., prompt injection bypass, PII leakage, authentication bypass, buffer overflow)
- Full paths of source file(s) related to the vulnerability
- Step-by-step instructions to reproduce the issue
- Proof-of-concept or exploit code (if possible)
- Impact assessment — what an attacker could achieve

### What to Expect

| Timeline | Action |
|----------|--------|
| **24 hours** | Acknowledgment of your report |
| **72 hours** | Initial assessment and severity classification |
| **7 days** | Detailed response with remediation plan |
| **90 days** | Fix released (may vary based on complexity) |

We follow [coordinated vulnerability disclosure](https://en.wikipedia.org/wiki/Coordinated_vulnerability_disclosure). We ask that you:

- Allow us reasonable time to address the issue before public disclosure
- Make a good faith effort to avoid privacy violations, data destruction, or service disruption
- Do not access or modify data belonging to other users

## Scope

The following are **in scope** for security reports:

- AegisGate core gateway (C++ codebase)
- Security guardrails (injection detection, PII masking, content filtering)
- Authentication and authorization (API key, RBAC, SSO)
- Client SDKs (Python, Node.js, Go)
- Official Docker images and Helm charts
- Configuration parsing and validation

The following are **out of scope**:

- Upstream LLM provider vulnerabilities
- Social engineering attacks
- Denial of service through legitimate API usage within rate limits
- Issues in third-party dependencies (report these upstream, but let us know)

## Security Best Practices

For guidance on securely deploying AegisGate, see [Security Best Practices](docs/guides/security-best-practices.md).

## Recognition

We gratefully acknowledge security researchers who help keep AegisGate and its users safe. With your permission, we will list your name/handle in our security acknowledgments.

## PGP Key

For sensitive communications, you may encrypt your message using our PGP key, available at [https://aegisgate.dev/.well-known/security.txt](https://aegisgate.dev/.well-known/security.txt).
