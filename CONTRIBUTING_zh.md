# 贡献指南

[English](CONTRIBUTING.md) | [中文](CONTRIBUTING_zh.md)

感谢你对 AegisGate 的关注！无论是修复拼写错误、报告 Bug、添加功能还是改进文档 — 每一份贡献都很重要。

## 目录

- [行为准则](#行为准则)
- [社区](#社区)
- [快速开始](#快速开始)
  - [环境要求](#环境要求)
  - [从源码构建](#从源码构建)
  - [运行测试](#运行测试)
- [Good First Issues](#good-first-issues)
- [如何贡献](#如何贡献)
  - [报告 Bug](#报告-bug)
  - [提交功能建议](#提交功能建议)
  - [提交代码](#提交代码)
- [开发工作流](#开发工作流)
  - [分支命名](#分支命名)
  - [提交信息规范](#提交信息规范)
  - [Pull Request 流程](#pull-request-流程)
- [代码风格](#代码风格)
- [测试规范](#测试规范)
- [文档](#文档)
- [安全](#安全)
- [许可证](#许可证)

## 行为准则

本项目遵循 [Contributor Covenant 行为准则](CODE_OF_CONDUCT.md)。参与项目即表示你同意遵守该准则。如遇不当行为，请发送邮件至 **conduct@aegisgate.dev**。

## 社区

加入我们的社区，提问、分享想法、与其他贡献者交流：

| 平台 | 用途 | 链接 |
|------|------|------|
| **GitHub Discussions** | 问答、创意、作品展示 | [Discussions](https://github.com/privonyx/loong-aegisgate/discussions) |
| **Discord** | 实时聊天、技术支持、开发协调 | [加入 Discord](https://discord.gg/aegisgate) |
| **GitHub Issues** | Bug 报告、功能请求 | [Issues](https://github.com/privonyx/loong-aegisgate/issues) |

## 快速开始

### 环境要求

- C++17 编译器（GCC 11+ 或 Clang 14+）
- CMake 3.20+
- [vcpkg](https://github.com/microsoft/vcpkg) 包管理器
- Git

### 从源码构建

```bash
git clone https://github.com/privonyx/loong-aegisgate.git
cd aegisgate

cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON

cmake --build build -j$(nproc)
```

### 可选：ONNX Embedder

启用神经嵌入引擎（需要 ONNX Runtime）：

```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DENABLE_ONNX=ON \
  -DBUILD_TESTS=ON

cmake --build build -j$(nproc)
```

### 运行测试

```bash
cd build && ctest --output-on-failure
```

提交 Pull Request 前必须确保所有测试通过。项目当前有 **96 个测试套件**，覆盖 87 个可执行文件。

## Good First Issues

初次接触 AegisGate？从这里开始！我们在 GitHub 上维护了标记为 [`good first issue`](https://github.com/privonyx/loong-aegisgate/labels/good%20first%20issue) 的初学者友好任务。

这些 Issue 的特点：
- 范围明确，有清晰的验收标准
- 不需要全面了解整个代码库
- 包含实现提示和相关代码指引

详见 [Good First Issues 指南](docs/guides/good-first-issues.md)，按技能级别整理了贡献领域。

**不确定从何开始？** 在 [GitHub Discussions](https://github.com/privonyx/loong-aegisgate/discussions/categories/q-a) 或 [Discord #contributing 频道](https://discord.gg/aegisgate) 留言 — 我们很乐意帮你找到合适的任务！

## 如何贡献

### 报告 Bug

发现 Bug？请[提交 Bug 报告](https://github.com/privonyx/loong-aegisgate/issues/new?template=bug_report.yml)，包含：

- 复现步骤
- 期望行为 vs. 实际行为
- 版本、操作系统和构建配置
- 相关日志（设置 `logging.level: debug` 获取更多详情）

**重要**：发布前请务必脱敏 API Key 和敏感信息！

### 提交功能建议

有想法？先从讨论开始：

1. **早期想法** → 发布到 [Discussions > Ideas](https://github.com/privonyx/loong-aegisgate/discussions/categories/ideas)
2. **明确的功能** → 提交 [Feature Request](https://github.com/privonyx/loong-aegisgate/issues/new?template=feature_request.yml) Issue

### 提交代码

1. 查看现有 Issue 或创建一个描述你计划做什么的 Issue
2. 在 Issue 下评论，告知你正在处理
3. Fork 仓库并创建分支
4. 按照以下规范实现变更
5. 提交 Pull Request

## 开发工作流

### 分支命名

```
feature/short-description     # 新功能
fix/short-description          # Bug 修复
docs/short-description         # 文档
refactor/short-description     # 代码重构
test/short-description         # 测试改进
```

### 提交信息规范

格式：

```
type(scope): 简短描述

可选的详细说明。
```

**类型：**

| 类型 | 用途 |
|------|------|
| `feat` | 新功能 |
| `fix` | Bug 修复 |
| `refactor` | 代码重构（无行为变更） |
| `test` | 添加或更新测试 |
| `docs` | 文档变更 |
| `chore` | 构建、CI 或工具链变更 |
| `perf` | 性能优化 |

**Scope**（可选）：受影响的模块 — `gateway`、`guardrail`、`cache`、`observe`、`auth`、`storage`、`server`、`cli`、`sdk`、`config`

**示例：**

```
feat(guardrail): add PII masking for bank card numbers
fix(gateway): correct token bucket refill timing under high concurrency
docs(sdk): update Python SDK installation instructions
test(cache): add concurrent insert/delete test for vector index
```

### Pull Request 流程

1. **Fork** 仓库并克隆
2. **创建分支**（基于 `main`）：
   ```bash
   git checkout -b feature/your-feature-name
   ```
3. **实现变更** — 遵循代码风格规范
4. **添加测试** — 每个功能或修复都应有对应测试
5. **运行测试** — 确保全部 96 个测试套件通过
6. **提交** — 使用上述规范的提交信息
7. **推送** 到你的 Fork 并向 `main` 发起 Pull Request
8. **描述变更** — 使用 [PR 模板](.github/PULL_REQUEST_TEMPLATE.md)
9. **回应审查** — 及时处理反馈

**PR 建议：**
- 保持 PR 聚焦 — 每个 PR 只包含一个逻辑变更
- 在 PR 描述中使用 "Fixes #123" 自动关联 Issue
- 对于界面/行为变更，附上截图或日志输出
- 如果 PR 还在进行中，标记为 "Draft"

## 代码风格

- **标准：** C++17 基线，C++20 特性用 `#if __cplusplus >= 202002L` 守卫
- **命名空间：** 所有代码在 `aegisgate` 下
- **头文件守卫：** 使用 `#pragma once`
- **指针：** 优先使用智能指针（`std::unique_ptr`、`std::shared_ptr`）
- **错误处理：** 初始化/配置错误使用异常；运行时使用返回值/枚举
- **日志：** 使用 spdlog + fmt 格式化
- **测试：** 每个 `.cpp` 应有对应的 `test_*.cpp`
- **格式化：** 遵循你修改的模块的现有代码风格
- **注释：** 解释"为什么"而非"做什么" — 代码本身应该能说明"做什么"

## 测试规范

- 为每个新功能和 Bug 修复编写测试
- 命名模式：`tests/test_<module_name>.cpp`
- 使用 Google Test 框架（`TEST`、`TEST_F`、`EXPECT_*`、`ASSERT_*`）
- 测试正常路径和错误情况
- 对异步/并发代码，显式测试线程安全
- 集成测试放在 `tests/integration/`
- 提交前运行完整套件：`cd build && ctest --output-on-failure`

## 文档

- 当代码变更影响用户可见行为时，更新相关文档
- 用户指南位于 `docs/guides/`
- API 变更应反映在 `docs/openapi.yaml`
- SDK 变更应更新对应的 `sdk/*/README.md`

## 安全

如果发现安全漏洞，**请勿创建公开 Issue**。请通过 [GitHub Security Advisories](https://github.com/privonyx/loong-aegisgate/security/advisories/new) 或邮件 **security@aegisgate.dev** 私下报告。详见 [SECURITY.md](SECURITY.md)。

编写代码时请遵循安全实践：
- 绝不硬编码 API Key、Token 或密码
- 使用参数化查询（禁止 SQL 字符串拼接）
- 验证和清理所有外部输入
- 优先使用 RE2 而非 std::regex（线性时间，防 ReDoS）
- 完整清单见 [安全最佳实践](docs/guides/security-best-practices.md)

## 许可证

贡献代码即表示你同意你的贡献将以 [Apache License 2.0](LICENSE) 许可。
