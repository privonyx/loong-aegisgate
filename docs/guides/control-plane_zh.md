# AegisGate 控制面使用指南

> 功能：带版本的 ConfigBundle + W3 双人审批流程（Phase 9.3）
> 适用版本：v1.1+（可选特性；构建时附加 `-DENABLE_CONTROL_PLANE=ON`）
> 二进制：`aegisgate-control-plane`（服务端）+ `aegisctl config …`（客户端）
> gRPC 契约：`api/control-plane/proto/control_plane/v1/control_plane.proto`
> OpenAPI 视图：[`control-plane-v1.yaml`](../../api/control-plane/openapi/control-plane-v1.yaml)

AegisGate **控制面**把 "谁能改网关的 YAML" 与 "改动什么时候生效" 彻底拆开。每一次改动都要经过 `apply`（提交）、第二位运维人员 `approve`（审批，SR5 强制要求 *submitter ≠ reviewer*）、显式 `activate`（激活），并写入可追溯的审计链。回滚到此前 ACTIVE 的版本只需一条命令，且**不再走一次审批流程**（R2 豁免）。

本指南介绍如何启用控制面、初始化用户、跑完一整套 apply → approve → activate 流程，并通过 mTLS 与 per-user 限流将部署收紧。

English version: [`control-plane.md`](./control-plane.md)

## 为什么需要控制面？

单文件配置（`config/aegisgate.yaml`）在几个运维共享一台主机时完全够用。但一旦进入合规/多租户场景，通常会同时需要：

1. **版本化** —— 每一次服役过的 YAML 都有 ULID、SHA-256 与不可篡改的审计链哈希可查。
2. **双人审批** —— 写改动的人不能是同时放行改动的人（SR5）。
3. **原子且可信号的激活** —— 数据面（`aegisgate`）重新装载时**不需要重启**，也不依赖 file-watcher 的竞态。
4. **历史回滚** —— 一条命令回到任意旧 ACTIVE 版本，不再重复评审。

`aegisgate-control-plane` + `aegisctl config` 就是围绕这四点在 gRPC（可选 mTLS）面上的一个小而专的实现。

## 控制面 vs. 数据面

| | 控制面 (`aegisgate-control-plane`) | 数据面 (`aegisgate`) |
|---|---|---|
| 传输 | gRPC，默认 `:9443` | HTTP/HTTPS，默认 `:8080` / `:8443` |
| 持久化 | Postgres / SQLite，存放 `config_versions`、用户、API Key、审计 | 运行态内存配置，SIGHUP 触发热加载 |
| 权限 | `SuperAdmin` 专属（SR1） | 按租户/用户/路由细分 |
| 是否可重启 | 可以，纯无状态（除 DB 外） | 不需要，SIGHUP 即可 |
| 默认是否开启 | **否** —— 需通过 `ENABLE_CONTROL_PLANE=ON` 显式启用 | 是 |

控制面没有 HTTP 面、没有请求流水线、没有路由 —— 它就是一层 gRPC 门面，后接 `ConfigServiceCore` + `AuditLogger`。

## 构建 / 安装

控制面**默认关闭**。不启用它的运维完全不承担额外的链接或二进制体积成本。

```bash
# 开启特性 + 链接依赖（grpc++, protobuf, sqlite/libpq）
cmake -B build-cp-on \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DENABLE_CONTROL_PLANE=ON \
      -DVCPKG_MANIFEST_FEATURES='control-plane'
cmake --build build-cp-on -j"$(nproc)" \
      --target aegisgate aegisgate-control-plane aegisctl
```

`vcpkg.json` 已经声明了 control-plane feature，无需额外的 `vcpkg install`。

## 快速上手（本机）

体会完整 apply → approve → activate 流程最快的方式是仓库自带的 smoke 脚本：

```bash
# 假设你已经构建过 build-cp-on/（见上）
bash scripts/test-control-plane-local.sh
```

这个脚本会：

1. 在 `127.0.0.1:19443` 启动 `aegisgate-control-plane`（dev TLS）；
2. 种入两个 SuperAdmin 用户（`alice`、`bob`）；
3. 依次执行 `config apply`（alice）→ `config approve`（bob）→ `config activate`（alice）；
4. 验证 `aegisctl config current` 返回的 `version_id` 与激活的版本一致；
5. 验证 `sha256(数据面 YAML) == sha256(aegisctl config show <vid>)`。

无论正常结束还是 Ctrl-C / SIGTERM，脚本都会在 `trap EXIT` 中彻底清理。

对具体步骤感兴趣的话，直接看源码：[`scripts/test-control-plane-local.sh`](../../scripts/test-control-plane-local.sh) —— 它本身就是一份可执行文档。

## 服务端配置（`aegisgate-control-plane.yaml`）

