#!/usr/bin/env python3
"""Combine results/*.csv (one per job) into a single results.csv.

Rows are sorted by (job_id, trial) so the file is stable across runs. A short
summary of any degseq_ok=0 rows is printed to stderr as a correctness warning.
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIELDS = [
    "job_id", "comparison", "tool", "conversion", "threads", "trial",
    "graph", "n", "m", "wall_s", "peak_rss_kb", "out_bytes", "degseq_ok",
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-d", "--results-dir", default=str(HERE / "results"))
    ap.add_argument("-o", "--out", default=str(HERE / "results.csv"))
    args = ap.parse_args()

    rows = []
    for p in sorted(Path(args.results_dir).glob("*.csv")):
        with open(p, newline="") as f:
            rows.extend(csv.DictReader(f))

    def key(r):
        return (int(r["job_id"]), int(r["trial"]))

    rows.sort(key=key)

    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)

    failed = [r for r in rows if r.get("degseq_ok") == "0"]
    print(f"aggregated {len(rows)} rows from {args.results_dir} -> {args.out}",
          file=sys.stderr)
    if failed:
        print(f"WARNING: {len(failed)} row(s) failed the degree-sequence gate:",
              file=sys.stderr)
        for r in failed:
            print(f"  job {r['job_id']} {r['tool']} {r['conversion']} "
                  f"{r['graph']} trial {r['trial']}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
