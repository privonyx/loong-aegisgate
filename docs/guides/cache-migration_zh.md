# 缓存迁移工具指南

> 覆盖功能：Phase 6.2 `aegisctl cache dump` / `aegisctl cache restore`
> 可用版本：v1.2+（TASK-20260513-01 落地）

`aegisctl cache` 子命令把语义缓存内容**离线**导出 / 导入。它解决两类场景：

1. **跨集群迁移**：把一个集群（hnsw 后端）的缓存搬到另一个集群（Milvus / Qdrant 后端）。
2. **冷启动加速**：用生产环境过去 7 天的缓存预热新部署的集群。

设计上**完全不依赖在线流量**：dump 走 `VectorStore::enumerate()` 只读快照，restore 是幂等 upsert。

## 二进制快照格式

```
magic       : "AGCM" (4 bytes)
version     : uint32  (current = 1)
dim         : uint32  (向量维度，与生成方一致)
entries     : uint64  (条目总数)
records     : N × {
    cache_key_len : uint32
    cache_key     : bytes
    tenant_id_len : uint32
    tenant_id     : bytes
    response_len  : uint32
    response      : bytes
    vector        : float[dim]
    expire_at     : int64  (epoch seconds; 0 = no expire)
}
tail_sha256 : 32 bytes (SR2 — 全文件除自身的 SHA-256)
```

格式刻意简单：单文件、self-describing、无第三方序列化依赖。

## 使用

### Dump（导出）

```bash
export AEGISGATE_CP_API_KEY="$(echo -n 'admin-secret' | sha256sum | awk '{print $1}')"
aegisctl cache dump \
    --output /tmp/cache-snapshot.bin \
    --tenant tenant-acme \    # 可选：仅导出指定租户
    --max-entries 100000      # 可选：硬上限（防误操作）
```

输出末尾会打印记录数 + 文件大小 + SHA-256（也写到了文件尾，方便比对）。

### Restore（导入）

```bash
export AEGISGATE_CP_API_KEY="$(echo -n 'admin-secret' | sha256sum | awk '{print $1}')"
export AEGISGATE_CACHE_MIGRATE_TENANT_ALLOW="tenant-acme,tenant-foo"
aegisctl cache restore \
    --input /tmp/cache-snapshot.bin \
    --skip-expired           # 可选：跳过 expire_at < now 的条目
```

Restore 之前 CLI 会**先 verify 文件尾的 SHA-256**（SR2），任何字节翻转会立即 abort 并退出码 2，不写入半条数据。

## 安全（SR2 + SR8）

- **SR2 快照完整性**：tail SHA-256 是文件被读取时第一件事；任何中途篡改 / 截断 / 拼接都会被检测到。文件传输建议直接走 `scp` 或 `sha256sum -c` 二次校验。
- **SR8 CLI API key**：`aegisctl cache dump|restore` 都强制要求 `AEGISGATE_CP_API_KEY` 环境变量与 control plane 的 admin token sha256 匹配；缺失或不匹配立即 exit 2，不输出任何用户数据。
- **租户白名单**（推荐）：restore 时设 `AEGISGATE_CACHE_MIGRATE_TENANT_ALLOW=tenant-a,tenant-b`，Migrator 会丢弃不在白名单内的条目，防止"快照里混进了不该到目标集群的租户"。

## 失败码

| 退出码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | I/O / VectorStore 错误 |
| 2 | 安全检查失败（API key / SHA-256 / 白名单未通过） |
| 3 | 格式不识别（magic 字节不匹配 / version 不支持） |

## 验证

```bash
# Dump → restore round-trip
aegisctl cache dump --output /tmp/a.bin
sha256sum /tmp/a.bin

# 验证 SR2：篡改一个字节再 restore，必须 exit 2
dd if=/dev/urandom of=/tmp/a.bin conv=notrunc bs=1 count=1 seek=100
aegisctl cache restore --input /tmp/a.bin   # → "snapshot tail sha256 mismatch", exit 2
```

`tests/unit/cache/test_cache_migrator.cpp` 中已经有覆盖 SR2 字节翻转 / SR8 缺 key / 白名单生效的 mutation 实验。

## 相关链接

- 设计文档：`docs/specs/2026-05-13-phase6-completion-design.md` §8
- 实现计划：`docs/plans/2026-05-13-phase6-completion.md` §6
- CLI 源：`src/cli/aegisctl.cpp`（`cache dump|restore` dispatcher）
