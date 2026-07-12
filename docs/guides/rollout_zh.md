# 灰度发布指南 — AegisGate Phase 9.3.4

> 分阶段配置灰度发布，支持指标门禁和自动回滚。

## 架构概览

```
 ┌─────────────┐     gRPC      ┌────────────────────┐
 │  aegisctl   │────────────>│  RolloutService     │
 │  rollout *  │              │  (控制面)            │
 └─────────────┘              │                     │
                              │  RolloutController  │
                              │    ├─ 状态机         │
                              │    ├─ Ticker (1s)    │
                              │    ├─ 指标提供者      │
                              │    └─ 审计桥接        │
                              └────────┬────────────┘
                                       │ 合并 YAML
                              ┌────────▼────────────┐
                              │  数据面               │
                              │  resolveActiveConfigId│
                              │  RouterOutcome →     │
                              │    FeedbackBus       │
                              └─────────────────────┘
```

## 快速开始

### 1. 建立基线活跃配置

```bash
export AEGISGATE_CP_API_KEY="$YOUR_KEY"
aegisctl config apply config/new.yaml --comment "v2 功能"
aegisctl config approve <version_id> --comment "审核通过"
aegisctl config activate <version_id> --comment "上线"
```

### 2. 创建灰度发布规格

```yaml
# rollout-spec.yaml
target_version_id: "01ABC..."    # 必须是 APPROVED 状态
sticky_key: "tenant_id"
auto_rollback_on_pause: true
auto_rollback_grace_seconds: 600
creator_comment: "v2 灰度发布"
stages:
  - name: canary
    scope:
      tenant_globs: ["beta-*"]
      regions: ["us-east-1"]
      percentage: 5
    observation:
      min_duration_seconds: 300
      min_sample_count: 1000
    auto_pause:
      error_rate_gt: 0.02
      p99_latency_ratio_gt: 2.0
      absolute_error_rate_gt: 0.10
      absolute_p99_latency_ms_gt: 5000
  - name: full
    scope:
      percentage: 100
    observation:
      min_duration_seconds: 600
      min_sample_count: 5000
    auto_pause:
      error_rate_gt: 0.01
      p99_latency_ratio_gt: 1.5
```

### 3. 执行灰度发布

```bash
aegisctl rollout create --spec rollout-spec.yaml
aegisctl rollout start --rollout-id <id>
# 监控状态
aegisctl rollout status --rollout-id <id> --output json
# 推进到下一阶段
aegisctl rollout promote --rollout-id <id>
```

## 指标门禁语义

Ticker 每秒评估每个 PROGRESSING 状态的灰度发布：

| 阈值 | 类型 | 含义 |
|------|------|------|
| `error_rate_gt` | 相对值 | 目标错误率 − 基线错误率 |
| `p99_latency_ratio_gt` | 相对值 | 目标 p99 / 基线 p99 |
| `absolute_error_rate_gt` | 绝对值 | 安全网（防止基线自身恶化导致漏报） |
| `absolute_p99_latency_ms_gt` | 绝对值 | 硬性 p99 延迟上限 |

观察期内 `min_duration_seconds` 和 `min_sample_count` 必须同时满足后，自动暂停判定才会生效。

## 自动回滚

当 `auto_rollback_on_pause: true` 时：

1. 指标越过阈值 → 灰度发布自动暂停
2. 宽限计时器启动（`auto_rollback_grace_seconds`）
3. 如果在宽限期内未手动恢复 → 状态变为 FAILED，恢复为之前的活跃配置版本

**熔断开关 (SR17)：** 设置 `AEGISGATE_DISABLE_AUTO_ROLLBACK=1` 可全局禁用自动回滚。

## CLI 参考

| 命令 | 描述 |
|------|------|
| `aegisctl rollout create --spec FILE` | 创建灰度发布 |
| `aegisctl rollout start --rollout-id ID` | 启动灰度发布 |
| `aegisctl rollout status --rollout-id ID` | 查询状态 |
| `aegisctl rollout list [--output json]` | 列出所有灰度发布 |
| `aegisctl rollout pause --rollout-id ID` | 手动暂停 |
| `aegisctl rollout resume --rollout-id ID` | 恢复灰度发布 |
| `aegisctl rollout promote --rollout-id ID` | 推进到下一阶段 |
| `aegisctl rollout abort --rollout-id ID` | 中止并恢复原配置 |

所有命令支持 `--output json` 和 `--output table`（默认）。

## 故障排查

| 错误 | 原因 | 解决方案 |
|------|------|----------|
| `FAILED_PRECONDITION: target version not APPROVED` | 目标配置未审批 | 先执行 `aegisctl config approve` |
| `FAILED_PRECONDITION: active rollout exists for target` | 存在并发灰度发布冲突 | 先中止已有灰度发布 |
| `RESOURCE_EXHAUSTED: tenant quota exceeded` | SR16: 24 小时内灰度发布数过多 | 等待配额窗口过期 |
| 状态卡在 PAUSED | 指标触发自动暂停 | 检查 `pause_detail` 字段；修复问题或 `abort` |
| `PERMISSION_DENIED` | 缺少 SuperAdmin 角色 (SR1) | 确认 API Key 具有 SuperAdmin 权限 |
