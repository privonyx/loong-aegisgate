# AegisGate 故障排查与 FAQ

本文汇总常见问题与排查顺序。默认网关地址 `http://127.0.0.1:8080`，配置文件 `config/aegisgate.yaml`，模型配置 `config/models.yaml`。

## 快速检查清单

1. 进程是否监听：`curl -s http://127.0.0.1:8080/health`
2. 配置是否有效：`aegisctl config validate config/aegisgate.yaml`
3. 环境变量是否注入：`AEGISGATE_API_KEY`、各上游 `*_API_KEY`
4. 错误码：对照 [错误码参考](./error-codes_zh.md) 中的 HTTP 与 `AEGIS-xxxx`

## 网关无法启动或无法连接

| 现象 | 排查 |
|------|------|
| 端口被占用 | 修改 `server.port` 或释放占用进程 |
| 配置解析失败 | 运行 `config validate`；检查 YAML 缩进与引号 |
| 依赖库缺失（ONNX） | 确认 `LD_LIBRARY_PATH` 或关闭 ONNX 构建，使用哈希嵌入 |
| `health` 中 `initialized: false` | 查看启动日志；确认配置文件路径与 `models_config` 可读 |

```bash
./build/aegisctl health
./build/aegisctl config validate config/aegisgate.yaml --strict
```

## 常见错误码与处理

| 码 | 简要处理 |
|----|----------|
| AEGIS-1001 | 检查 `Authorization: Bearer` 与 `auth.api_keys` / 环境变量 |
| AEGIS-1003 | 管理接口使用 `auth.admin_key`，不要用普通 API Key |
| AEGIS-2001 | 降低并发或提高 `rate_limit`；区分上游 429 |
| AEGIS-2003 | 审查是否误触滥用检测；调整 `security.abuse_detection` |
| AEGIS-3001～3005 | 见下文「安全误报」 |
| AEGIS-4003 | 增大 `timeout_ms`、检查网络与上游状态 |
| AEGIS-4004 | 查看上游 API 错误体；校验 Key 与 `base_url` |
| AEGIS-5002 | 缩小请求体或增大 `limits.max_request_body_size` |
| AEGIS-9003 | 语义缓存不可用；检查嵌入模型与缓存配置 |

## 上游超时与 502/504

1. 在 `models.yaml` 中调大对应 provider 的 `timeout_ms`。  
2.  traceroute / curl 直连上游，排除防火墙与 DNS。  
3. 启用 fallback 链，避免单点模型失败即 **AEGIS-4004**。  
4. 查看审计日志中的上游片段（注意脱敏）。

## 性能：响应慢

- **未命中缓存**：调 `cache.threshold`、检查是否流式请求被策略跳过（`cache.policy`）。  
- **嵌入慢**：ONNX 首次加载、CPU 争用；考虑独立部署或降并发。  
- **护栏规则过多**：精简 `config/rules` 下规则，分环境启用。  
- **线程过多**：尝试固定 `server.threads` 为 CPU 核心附近，并用 `aegisctl bench` 对比。

```bash
./build/aegisctl --api-key "$AEGISGATE_API_KEY" \
  bench --concurrency 10 --requests 100 --model your-model
```

## 性能：内存偏高

- 降低 `cache.max_entries`、`cache.max_partitions`。  
- 缩短 `cache.ttl_seconds`。  
- 检查是否大量长上下文常驻缓存。

## 安全误报（护栏过于敏感）

1. **注入（3001）**：编辑 `config/rules/injection_patterns.yaml`，移除过宽模式；在预发复现。  
2. **PII（3002）**：调整 `pii_patterns.yaml`；业务上先脱敏再请求。  
3. **主题（3003）**：放宽 `topic_whitelist` 或用户提示约束。  
4. **编码（3005）**：调整 `security.encoding_min_base64_length` 等参数。  

每次修改后使用 `admin/reload` 或重启，并观察 metrics 中 403 比例。

## 缓存：命中率低

- 降低 `threshold`（注意误命中）。  
- 确认请求模型未被 `cache.policy.skip_models` 排除。  
- 高温、流式若被策略跳过，属预期行为。  
- ONNX 与哈希嵌入切换会改变向量空间，旧条目可能失效。

## 缓存：结果「过期」或不对

- 检查 `ttl_seconds`；关键业务可缩短 TTL。  
- 同一问题不同表述导致相似度不足：属于语义缓存固有限制。

## 日志与审计

**实时审计流（SSE）**：

```bash
./build/aegisctl --url http://127.0.0.1:8080 \
  --api-key "$AEGISGATE_ADMIN_KEY" \
  logs tail --level warn
```

说明：`--api-key` 此处必须为**管理员** Key。可选 `--level` 过滤。

**应用日志**：调整 `logging.level`（如 `debug`）用于短期诊断，生产勿长期开启。

## 配置校验命令

```bash
./build/aegisctl config validate config/aegisgate.yaml
./build/aegisctl config validate config/aegisgate.yaml --strict
```

将校验加入 CI，避免损坏 YAML 上线。

## 仍无法解决时

