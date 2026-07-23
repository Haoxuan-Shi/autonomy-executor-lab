"""Run one workload under every policy and print a compact comparison table."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys


POLICIES = ("fifo", "fixed-priority", "edf", "latest-only")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument("workload", type=Path)
    args = parser.parse_args()
    rows = []
    for policy in POLICIES:
        completed = subprocess.run(
            [str(args.executable), str(args.workload), "--policy", policy],
            check=False,
            text=True,
            capture_output=True,
            timeout=30,
        )
        if completed.returncode != 0:
            print(completed.stderr, file=sys.stderr, end="")
            return completed.returncode
        report = json.loads(completed.stdout)
        rows.append(report)
    print("policy,completed,deadline_misses,stale_rejections,superseded,max_response_ms,max_age_ms")
    for row in rows:
        print(
            ",".join(
                str(row[key])
                for key in (
                    "policy",
                    "completed",
                    "deadline_misses",
                    "stale_rejections",
                    "superseded",
                    "maximum_response_ms",
                    "maximum_age_ms",
                )
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