```yaml
edition: enterprise                 # SR1 要求 RBAC
storage:
  persistent_backend: postgres      # 单机部署可选 sqlite
  postgres:
    connection_string: >
      host=db user=aegisgate password=${PGPASSWORD}
      dbname=aegisgate_cp sslmode=verify-full
      sslrootcert=/etc/aegisgate/ca.crt

tls:                                # SR7 fail-closed：没有关闭 TLS 的开关
  cert_path: /etc/aegisgate/certs/server.crt
  key_path:  /etc/aegisgate/certs/server.key
  mutual: true                      # 要求客户端证书
  client_ca_path: /etc/aegisgate/certs/client_ca.crt
  allowed_client_fingerprints_sha256:
    - b3f1…                         # 由 `gen-control-plane-dev-certs.sh` 打印

control_plane:
  server:
    listen_address: 0.0.0.0:9443
    grpc_max_recv_msg_size_bytes: 1048576   # 与 SR2 1 MiB 上限一致
  submit_rate_limit_per_user_per_min: 10   # SR10
  max_yaml_size_bytes: 1048576              # SR2
  bootstrap_from_active_yaml: /etc/aegisgate/aegisgate.yaml   # 可选（Q5）
```

启动：

```bash
aegisgate-control-plane --config /etc/aegisgate/aegisgate-control-plane.yaml
```

信号：

| 信号 | 行为 |
|---|---|
| `SIGINT`/`SIGTERM` | 5 秒优雅关停 |

服务端**没有**关掉 TLS 的开关 —— SR7 要求默认安全。若无法签客户端证书，可以把 `mutual: false`，但传输层始终加密。

## 客户端配置（`aegisctl`）

`aegisctl` 先读 argv，再回退到环境变量：

| 标志 / 环境变量                             | 作用 |
|---------------------------------------------|---|
| `--endpoint` / `AEGISGATE_CP_ENDPOINT`      | 服务端 `host:port`（默认 `localhost:9443`） |
| `--tls-ca` / `AEGISGATE_CP_TLS_CA`          | 用于校验服务端证书的 CA 文件 |
| `--tls-cert` / `AEGISGATE_CP_TLS_CERT`      | mTLS 客户端证书 |
| `--tls-key` / `AEGISGATE_CP_TLS_KEY`        | mTLS 客户端私钥 |
| `--insecure-plaintext`                      | **仅开发**。跳过 TLS；发布版拒绝该标志 |
| `--output {table,json}`                     | 读命令的输出格式 |
| `AEGISGATE_CP_API_KEY`                      | 传递 Bearer API Key 的**唯一**途径（SR8），**绝不**走 CLI 参数 |

SR8 背后的原因：命令行参数会出现在 `ps`、shell 历史、进程监控里；环境变量不会，除非用户主动把它写出去。

## W3 双人审批流程

```text
PENDING  -- approve (reviewer ≠ submitter) --> APPROVED
PENDING  -- reject  (reviewer ≠ submitter) --> REJECTED      （终态）
APPROVED -- reject                         --> REJECTED      （终态）
APPROVED -- activate                       --> ACTIVE   （旧 ACTIVE -> SUPERSEDED）
ACTIVE   -- rollback-to (更早的 ACTIVE)    --> 仍为 ACTIVE（幂等）
SUPERSEDED -- rollback-to                  --> ACTIVE        （R2 豁免）
```

每一次状态迁移都会写一条带链式 SHA-256（`chain_hash`）的审计记录，整条序列可后验不可篡改。

### 端到端示例

```bash
# copied from EN
export AEGISGATE_CP_ENDPOINT=cp.aegisgate.internal:9443
export AEGISGATE_CP_TLS_CA=/etc/aegisgate/certs/ca.crt

# alice 提交
export AEGISGATE_CP_API_KEY="${ALICE_TOKEN}"
aegisctl config apply ./aegisgate.yaml --comment 'bump llm pool to 32 workers'
#   Submitted:
#   version_id: 01HZ2…V4W
#   status:     PENDING
#   sha256:     9e1b…
#   …

# bob 审查并批准
export AEGISGATE_CP_API_KEY="${BOB_TOKEN}"
aegisctl config diff 01HZ2…V4W            # 先看改了啥
aegisctl config approve 01HZ2…V4W --comment 'LGTM — workers doubled for LLM surge'

# alice 激活，指定数据面 yaml 落地路径与待 SIGHUP 的 PID
export AEGISGATE_CP_API_KEY="${ALICE_TOKEN}"
aegisctl config activate 01HZ2…V4W \
    --comment 'activate — coordinated with sre-oncall' \
    --data-plane-config-path /etc/aegisgate/aegisgate.yaml \
    --signal-pid $(pgrep -x aegisgate)
```

`config activate` 会依次执行（任意一步失败都会硬回滚）：

