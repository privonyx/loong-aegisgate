# Changelog

All notable changes to this project are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Redis-backed circuit breaker state for multi-node deployments, enabling
  coordinated failover across gateway replicas (falls back to in-memory state
  when Redis is unavailable).
- Per-route semantic cache similarity thresholds for fine-grained control over
  cache hit rates.
- `aegisctl` configuration validation subcommand.
- Admin dashboard savings analytics.
- Advanced routing license gate for enterprise editions.
- Abuse detection via content-similarity clustering.
- Sample Prometheus alerting rules.

### Changed

- Session idle timeout now defaults to 3600s (was 1800s).
- Circuit breaker thresholds are now configurable per provider.
- Expanded PII filter entity coverage.
- Expanded content moderation categories.

### Fixed

- Fixed a race condition in the Redis rate limiter that could allow brief
  bursts above the configured limit under high concurrency.
- Fixed circuit breaker state inconsistency after the reset timeout elapsed.
- Fixed streaming requests not retrying across multiple API keys on failure.
- Fixed packaging of configuration files that were previously omitted.

## [1.0.0] - 2026-06-15

Initial public release of AegisGate — a single-binary, OpenAI-compatible LLM
gateway with intelligent routing, semantic caching, and enterprise-grade
guardrails.

### Added

- **OpenAI-compatible API** — `/v1/chat/completions`, `/v1/embeddings`, and
  `/v1/models` endpoints; point an existing OpenAI SDK at AegisGate.
- **Intelligent routing** — model-based, cost-aware, and geo-aware routing
  strategies with automatic failover and circuit breaking.
- **Semantic caching** — embedding-based response cache that reduces cost and
  latency for repeated queries.
- **Enterprise guardrails** — PII filtering, prompt-injection detection, and
  content moderation with configurable policy rules.
- **Cost controls** — per-API-key and per-tenant token-bucket rate limiting
  with budget tracking.
- **Observability** — Prometheus metrics, structured logging, and OpenTelemetry
  tracing support.
- **Multi-tenancy** — tenant isolation with role-based access control (RBAC).
- **Flexible deployment** — single binary, Docker, or Kubernetes (Helm chart),
  with optional Redis for clustered state.

[Unreleased]: https://github.com/privonyx/loong-aegisgate/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/privonyx/loong-aegisgate/releases/tag/v1.0.0
