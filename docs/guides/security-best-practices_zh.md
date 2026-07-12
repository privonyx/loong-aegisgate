# AegisGate 安全最佳实践

网关暴露在公网或内网关键路径上时，建议同时加固**认证**、**传输**、**护栏规则**与**运维面**。以下与默认配置 `config/aegisgate.yaml` 及 `config/rules/*.yaml` 对应。

## API Key 管理

- **勿将密钥写入 Git**：使用 `${ENV}` 占位，在部署环境注入 `AEGISGATE_API_KEY`、`OPENAI_API_KEY` 等。
- **网关 Key 与上游 Key 分离**：`auth.api_keys` 仅用于客户端访问网关；真实模型密钥只在 `config/models.yaml` 的 provider 中配置。
- **轮换**：新增 Key → 客户端逐步切换 → 从列表移除旧 Key。避免单点泄露导致长期敞口。
- **最小权限**：若未来区分多 Key 能力，按业务线拆分 Key，便于吊销与审计。

## 管理员 Key（Admin）

- `auth.admin_key` 同样支持 `${AEGISGATE_ADMIN_KEY}` 形式从环境读取。
- **与业务 API Key 不同**：`aegisctl` 访问 `/admin/reload`、`/admin/logs/stream`、`/admin/cache/import` 时应使用管理员 Key（通过 `--api-key` 传入的是 Bearer Token，此处应为 admin）。
- 生产环境应**高强度随机**、仅存于密钥管理器或编排系统 Secret。

## 护栏配置

### 注入检测

文件：`config/rules/injection_patterns.yaml`  

- 定期审查规则，避免过度宽泛导致误杀（见[故障排查](./troubleshooting_zh.md)）。
- 与 `security` 节下的启发式开关配合使用；日志中关注 **AEGIS-3001**。

### PII

文件：`config/rules/pii_patterns.yaml`  

- 默认使用 RE2，注意规则复杂度与维护成本。
- 对合规场景：宁可拦截后日志告警，也不要把明文 PII 写入第三方模型。

### 主题边界

文件：`config/rules/topic_whitelist.yaml`（及企业版自定义规则）  

- 白名单/黑名单与业务域对齐；变更前在预发环境验证 **AEGIS-3003** 触发率。

### 编码与 Unicode

`aegisgate.yaml` 中：

```yaml
security:
  unicode_normalization: true
  encoding_detection: true
  encoding_min_base64_length: 20
```

- 对合法 Base64 业务负载，若误报 **AEGIS-3005**，可适当调高 `encoding_min_base64_length`（需评估安全边界）。

### 滥用检测

`security.abuse_detection`：窗口、警告/节流/封禁阈值与时长，以及基于 64-bit SimHash 的
**内容相似度聚类**（`similarity_*` 配置项）。

- 公网入口建议**偏严格**，内网工具链可适当放宽。
- 相似度指纹为 **节点本地**（即使拒绝计数走 Redis 也不跨节点同步指纹）。
- 若合法模板流量误报偏高，可调高 `similarity_hamming_threshold`（默认 `3`）。
- 与 **AEGIS-2003** 关联；调整后观察审计与 metrics。

## TLS

```yaml
tls:
  enabled: true
  port: 0          # 0 或未设时可用 HTTP 端口+1，或通过 AEGISGATE_TLS_PORT 覆盖
  cert_path: "/path/to/fullchain.pem"
  key_path: "/path/to/privkey.pem"
```

- 生产环境对外仅开放 HTTPS；证书定期续期（Let’s Encrypt 或内部 PKI）。
- 反向代理（Nginx/Caddy）终止 TLS 时，仍需保证**到网关**一段网络可信（或配置 mTLS，视架构而定）。

## 规则热重载

配置变更后可通过管理接口重载（需 **admin_key**），避免频繁重启进程：

```bash
curl -s -X POST http://127.0.0.1:8080/admin/reload \
  -H "Authorization: Bearer ${AEGISGATE_ADMIN_KEY}"
```

- **流程建议**：Git 管理规则 → CI 校验 YAML → 发布到服务器 → `reload` → 抽样验证。
- **权限**：仅运维角色持有 admin；操作记入审计。

## 审计日志

```yaml
audit:
  log_path: "logs/audit.log"
  retention_days: 0
```

- 日志可能含请求摘要；磁盘与权限（`chmod`、专用用户）需限制。
- 长期保留时对接集中日志（SIEM），并做脱敏策略。
- 实时查看：`aegisctl --api-key "$AEGISGATE_ADMIN_KEY" logs tail --level warn`

## 内容过滤（输出侧）

- 配置输出护栏动作（替换、截断、告警）时，明确**失败关闭**还是**失败放行**的业务后果。
- **AEGIS-3004** 频繁时，区分模型胡言与规则过严。

## 观测与告警

- **`/metrics`**：为 401/403/429/5xx 设置告警阈值。
- **成本与配额**：防止 Key 泄露导致账单异常（见 `observe` 相关配置与持久化）。

## 配置校验

发布前执行：

```bash
./build/aegisctl config validate config/aegisgate.yaml
./build/aegisctl config validate config/aegisgate.yaml --strict
```

`--strict` 将警告视为错误，适合 CI。

## 审计完整性与持久化

SQLite 后端（`storage.persistent_backend: sqlite`）常用于审计与成本数据。建议：

- 数据库文件与备份权限限制在网关运行用户；
- 定期备份 `data/aegisgate.db`（或你自定义的路径），并验证恢复流程；
- 审计链若包含完整性哈希（FNV-1a 等实现细节以源码为准），篡改检测应配合**只追加**的远程存储策略。

## 反向代理与真实 IP

若网关前置 Nginx/Ingress，需注意：

- 信任链：仅在代理可靠时，才根据 `X-Forwarded-For` 做限流或审计归因；
- 避免将管理路径 `/admin/*` 暴露到公网；可用网络策略或独立监听地址限制来源。

具体头部行为以当前 Drogon 与项目中间件实现为准，升级版本后复核一次。

## 企业版与功能开关

`edition: enterprise` 与 `features` 节控制路由、护栏、管理面板等能力。Community 部署勿误开企业特性依赖的配置项，以免启动失败或静默降级。许可证文件路径见 `license_file`，勿提交到版本库。

## 供应链与依赖

- 使用 vcpkg baseline 锁定依赖版本，减少供应链漂移；
- 生产镜像构建尽量**可重现**（同一 tag 对应同一依赖树）；
- 对 Drogon、yaml-cpp、RE2 等安全公告保持订阅。

## 事件响应

一旦发现 API Key 泄露：

1. 立即从 `auth.api_keys` 与上游 Key 列表中吊销对应项；
2. `POST /admin/reload` 或滚动重启使配置生效；
3. 审计日志中按时间窗口排查异常调用；
4. 轮换所有可能受影响的密钥。

## 相关文档

- [错误码参考](./error-codes_zh.md)
- [故障排查](./troubleshooting_zh.md)
- [快速开始](./quick-start_zh.md)