1. gRPC `ActivateVersion` —— 服务端在 DB 原子切换 `status`；
2. 原子文件写 —— 先把新 YAML `fsync` 到同目录的 tempfile，再 `rename(2)` 到 `--data-plane-config-path`；
3. `kill -HUP <signal-pid>` —— 数据面原地重载。

若第 2 步或第 3 步失败，CLI 会补发一次 `RollbackVersion`，避免集群状态漂移。

## mTLS 配置

开发用的证书（一次性 CA + 明文私钥）可以用：

```bash
scripts/gen-control-plane-dev-certs.sh /etc/aegisgate/certs/dev
# -> ca.crt, server.{crt,key}, client.{crt,key}, client.sha256
```

把打印出来的 `client.sha256` 填到服务端配置的 `tls.allowed_client_fingerprints_sha256` 里即可。生产环境请用你们自己的 CA，并按常规密钥管理流程轮换；控制面只需要 `ca.crt` 校验 + 指纹名单放行。

## 可观测性

控制面发出的 Prometheus 指标族与数据面同风格：counter `control_plane_rpc_total{rpc,status}`、histogram `control_plane_rpc_latency_ms_bucket{rpc}`、gauge `control_plane_audit_chain_length`。把 Prometheus 的 scrape 指到 `:9443/metrics`，再按 `rpc="activate"` 做看板即可得到变更可观测性。

审计事件落在 `audit_events` 表，`chain_hash` 指向控制面命名空间（`config_version:*`）。链完整性检查与数据面共用 `aegisctl audit verify`（Phase 7 上线）。

## 故障排查

| 现象 | 可能原因 | 解决方法 |
|---|---|---|
| `PERMISSION_DENIED: SuperAdmin role required` | 解析出的用户不是 `super_admin` | 用 `aegisctl users set-role` 提权，或换一个 SuperAdmin 账号重发 API Key（SR1） |
| `FAILED_PRECONDITION: submitter cannot approve own version` | 同一个用户同时做了 `apply` 与 `approve` | SR5 禁止自批自。换另一个运维账号。 |
| `config apply` 收到 `RESOURCE_EXHAUSTED: rate limit` | 单用户每分钟超过 10 次 | 提高 `control_plane.submit_rate_limit_per_user_per_min`，或降低提交频率（SR10） |
| `INVALID_ARGUMENT: yaml exceeds 1048576 bytes` | 提交的 YAML 过大 | SR2 上限 —— 拆包或把重复块抽公共段 |
| `UNAUTHENTICATED: missing bearer token` | `AEGISGATE_CP_API_KEY` 没设 | SR8：Token **只能**走环境变量，不能放命令行 |
| 数据面 `activate` 后没重载 | `--signal-pid` 错了，或发信号的用户没权限 | 先 `pgrep aegisgate` 确认 PID，再确认运行控制面 CLI 的用户有 kill 权限 |
| `activate` 报成功但数据面仍在用旧 YAML | 目标路径不是数据面真正读的文件 | `ps -fp $(pgrep aegisgate)` 看 argv 里的路径，与 `--data-plane-config-path` 对齐 |

## 运维 SLO

| SLO | 目标 | 设计依据 |
|---|---|---|
| `config apply` p95 延迟 | < 150 ms（1 MiB yaml 场景） | 验证 + RE2 SR4 扫描 + SHA-256 |
| `config activate` p95 延迟 | < 100 ms（DB 事务 + fsync） | SR7/R2 先保一致 |
| 控制面冷启动 | < 3 s | 除 DB 外无状态 |
| 审计链完整性校验 | O(n)，5k 行/s | 只跑 SHA-256 |

## 路线图

- **Phase 9.3.3** —— Argo CD 参考部署；`aegisctl config apply` 成为 Argo CD 的 PostSync hook。
- **Phase 9.4** —— Kubernetes Operator（`aegisgateconfigs.aegisgate.io`），从 CR 收敛到控制面。
- **Phase 9.5** —— 多租户接入面、按租户分配审批人。

完整规划请看 [`docs/ROADMAP_zh.md`](../ROADMAP_zh.md)。

## 参考资料

- gRPC 契约：[`api/control-plane/proto/control_plane/v1/control_plane.proto`](../../api/control-plane/proto/control_plane/v1/control_plane.proto)
- OpenAPI 视图：[`control-plane-v1.yaml`](../../api/control-plane/openapi/control-plane-v1.yaml)
- 集成脚本：[`scripts/test-control-plane-local.sh`](../../scripts/test-control-plane-local.sh)
- 开发证书：[`scripts/gen-control-plane-dev-certs.sh`](../../scripts/gen-control-plane-dev-certs.sh)
- OpenAPI ↔ proto 校验：[`scripts/verify-openapi-sync.sh`](../../scripts/verify-openapi-sync.sh)
