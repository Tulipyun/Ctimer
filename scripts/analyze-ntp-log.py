"""Summarize Ctimer v0.6 raw observations; this does not measure true UTC error."""
import argparse
import csv
import json
import math
from collections import Counter
from pathlib import Path
from statistics import median


def summarize(directory):
    with (directory / "ntp-samples-v2.csv").open(encoding="utf-8", newline="") as stream:
        samples = list(csv.DictReader(stream))
    with (directory / "clock-models-v1.csv").open(encoding="utf-8", newline="") as stream:
        models = list(csv.DictReader(stream))
    sources = []
    for host in dict.fromkeys(row["source"] for row in samples):
        rows = [row for row in samples if row["source"] == host]
        good = [row for row in rows if row["stored"] == "1"]
        rtts = sorted(float(row["rtt_ms"]) for row in good)
        sources.append({
            "host": host, "group": rows[0]["group"],
            "queries_sent": sum(row["sent"] == "1" for row in rows),
            "valid_stored": len(good),
            "deferred": sum(row["deferred"] == "1" for row in rows),
            "addresses": sorted({row["ip"] for row in rows if row["ip"]}),
            "rtt_min_ms": min(rtts) if rtts else None,
            "rtt_median_ms": median(rtts) if rtts else None,
            "rtt_p95_ms": rtts[math.ceil(.95 * len(rtts)) - 1] if rtts else None,
            "statuses": dict(Counter(row["status"] for row in rows)),
        })
    accepted = {}
    for row in models:
        if row["model_version"] != "0" and row["failed"] == "0":
            accepted[(row["session"], row["model_version"])] = row
    updates = list(accepted.values())
    latest = updates[-1] if updates else {}
    report = {
        "metric": "NTP reachability, RTT and internal model diagnostics; no independent UTC reference",
        "sessions": sorted({row["session"] for row in samples}),
        "queries_sent": sum(row["sent"] == "1" for row in samples),
        "stored_samples": sum(row["stored"] == "1" for row in samples),
        "deferred_queries": sum(row["deferred"] == "1" for row in samples),
        "accepted_models": len(updates),
        "primary_counts": dict(Counter(row["primary"] for row in updates)),
        "latest_model": latest,
        "sources": sources,
    }
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = summarize(args.directory)
    output = args.output or args.directory / "summary.json"
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(output)
