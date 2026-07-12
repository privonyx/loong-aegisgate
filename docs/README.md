# AegisGate Documentation

Central index for all AegisGate guides and references. New here? Start with the
[5-Minute Quickstart](quickstart.md).

> 中文文档索引：[文档总览](README_zh.md)

## Getting Started

| Guide | Description |
|-------|-------------|
| [5-Minute Quickstart](quickstart.md) | Zero-build Docker walkthrough — see cache savings in 5 minutes |
| [Quick Start](guides/quick-start.md) | Build from source, minimal configuration, first chat call |
| [Usage Examples](guides/usage-examples.md) | End-to-end curl session covering all core features |
| [Savings Estimate](estimate.md) | Project your token/cost savings with `aegisctl estimate` |

## Architecture & Operations

| Guide | Description |
|-------|-------------|
| [Architecture](guides/architecture.md) | Pipeline framework, routing, storage, sequence diagrams |
| [Production Deployment](guides/production-deployment.md) | Docker, Helm, cluster mode, hardware requirements |
| [Performance Tuning](guides/performance-tuning.md) | Throughput, latency, and resource optimization |
| [Multi-Region](guides/multi-region.md) | Geo-distributed deployment topology |
| [Rollout](guides/rollout.md) | Progressive rollout and canary strategies |
| [Control Plane](guides/control-plane.md) | Centralized configuration and fleet management |
| [Troubleshooting](guides/troubleshooting.md) | Common issues and resolution steps |
| [OTEL Offline Deps](guides/otel-offline-deps.md) · [OTEL Verification](guides/otel-verification.md) | OpenTelemetry setup and validation |

## Security & Compliance

| Guide | Description |
|-------|-------------|
| [Security Best Practices](guides/security-best-practices.md) | Hardening, secrets, deployment security |
| [External Safety](guides/external-safety.md) | OpenAI Moderation / Perspective API integration |
| [Adaptive Guard](guides/adaptive-guard.md) · [Guard Model](guides/guard-model.md) | ONNX safety classification |
| [Error Codes](guides/error-codes.md) | Full `AEGIS-xxxx` error code reference |
| [Compliance Mapping](compliance/README.md) | EU AI Act / ISO 42001 / China measures readiness |

## Features & Integration

| Guide | Description |
|-------|-------------|
| [Feature List](feature-list.md) | Complete capability inventory grouped by module, with edition attribution, key endpoints/config, and per-item guide links |
| [SDK Integration](guides/sdk-integration.md) | Python / Node.js / Go client SDKs |
| [Admin API](guides/admin-api.md) | Admin REST API and WebSocket endpoints |
| [Admin Savings](guides/admin-savings.md) | Savings dashboard and reporting |
| [Cost Optimization](guides/cost-optimization.md) · [Cost Autonomy](guides/cost-autonomy.md) | Budget control and autonomous cost governance |
| [Multimodal](guides/multimodal.md) | Embeddings, images, audio proxy endpoints |
| [Conversation Cache](guides/conversation-cache.md) · [Cache Migration](guides/cache-migration.md) | Semantic cache behavior and migration |
| [Provider Manifest](guides/provider-manifest.md) | Model/provider definition format |
| [Feedback Bus](guides/feedback-bus.md) | Event/feedback pipeline |

## Example Apps

| App | Description |
|-----|-------------|
| [Showcase Demo](../apps/showcase/README.md) | LLM→AegisGate→App reference demo: generic skeleton + pluggable scenarios (AI comic-drama flagship / e-commerce validation), with a value panel that surfaces cost savings and compliance |

## API Reference

| Reference | Description |
|-----------|-------------|
| [OpenAPI Spec](openapi.yaml) | Machine-readable API definition |
| [OpenAI Compatibility Matrix](openai-compat-matrix.md) | Supported OpenAI API surface |

## Positioning & Vision

| Document | Description |
|----------|-------------|
| [AegisOps Vision](positioning/aegisops-vision.md) | Product/platform layer strategy |

## Contributing & Development

| Guide | Description |
|-------|-------------|
| [Good First Issues](guides/good-first-issues.md) | Beginner-friendly tasks by skill level |
| [Workflow 2.0](guides/workflow-2.0.md) | Development workflow |
| [macOS Development](guides/macos-development.md) | macOS build/dev notes |
| [Upgrade to v1.0](guides/upgrade-v1.0.md) | Upgrade guide |

## Project Root References

- [CHANGELOG](../CHANGELOG.md) · [VERSIONING](../VERSIONING.md) · [CONTRIBUTING](../CONTRIBUTING.md) · [SECURITY](../SECURITY.md) · [CODE_OF_CONDUCT](../CODE_OF_CONDUCT.md) · [ADOPTERS](../ADOPTERS.md)
