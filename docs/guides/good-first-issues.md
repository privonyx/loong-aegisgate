# Good First Issues Guide

[English](good-first-issues.md) | [中文](#中文版)

Welcome to AegisGate! This guide helps new contributors find their first task. Issues are organized by skill level and component — pick one that matches your interests and experience.

## How to Get Started

1. **Browse** the list below and find a task that interests you
2. **Check** [GitHub Issues](https://github.com/privonyx/loong-aegisgate/labels/good%20first%20issue) for open `good first issue` tasks
3. **Comment** on the issue to claim it (or open a new one using the [Good First Issue template](https://github.com/privonyx/loong-aegisgate/issues/new?template=good_first_issue.yml))
4. **Read** [CONTRIBUTING.md](../../CONTRIBUTING.md) for development setup and workflow
5. **Ask for help** in [Discord #contributing](https://discord.gg/aegisgate) or [GitHub Discussions](https://github.com/privonyx/loong-aegisgate/discussions) if you get stuck

## Beginner: No C++ Experience Needed

These tasks involve documentation, configuration, scripts, or tooling — no C++ coding required.

### Documentation

| Task | Description | Files |
|------|-------------|-------|
| Translate docs to Chinese | Add Chinese translations for English-only guides | `docs/guides/*.md` |
| Improve error code docs | Add examples and troubleshooting tips for each error code | `docs/guides/error-codes.md` |
| Add deployment examples | Document common deployment patterns (AWS, GCP, Azure) | `docs/guides/` |
| SDK usage examples | Add real-world usage examples to SDK READMEs | `sdk/*/README.md` |
| FAQ page | Compile frequently asked questions from Discussions into a FAQ | `docs/guides/faq.md` |
| Configuration reference | Create comprehensive config file reference with all options | `docs/guides/` |

### Configuration & Scripts

| Task | Description | Files |
|------|-------------|-------|
| Add YAML schema | Create JSON Schema for aegisgate.yaml validation | `config/` |
| Security rule templates | Create industry-specific rule templates (healthcare, finance) | `config/rules/templates/` |
| Docker healthcheck | Improve Docker healthcheck to verify readiness, not just liveness | `Dockerfile`, `docker-compose.yaml` |

## Easy: Basic C++ Knowledge

These tasks involve straightforward C++ changes — typically adding a simple function, test, or configuration option.

### Tests

| Task | Description | Hint |
|------|-------------|------|
| Add edge case tests | Test empty inputs, max-length strings, unicode edge cases in existing modules | Follow patterns in `tests/test_*.cpp` |
| Improve test coverage | Add tests for uncovered branches (check coverage report) | `cd build && ctest --output-on-failure` |
| Add benchmark tests | Create benchmarks for hot paths (token estimation, PII detection) | See `benchmarks/` for examples |

### Guardrails

| Task | Description | Files |
|------|-------------|-------|
| New PII pattern | Add detection for a new PII type (passport numbers, drivers license) | `src/guardrail/inbound/pii_filter.h`, `config/rules/pii_patterns.yaml` |
| New injection pattern | Add detection for a new prompt injection technique | `config/rules/injection_patterns.yaml`, `tests/test_injection_detector.cpp` |
| Multilingual topic keywords | Add non-English keywords to topic boundary detection | `config/rules/topic_whitelist.yaml` |

### CLI

| Task | Description | Files |
|------|-------------|-------|
| Add `aegisctl version` | Implement version command showing build info | `src/cli/` |
| Colorized output | Add color support to CLI output (errors in red, success in green) | `src/cli/` |
| Shell completion | Generate bash/zsh/fish completion scripts | `scripts/` |

## Medium: Intermediate C++ Knowledge

These tasks involve creating a new feature within an existing module or making cross-cutting changes.

### Gateway

| Task | Description | Files |
|------|-------------|-------|
| New model connector | Add support for a new AI provider (e.g., Cohere, AI21) | `src/gateway/connector/`, follow existing patterns |
| Request timeout config | Make per-model request timeouts configurable | `src/gateway/connector/`, `config/models.yaml` |
| Retry with backoff | Add configurable retry with exponential backoff for upstream failures | `src/gateway/fallback.h` |

### Observability

| Task | Description | Files |
|------|-------------|-------|
| New Prometheus metric | Add a useful metric (e.g., cache entry count, active connections) | `src/observe/metrics.h` |
| Log rotation | Add file-based log rotation support | `src/observe/request_logger.h` |
| Request duration by stage | Add per-pipeline-stage duration metrics | `src/core/pipeline.h`, `src/observe/metrics.h` |

### Cache

| Task | Description | Files |
|------|-------------|-------|
| Cache statistics endpoint | Expose cache stats (hit rate, entry count, avg similarity) via API | `src/cache/semantic_cache.h`, `src/server/api_controller.h` |
| Cache entry inspection | Add aegisctl command to inspect cached entries | `src/cli/`, `src/cache/` |

## Contribution Areas by Component

| Component | Difficulty Range | Good For |
|-----------|-----------------|----------|
| Documentation | Beginner | Writers, non-C++ developers |
| Configuration | Beginner-Easy | DevOps engineers, YAML/scripting |
| Tests | Easy | C++ beginners learning the codebase |
| Guardrails | Easy-Medium | Security-minded developers |
| CLI | Easy-Medium | Developers who like CLI tools |
| Gateway | Medium | C++ developers, networking |
| Observability | Medium | SRE/monitoring background |
| SDK | Easy-Medium | Python/Node.js/Go developers |

## Tips for New Contributors

1. **Start small** — A well-done small PR is better than an incomplete large one
2. **Ask questions** — No question is too basic; we're here to help
3. **Read existing code** — Understanding patterns in the codebase makes your contribution smoother
4. **Test thoroughly** — Every change should have tests; `ctest` must pass
5. **One thing per PR** — Keep pull requests focused on a single change
6. **Be patient** — Reviews may take a few days; we appreciate your patience

## Need Help?

- **Discord**: [#contributing channel](https://discord.gg/aegisgate) for real-time help
- **GitHub Discussions**: [Q&A category](https://github.com/privonyx/loong-aegisgate/discussions/categories/q-a) for longer questions
- **Mentorship**: Tag `@maintainers` in your issue or PR if you need guidance

---

<a id="中文版"></a>

# Good First Issues 指南（中文版）

欢迎来到 AegisGate！本指南帮助新贡献者找到第一个任务。任务按技能级别和组件分类 — 选择一个匹配你兴趣和经验的任务。

## 如何开始

1. **浏览**下方列表，找到感兴趣的任务
2. **查看** GitHub 上标记为 [`good first issue`](https://github.com/privonyx/loong-aegisgate/labels/good%20first%20issue) 的开放任务
3. **评论**认领任务（或使用 [Good First Issue 模板](https://github.com/privonyx/loong-aegisgate/issues/new?template=good_first_issue.yml) 创建新的）
4. **阅读** [CONTRIBUTING.md](../../CONTRIBUTING.md) 了解开发环境和工作流
5. 遇到困难时在 [Discord #contributing](https://discord.gg/aegisgate) 或 [GitHub Discussions](https://github.com/privonyx/loong-aegisgate/discussions) **寻求帮助**

## 入门级：不需要 C++ 经验

涉及文档、配置、脚本或工具链 — 不需要 C++ 编码。

| 任务 | 描述 |
|------|------|
| 文档翻译 | 为仅英文的指南添加中文翻译 |
| 错误码文档 | 为每个错误码添加示例和排查建议 |
| 部署示例 | 记录常见部署模式（AWS、GCP、Azure） |
| SDK 使用示例 | 为 SDK README 添加真实场景示例 |
| FAQ 页面 | 将 Discussions 中的常见问题整理成 FAQ |
| YAML Schema | 为 aegisgate.yaml 创建 JSON Schema |

## 简单级：基础 C++ 知识

简单的 C++ 变更 — 通常是添加函数、测试或配置选项。

| 任务 | 描述 |
|------|------|
| 边界用例测试 | 测试空输入、超长字符串、Unicode 边界情况 |
| 新 PII 模式 | 添加新 PII 类型检测（护照号、驾照号） |
| 新注入模式 | 添加新 Prompt 注入技术的检测 |
| aegisctl version | 实现显示构建信息的 version 命令 |
| CLI 着色输出 | 为 CLI 添加彩色输出支持 |

## 中等级：中级 C++ 知识

在现有模块中创建新功能或进行跨模块变更。

| 任务 | 描述 |
|------|------|
| 新模型连接器 | 为新 AI 供应商添加支持（如 Cohere、AI21） |
| 请求超时配置 | 实现按模型可配置的请求超时 |
| 新 Prometheus 指标 | 添加有用的指标（缓存条目数、活跃连接数等） |
| 缓存统计端点 | 通过 API 暴露缓存统计信息 |

## 需要帮助？

- **Discord**: [#contributing 频道](https://discord.gg/aegisgate) 获取实时帮助
- **GitHub Discussions**: [Q&A 分类](https://github.com/privonyx/loong-aegisgate/discussions/categories/q-a) 提出问题
- **Mentorship**: 在 Issue 或 PR 中 @maintainers 获取指导
