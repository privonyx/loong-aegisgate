#!/usr/bin/env python3
"""签发 AegisGate 企业版【测试】License（ChecksumV1）。

仅用于本地验收测试，不用于生产授权。校验逻辑见 src/core/feature_gate.cpp
(validateLicenseKey)：license_key 末 16 位 == sha256(payload + SALT)[:16]，
payload = "edition:customer:expires[:sorted,comma,features]"。

不带 --feature 时不写 features 字段 => 服务端 enableAllFeatures()（全部特性）。
带 --feature 时仅启用所列特性（依赖项由服务端 resolveDependencies 自动补全）。
"""
import argparse
import hashlib
import json
import sys

SALT = "aegisgate-v1-f7e2a9c4d1b8"
EDITION = "enterprise"
KNOWN_FEATURES = {
    "advanced_routing", "custom_rules", "web_management", "cluster_deployment",
    "rbac", "sso", "compliance_report", "alerting", "plugin_system",
    "agent_orchestration", "rag_pipeline",
}


def build_payload(customer: str, expires: str, features: list[str]) -> str:
    payload = f"{EDITION}:{customer}:{expires}"
    if features:
        payload += ":" + ",".join(sorted(features))
    return payload


def main() -> int:
    ap = argparse.ArgumentParser(description="生成 AegisGate 企业版测试 License")
    ap.add_argument("--customer", default="acceptance-test", help="客户名")
    ap.add_argument("--expires", default="2099-12-31", help="到期日 YYYY-MM-DD")
    ap.add_argument("--feature", action="append", default=[],
                    help="仅启用指定特性（可多次）。不传则启用全部特性")
    ap.add_argument("--prefix", default="AEGIS-ENT-", help="license_key 前缀")
    ap.add_argument("--out", default="-", help="输出文件路径，'-' 输出到 stdout")
    args = ap.parse_args()

    for f in args.feature:
        if f not in KNOWN_FEATURES:
            print(f"[warn] 未知特性 '{f}'（服务端会忽略）。已知: "
                  f"{sorted(KNOWN_FEATURES)}", file=sys.stderr)

    payload = build_payload(args.customer, args.expires, args.feature)
    checksum = hashlib.sha256((payload + SALT).encode()).hexdigest()[:16]
    license_key = args.prefix + checksum  # 末 16 位即 checksum

    doc = {
        "edition": EDITION,
        "customer": args.customer,
        "expires": args.expires,
        "license_key": license_key,
    }
    if args.feature:
        doc["features"] = args.feature

    text = json.dumps(doc, indent=2, ensure_ascii=False)
    if args.out == "-":
        print(text)
    else:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
        print(f"[ok] 已写入 {args.out}", file=sys.stderr)
        print(f"[ok] payload={payload}", file=sys.stderr)
        print("[next] 在 aegisgate.yaml 设置 edition: enterprise / "
              f"license_file: \"{args.out}\" / rbac.enabled: true，然后重启或热重载",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
