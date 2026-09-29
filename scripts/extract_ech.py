#!/usr/bin/env python3
"""从 DoH 的 DNS JSON 应答里取出 ECH 配置与 IPv4 起点地址。

与 ech_http 的 DohEchResolver 同解析规则（https 记录 data 串里的 ech="? 与
ipv4hint=），用于 CI 现场取「活值」——不把 ECH 配置写死进仓库。

用法: curl ... | python3 scripts/extract_ech.py
输出: 可直接 eval 的 KEY=VALUE 若干行
"""
import json
import re
import sys

TYPE_HTTPS = 65


def parse(payload: dict) -> dict:
    if payload.get("Status") != 0:
        raise SystemExit(f"DNS 应答 Status={payload.get('Status')}，非成功")
    answers = payload.get("Answer") or []
    for record in answers:
        if record.get("type") != TYPE_HTTPS:
            continue
        data = record.get("data")
        if not isinstance(data, str):
            continue
        ech = re.search(r'(?:^|\s)ech="?([A-Za-z0-9+/=]+)', data)
        if not ech:
            continue
        result = {"ECH_CONFIG": ech.group(1), "TTL": str(record.get("TTL", 0))}
        hint = re.search(r'(?:^|\s)ipv4hint="?([0-9.,]+)', data)
        if hint:
            result["IPV4_HINT"] = hint.group(1).split(",")[0]
        return result
    raise SystemExit("HTTPS 记录里没有 ech= 参数")


def main() -> int:
    raw = sys.stdin.read()
    if not raw.strip():
        raise SystemExit("标准输入为空")
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise SystemExit(f"应答不是合法 JSON: {exc}") from exc
    for key, value in parse(payload).items():
        print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
