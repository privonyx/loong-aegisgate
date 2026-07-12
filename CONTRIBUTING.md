# Contributing to AegisGate

[English](CONTRIBUTING.md) | [中文](CONTRIBUTING_zh.md)

Thank you for your interest in contributing to AegisGate! Whether you're fixing a typo, reporting a bug, adding a feature, or improving documentation — every contribution matters.

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Community](#community)
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Build from Source](#build-from-source)
  - [Running Tests](#running-tests)
- [Good First Issues](#good-first-issues)
- [How to Contribute](#how-to-contribute)
  - [Reporting Bugs](#reporting-bugs)
  - [Suggesting Features](#suggesting-features)
  - [Submitting Code](#submitting-code)
- [Development Workflow](#development-workflow)
  - [Branch Naming](#branch-naming)
  - [Commit Message Convention](#commit-message-convention)
  - [Pull Request Workflow](#pull-request-workflow)
- [Code Style](#code-style)
- [Testing Guidelines](#testing-guidelines)
- [Documentation](#documentation)
- [Security](#security)
- [License](#license)

## Code of Conduct

This project follows the [Contributor Covenant Code of Conduct](CODE_OF_CONDUCT.md). By participating, you are expected to uphold this code. Please report unacceptable behavior to **conduct@aegisgate.dev**.

## Community

Join our community to ask questions, share ideas, and connect with other contributors:

| Platform | Purpose | Link |
|----------|---------|------|
| **GitHub Discussions** | Q&A, ideas, show & tell | [Discussions](https://github.com/privonyx/loong-aegisgate/discussions) |
| **Discord** | Real-time chat, help, dev coordination | [Join Discord](https://discord.gg/aegisgate) |
| **GitHub Issues** | Bug reports, feature requests | [Issues](https://github.com/privonyx/loong-aegisgate/issues) |

## Getting Started

### Prerequisites

- C++17 compiler (GCC 11+ or Clang 14+)
- CMake 3.20+
- [vcpkg](https://github.com/microsoft/vcpkg) package manager
- Git

### Build from Source

```bash
git clone https://github.com/privonyx/loong-aegisgate.git
cd aegisgate

cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON

cmake --build build -j$(nproc)
```

### Optional: ONNX Embedder

To enable the neural embedding engine (requires ONNX Runtime):

```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DENABLE_ONNX=ON \
  -DBUILD_TESTS=ON

cmake --build build -j$(nproc)
```

### Running Tests

```bash
cd build && ctest --output-on-failure
```

All tests must pass before submitting a pull request. The project currently has **96 test suites** across 87 executables.

## Good First Issues

New to AegisGate? Start here! We maintain a list of beginner-friendly tasks labeled [`good first issue`](https://github.com/privonyx/loong-aegisgate/labels/good%20first%20issue) on GitHub.

These issues are:
- Well-scoped with clear acceptance criteria
- Require minimal context about the full codebase
- Include implementation hints and pointers to relevant code

See our [Good First Issues Guide](docs/guides/good-first-issues.md) for a curated list of contribution areas organized by skill level.

**Not sure where to start?** Drop a message in [GitHub Discussions](https://github.com/privonyx/loong-aegisgate/discussions/categories/q-a) or our [Discord #contributing channel](https://discord.gg/aegisgate) — we're happy to help you find the right task!

## How to Contribute

### Reporting Bugs

Found a bug? Please [open a bug report](https://github.com/privonyx/loong-aegisgate/issues/new?template=bug_report.yml). Include:

- Steps to reproduce
- Expected vs. actual behavior
- Version, OS, and build configuration
- Relevant logs (set `logging.level: debug` for more detail)

**Important**: Always redact API keys and sensitive information before posting!

### Suggesting Features

Have an idea? Start with a discussion:

1. **Early-stage ideas** → Post in [Discussions > Ideas](https://github.com/privonyx/loong-aegisgate/discussions/categories/ideas)
2. **Well-defined features** → Open a [Feature Request](https://github.com/privonyx/loong-aegisgate/issues/new?template=feature_request.yml) issue

### Submitting Code

1. Check existing issues or create one describing what you plan to do
2. Comment on the issue to let others know you're working on it
3. Fork the repository and create your branch
4. Implement your changes following the guidelines below
5. Submit a pull request

## Development Workflow

### Branch Naming

```
feature/short-description     # New features
fix/short-description          # Bug fixes
docs/short-description         # Documentation
refactor/short-description     # Code restructuring
test/short-description         # Test improvements
```

### Commit Message Convention

Use the following format:

```
type(scope): short description

Optional longer description.
```

**Types:**

| Type | Usage |
|------|-------|
| `feat` | New feature |
| `fix` | Bug fix |
| `refactor` | Code restructuring (no behavior change) |
| `test` | Adding or updating tests |
| `docs` | Documentation changes |
| `chore` | Build, CI, or tooling changes |
| `perf` | Performance improvement |

**Scope** (optional): the affected module — `gateway`, `guardrail`, `cache`, `observe`, `auth`, `storage`, `server`, `cli`, `sdk`, `config`

**Examples:**

```
feat(guardrail): add PII masking for bank card numbers
fix(gateway): correct token bucket refill timing under high concurrency
docs(sdk): update Python SDK installation instructions
test(cache): add concurrent insert/delete test for vector index
```

### Pull Request Workflow

1. **Fork** the repository and clone your fork
2. **Create a branch** from `main`:
   ```bash
   git checkout -b feature/your-feature-name
   ```
3. **Make your changes** — follow the code style below
4. **Add tests** — every feature or fix should have corresponding tests
5. **Run tests** — ensure all 96 test suites pass
6. **Commit** with a descriptive message following the convention above
7. **Push** to your fork and open a Pull Request against `main`
8. **Describe your changes** in the PR body using the [PR template](.github/PULL_REQUEST_TEMPLATE.md)
9. **Respond to reviews** — address feedback promptly

**PR tips:**
- Keep PRs focused — one logical change per PR
- Include "Fixes #123" in the PR description to auto-link issues
- Add screenshots or log output for visual/behavioral changes
- Mark as "Draft" if the PR is work-in-progress

## Code Style

- **Standard:** C++17 baseline, C++20 features guarded with `#if __cplusplus >= 202002L`
- **Namespace:** All code under `aegisgate`
- **Header guards:** Use `#pragma once`
- **Pointers:** Prefer smart pointers (`std::unique_ptr`, `std::shared_ptr`)
- **Error handling:** Exceptions for initialization/config errors; return values/enums for runtime
- **Logging:** Use spdlog with fmt formatting
- **Testing:** Each `.cpp` should have a corresponding `test_*.cpp`
- **Formatting:** Follow the existing code style in the module you're modifying
- **Comments:** Explain *why*, not *what* — code should be self-documenting for the "what"

## Testing Guidelines

- Write tests for every new feature and bug fix
- Follow the naming pattern: `tests/test_<module_name>.cpp`
- Use Google Test framework (`TEST`, `TEST_F`, `EXPECT_*`, `ASSERT_*`)
- Test both happy paths and error cases
- For async/concurrent code, test thread safety explicitly
- Integration tests go in `tests/integration/`
- Run the full suite before submitting: `cd build && ctest --output-on-failure`

## Documentation

- Update relevant docs when your code changes user-facing behavior
- Documentation lives in `docs/guides/` for user guides
- API changes should be reflected in `docs/openapi.yaml`
- SDK changes should update the respective `sdk/*/README.md`

## Security

If you discover a security vulnerability, **do NOT create a public issue**. Please report it privately via [GitHub Security Advisories](https://github.com/privonyx/loong-aegisgate/security/advisories/new) or email **security@aegisgate.dev**. See [SECURITY.md](SECURITY.md) for details.

When writing code, follow these security practices:
- Never hardcode API keys, tokens, or passwords
- Use parameterized queries (no SQL string concatenation)
- Validate and sanitize all external input
- Prefer RE2 over std::regex (linear time, ReDoS-proof)
- See [Security Best Practices](docs/guides/security-best-practices.md) for the full checklist

## Versioning & CHANGELOG

AegisGate follows [Semantic Versioning](https://semver.org/). See [VERSIONING.md](VERSIONING.md) for the full policy.

**Every PR that adds features, fixes bugs, or introduces breaking changes must include a CHANGELOG entry.**

### Updating the CHANGELOG

1. Add your entry under the `## [Unreleased]` section at the top of `CHANGELOG.md`
2. Use the appropriate subsection: `### Added`, `### Changed`, `### Fixed`, `### Deprecated`, or `### Removed`
3. Write a concise, user-facing description (not implementation details)

Example:

```markdown
## [Unreleased]

### Added
- **Streaming timeout** — configurable per-model streaming timeout with automatic connection cleanup
```

### Version Number Sync

If a release is being prepared, ensure version numbers are consistent across all files listed in [VERSIONING.md](VERSIONING.md#版本号同步).

## License

By contributing, you agree that your contributions will be licensed under the [Apache License 2.0](LICENSE).
