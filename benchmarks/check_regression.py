#!/usr/bin/env python3
# Copyright (c) 2026 sqm
# SPDX-License-Identifier: MIT
"""Compare a benchmark run against a stored baseline and fail on regression.

This is the CI gate described in docs/adr/0033-performance-regression-gate.md. It
reads two JSON files produced by ``fork_bench --json``: the
recorded baseline (``benchmarks/baseline.json``) and the run from this build, and
exits non-zero when a metric regressed beyond the allowed tolerance.

Only the p50 (typical frame) is gated. p95/p99 are reported but not gated,
because a shared CI runner's worst frames are dominated by noise the engine does
not control; gating them would make the check flaky, and a flaky performance gate
gets disabled, which is worse than no gate.

Two knobs keep the gate honest but stable:

* ``--tolerance`` (default 25%) is the fractional slowdown allowed before a
  metric is called a regression. A real regression is larger than this; runner
  noise is not.
* ``--floor-ms`` (default 0.5 ms) suppresses the check when the baseline p50 is
  already tiny. Below a fraction of a millisecond, the measurement is dominated
  by timer resolution, so a percentage comparison is meaningless.

The engine is deterministic per scene, but the *timings* are not, so the gate is
deliberately coarse. Its job is to catch a 2x slowdown, not a 5% wobble.
"""

from __future__ import annotations

import argparse
import json
import sys

# Metrics that are gated. Each entry is (label, key). Everything else in the
# file is informational.
GATED_METRICS = [
    ("frame_total", "p50_ms"),
]


def load_metrics(path: str) -> dict:
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except FileNotFoundError:
        raise SystemExit(f"error: benchmark result not found: {path}")
    except json.JSONDecodeError as exc:
        raise SystemExit(f"error: {path} is not valid JSON: {exc}")

    metrics = data.get("metrics")
    if not isinstance(metrics, dict) or not metrics:
        raise SystemExit(f"error: {path} has no 'metrics' object")
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description="Fork Engine benchmark regression gate")
    parser.add_argument("--baseline", required=True, help="recorded baseline JSON")
    parser.add_argument("--current", required=True, help="benchmark run JSON to check")
    parser.add_argument(
        "--tolerance",
        type=float,
        default=0.25,
        help="fractional slowdown allowed before failing (default 0.25 = 25%%)",
    )
    parser.add_argument(
        "--floor-ms",
        type=float,
        default=0.5,
        help="skip the gate when the baseline is below this many ms (default 0.5)",
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="write the current run back to the baseline path instead of checking",
    )
    args = parser.parse_args()

    current = load_metrics(args.current)

    if args.update:
        with open(args.baseline, "w", encoding="utf-8") as handle:
            json.dump(current, handle, indent=2)
            handle.write("\n")
        print(f"baseline updated: {args.baseline}")
        return 0

    baseline = load_metrics(args.baseline)

    base_metrics = baseline["metrics"]
    cur_metrics = current["metrics"]

    print(f"scene: {current.get('scene', '?')}  baseline scene: {baseline.get('scene', '?')}")
    if baseline.get("scene") != current.get("scene"):
        print("warning: scene names differ; comparing anyway")

    failures = []
    print(f"{'metric':<16}{'baseline p50':>14}{'current p50':>14}{'delta':>12}   verdict")
    for label, key in GATED_METRICS:
        base_entry = base_metrics.get(label)
        cur_entry = cur_metrics.get(label)
        if base_entry is None or cur_entry is None:
            print(f"{label:<16}{'—':>14}{'—':>14}{'—':>12}   missing (skipped)")
            continue

        base_value = float(base_entry[key])
        cur_value = float(cur_entry[key])

        if base_value < args.floor_ms:
            verdict = f"below floor ({args.floor_ms} ms), skipped"
            delta_text = "n/a"
        else:
            ratio = (cur_value - base_value) / base_value
            delta_text = f"{ratio * 100:+.1f}%"
            if ratio > args.tolerance:
                verdict = f"REGRESSION (>{args.tolerance * 100:.0f}%)"
                failures.append(f"{label}: {base_value:.3f}ms -> {cur_value:.3f}ms ({delta_text})")
            else:
                verdict = "ok"

        print(f"{label:<16}{base_value:>13.3f}m{cur_value:>13.3f}m{delta_text:>12}   {verdict}")

    if failures:
        print()
        print("benchmark regression gate FAILED:")
        for line in failures:
            print(f"  - {line}")
        print()
        print("If this is an intentional change, re-record the baseline:")
        print(f"  {sys.argv[0]} --baseline {args.baseline} --current {args.current} --update")
        return 1

    print()
    print("benchmark regression gate passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
