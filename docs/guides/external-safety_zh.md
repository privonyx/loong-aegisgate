# 外部安全 API 与影子模式指南

> 覆盖功能：Phase 6.3 `ExternalSafetyStage`（OpenAI Moderation + Google Perspective）+ shadow_mode（Epic 4 新增）
> 可用版本：v1.2+（TASK-20260513-01 接入 shadow_mode）

`ExternalSafetyStage` 是 AegisGate 的 L4 内容安全护栏，在本地 PII / Topic / Guard 之后调用云端审核 API（OpenAI Moderation、Google Perspective），把它们的判决并入护栏链。

## 它解决什么问题

- 法规 / 客户合规要求"必须调云端 Moderation"
- 不同云 provider 对同一段文本的 false positive / false negative 差异很大，要支持多 provider 投票
- **新引入一个云 provider 时，先观察 1-2 周判决再决定要不要真切到 Reject 路径** — 这就是 Epic 4 新加的 `shadow_mode`

## 同步（默认）模式

```yaml
security:
  external_safety:
    enabled: true
    mode: any              # any | all | majority — 何时触发 Reject
    fail_policy: open      # open: API 失败放行；closed: API 失败拒绝
    parallel: true         # 多 provider 并发调用
    openai_moderation:
      enabled: true
      api_key: "${OPENAI_API_KEY}"
    perspective:
      enabled: true
      api_key: "${PERSPECTIVE_API_KEY}"
      threshold: 0.7
```

行为：`process()` 串行/并行调每个启用的 provider，`mode=any` 任一返 flagged 就 Reject。

## Shadow 模式（Epic 4）

```yaml
security:
  external_safety:
    enabled: true
    mode: any
    fail_policy: open
    shadow_mode: true              # ← 关键开关
    shadow_max_inflight: 1000      # SR6 背压上限
    shadow_audit_ttl_seconds: 86400 # SR3 审计保留 24h
    openai_moderation:
      enabled: true
      api_key: "${OPENAI_API_KEY}"
```

行为变化：

1. `process()` **立即返回 Continue**（< 10 ms,与 provider 实际延迟无关）
2. 通过 `std::async` 异步调 provider，结果写入审计日志，标记 `shadow=true`
3. 主请求路径不被阻塞，不会因为 provider 慢 / 错而 Reject
4. 当并发 in-flight shadow worker 超过 `shadow_max_inflight` 时跳过新 dispatch（SR6 背压）

启动日志会出现：

```
ExternalSafetyStage: L4 active with 1 provider(s), mode=any, fail=open, shadow=on (cap=1000, ttl=86400s)
```

## 推荐 rollout 流程

1. **第 1 周** `shadow_mode: true` — 上线，从 audit log 拉 `shadow=true` 标记的判决，统计 FP/FN 比例
2. **第 2 周** 调阈值（OpenAI 是模型级，Perspective 是 `threshold`）
3. **第 3 周** `shadow_mode: false`,切真实拦截

任何阶段都可以**回退到 shadow** 而不影响业务流量。

## 安全（SR3 + SR6）

- **SR3 shadow audit**：每次 shadow 扫描必写审计 entry,`action="external_safety_shadow"`,`detail=shadow=true; <provider>:flagged|ok|error;...`,`tenant_id` 字段保留。运维和合规可以通过审计日志比较 shadow 与真模式的差异。
- **SR6 inflight 背压**：默认 1000 个并发 shadow worker,超过即 skip + 一条 warn 日志,不会无限制堆积线程。
- **fail-open + shadow_mode 互不抵消**：shadow_mode 时 process() 必返 Continue,无关 fail_policy；切回真同步时 fail_policy 才生效。

## Mutation 实证

- 关掉 audit_logger->logAction 调用 → SR3 测试 `Sr3ShadowWritesAuditTaggedShadow` 立即 FAIL（已在 Epic 4 退出验证 Checkpoint 4 实测）。
- 把 `shadow_max_inflight` 设 0 → SR6 测试 `ConfiguredMaxInflightHonored` 验证所有 dispatch 都被 skip。

## 常见问题

- **shadow_mode 会增加成本吗？** 会。Provider 仍然真实调用,只是判决不影响主路径。监控 `shadow_dispatched` / `shadow_skipped` counter 评估额外成本。
- **能不能只对部分租户开 shadow？** 当前不支持租户级开关；`ExternalSafetyStage` 是全局 stage。可以在 plugin 层做 tenant 过滤。
- **shadow 路径有回压吗？** 有。`shadow_max_inflight` 是硬上限,超过的请求 process() 仍立即返回 Continue,只是没有 audit 记录。

## 验证

- `tests/unit/guardrail/test_external_safety_shadow.cpp` 12 个测试,覆盖:
  - `ShadowDisabledKeepsSyncBlocking`(默认行为不变)
  - `ShadowReturnsContinueImmediately`(< 20ms,30ms provider)
  - `Sr3ShadowWritesAuditTaggedShadow`(SR3 审计标记)
  - `Sr6InflightCapSkipsOverflow`(SR6 背压)
  - `SlowProviderStaysUnderTenMs`(Epic 4.3 集成: 500ms provider, < 10ms process)
  - `BurstStaysFireAndForget`(Epic 4.3: 5 burst < 50ms)

## 相关链接

- 设计文档：`docs/specs/2026-05-13-phase6-completion-design.md` §7
- 实现计划：`docs/plans/2026-05-13-phase6-completion.md` §7
