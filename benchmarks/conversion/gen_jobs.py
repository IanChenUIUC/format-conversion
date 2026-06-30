#!/usr/bin/env python3
"""Expand config.toml into jobs.csv — one row per SLURM array task.

Each job is a single (comparison, tool, conversion, threads, graph) point.
run_job.py runs two trials (warmup + measure) per job, so a job is the unit of
SLURM array parallelism.

jobs.csv schema (matches the handoff):
    job_id,comparison,tool,conversion,threads,graph,graph_path,
    graph_format,graph_nodes,graph_skip_rows

Filters (--comparison/--tool/--graph) subset the matrix, which is how you run a
format_conv-only smoke test locally before networkit/icebug are installed:

    python gen_jobs.py --tool format_conv -o jobs.csv
"""

from __future__ import annotations

import argparse
import csv
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIELDS = [
    "job_id",
    "comparison",
    "tool",
    "conversion",
    "threads",
    "graph",
    "graph_path",
    "graph_format",
    "graph_nodes",
    "graph_skip_rows",
]


def load_config(path: Path) -> dict:
    with open(path, "rb") as f:
        return tomllib.load(f)


def expand(cfg: dict, only_comparison=None, only_tool=None, only_graph=None) -> list[dict]:
    threads = cfg["run"]["threads"]
    graphs = cfg["graphs"]
    rows: list[dict] = []
    jid = 0
    for cname, comp in cfg["comparisons"].items():
        if only_comparison and cname != only_comparison:
            continue
        for tool in comp["tools"]:
            if only_tool and tool != only_tool:
                continue
            for conversion in comp["conversions"]:
                for t in threads:
                    for g in graphs:
                        if only_graph and g["name"] != only_graph:
                            continue
                        jid += 1
                        rows.append(
                            {
                                "job_id": jid,
                                "comparison": cname,
                                "tool": tool,
                                "conversion": conversion,
                                "threads": t,
                                "graph": g["name"],
                                "graph_path": g["path"],
                                "graph_format": g["format"],
                                "graph_nodes": g.get("nodes", ""),
                                "graph_skip_rows": g.get("skip_rows", 0),
                            }
                        )
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("-c", "--config", default=str(HERE / "config.toml"))
    ap.add_argument("-o", "--out", default=str(HERE / "jobs.csv"))
    ap.add_argument("--comparison", help="only this comparison")
    ap.add_argument("--tool", help="only this tool")
    ap.add_argument("--graph", help="only this graph")
    args = ap.parse_args()

    cfg = load_config(Path(args.config))
    rows = expand(cfg, args.comparison, args.tool, args.graph)

    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)

    n = len(rows)
    print(f"wrote {n} jobs to {args.out}", file=sys.stderr)
    if n:
        print("# SLURM array invocation:", file=sys.stderr)
        print(f"sbatch --array=1-{n} {HERE / 'slurm_job.sh'}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