- 收集：`health` 输出、相关错误 JSON、`aegisgate` 启动日志片段（脱敏后）。  
- 对照 `docs/openapi.yaml` 确认请求格式。  
- 提交 Issue 时附上版本、构建选项（如 `ENABLE_ONNX`）与最小复现。

## SSE / 流式中断

若客户端在流式响应中途断开：

- 网关与上游可能仍消耗 token，属预期；客户端应妥善 `abort` 并限制重连风暴。
- 若频繁出现半包或代理超时，检查 Nginx `proxy_read_timeout`、负载均衡空闲超时等。

## `aegisctl` 常见问题

| 现象 | 处理 |
|------|------|
| `logs tail` 返回 401 | 使用管理员 Key，而非普通 API Key |
| `bench` 错误率高 | 确认 `--model` 与 `models.yaml` 中 id 一致；检查上游可用性 |
| `cache import` 失败 | 校验 JSON 格式与管理权限；查看返回体中的 `AEGIS` 码 |

环境变量 `AEGISGATE_URL`、`AEGISGATE_API_KEY` 可简化命令行，但与「管理操作用 admin key」不要混淆。

## 模型列表为空或与预期不符

- `GET /v1/models` 依赖当前加载的 `models.yaml`；修改后需重载配置或重启。
- Provider `api_keys` 若环境变量未设置，该 provider 可能无法初始化；以启动日志为准。

## Redis / Postgres（可选构建）

若启用 `-DENABLE_REDIS=ON` 或 `-DENABLE_PG=ON`：

- 连接串与池大小错误会导致运行时降级或报错；先用 `config validate` 与最小连通性测试（`redis-cli`、`psql`）。
- 网络分区下可能出现缓存未命中增多，而非进程崩溃——需结合 metrics 判断。

## 磁盘与日志打满

- `audit.log_path` 与 `logging.file`（若配置）持续增长可能占满磁盘，导致写入失败或进程异常。
- 配置 `audit.retention_days`（非 0 时按实现轮转/清理）或外挂 logrotate；并监控磁盘使用率。

## 崩溃 dump 日志（进程崩溃 / 消失）

若 `aegisgate` 进程因致命信号（`SIGSEGV`/`SIGABRT`/`SIGFPE`/`SIGILL`/`SIGBUS`）或未捕获 C++ 异常退出，进程会在退出前写出一份进程内**崩溃 dump 日志**——无需任何配置（默认始终开启）。

- **位置**：每次崩溃一个独立文件 `logs/crash-<pid>-<epoch>.log`（相对运行目录；跟随 `logging.file` 所在目录，默认 `logs/`）。同一份报告也会写到 `stderr`。
- **字段**：`version`、`reason`（如 `signal: SIGSEGV (11)` 或 `terminate: uncaught exception (<类型>): <what>`）、`time`（epoch 秒）、`pid`，以及 `backtrace:` 调用栈。
- **可读性**：`aegisgate` 可执行以 `-rdynamic`（CMake `ENABLE_EXPORTS`）链接，backtrace 中带函数名。精确源码行号需配合 core dump + `addr2line`/`gdb`（见下）。

```bash
ls -t logs/crash-*.log | head    # 最近一次崩溃在最前
cat logs/crash-<pid>-<epoch>.log
```

> 崩溃日志按内部日志同等保管：`terminate` 报告含异常 `what()` 文本，可能携带业务上下文，应与其他日志使用相同访问控制。

## OS core dump（行级分析）

进程内 backtrace 告诉你*崩溃在哪里*；core dump 让你用 `gdb` 检查*崩溃时的状态*（变量、完整线程栈）。

1. 允许产生 core dump：

```bash
ulimit -c unlimited          # 当前 shell；systemd 服务用 LimitCORE=infinity
cat /proc/sys/kernel/core_pattern   # core 落点（常被 systemd-coredump 接管）
```

2. 复现崩溃，再定位 core：

```bash
# systemd-coredump 系统：
coredumpctl list aegisgate
coredumpctl gdb aegisgate          # 对最近的 core 打开 gdb

# 普通 core 文件（core_pattern 为路径如 core.%p）：
gdb ./build/src/aegisgate core.<pid>
```

3. 在 `gdb` 中：`bt full` 看完整 backtrace，`info threads` / `thread apply all bt` 看所有线程。把崩溃日志中的裸地址映射到源码行：

```bash
addr2line -e ./build/src/aegisgate -f -C 0x<地址>
```

> 用带调试信息的构建（`-DCMAKE_BUILD_TYPE=RelWithDebInfo` 或 `Debug`）做 core dump 分析最有用；strip 过的 Release 二进制只能给出地址、符号较少。

## 版本升级后行为变化

升级主版本或 vcpkg baseline 后建议：

1. 跑一遍单元测试与关键集成测试；
2. 重新执行 `config validate --strict`；
3. 对比 `/metrics` 与采样请求延迟。

## 相关文档

- [错误码参考](./error-codes_zh.md)
- [性能调优](./performance-tuning_zh.md)
- [安全最佳实践](./security-best-practices_zh.md)
- [快速开始](./quick-start_zh.md)
