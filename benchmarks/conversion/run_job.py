#!/usr/bin/env python3
"""Run one benchmark job (two trials) and write results/<N>.csv.

Two modes:

  --job-line N      Orchestrator. Reads data row N (1-based) of jobs.csv, runs
                    trial 1 (warmup) then trial 2 (measure), each in a fresh
                    worker subprocess, and writes results/<N>.csv (two rows).

  --worker ...      Internal. Executes a single conversion in isolation, times
                    it tightly, gates the output against format_conv's reference
                    degree sequence, and prints a JSON result line on stdout.

results schema (two rows per job; trial=1 warmup, trial=2 measure):
    job_id,comparison,tool,conversion,threads,trial,graph,n,m,
    wall_s,peak_rss_kb,out_bytes,degseq_ok

Running a worker in its own process gives each trial a clean page cache / heap
(true warmup-then-measure) and lets /usr/bin/time -v capture per-trial peak RSS.
"""

from __future__ import annotations

import argparse
import csv
import json
import resource
import shutil
import sys
import time
import tomllib
from pathlib import Path

import metrics

HERE = Path(__file__).resolve().parent
RESULTS_FIELDS = [
    "job_id", "comparison", "tool", "conversion", "threads", "trial",
    "graph", "n", "m", "wall_s", "peak_rss_kb", "out_bytes", "degseq_ok",
]

_CONVERSIONS = {
    "csv->metis": ("csv", "metis"),
    "metis->csv": ("metis", "csv"),
    "csv->csr": ("csv", "csr"),
}


def _resolve(p: str) -> str:
    """Resolve a config-relative path against this directory."""
    if not p:
        return ""
    q = Path(p)
    return str(q if q.is_absolute() else (HERE / q).resolve())


def load_config() -> dict:
    with open(HERE / "config.toml", "rb") as f:
        return tomllib.load(f)


def read_job(job_line: int) -> dict:
    with open(HERE / "jobs.csv", newline="") as f:
        rows = list(csv.DictReader(f))
    if job_line < 1 or job_line > len(rows):
        raise SystemExit(f"job_line {job_line} out of range 1..{len(rows)}")
    return rows[job_line - 1]


# ── worker ────────────────────────────────────────────────────────────────────

def _import_adapter(tool: str):
    if tool == "format_conv":
        from adapters import format_conv as mod
    elif tool == "networkit":
        from adapters import networkit as mod
    elif tool == "icebug":
        from adapters import icebug as mod
    else:
        raise SystemExit(f"unknown tool {tool!r}")
    return mod


def _prepare_inputs(job: dict, csv_path: str, csv_sep: str, csv_skip: int,
                    nodes_path, workdir: Path) -> dict:
    """Build the shared inputs the networkit comparison's two tools both read.

    NetworKit's EdgeListReader has no header-skip option and mis-parses a
    header row as a phantom edge (see adapters/networkit.py docstring), and the
    source CSV may have gapped ids. So for the networkit comparison, both
    format_conv and networkit read the SAME prepared, headerless, compacted CSV
    (comma-separated, 0-indexed) and the SAME prepared METIS file, built once
    here via format_conv from the real source — untimed setup, identical input
    for both tools, no bias toward either.

    Returns {"csv": <path>, "metis": <path>}.
    """
    from adapters import format_conv as fc

    base = {
        "src_path": csv_path, "src_format": "csv",
        "threads": int(job["threads"]), "nodes_path": nodes_path,
        "skip_rows": csv_skip, "sep": csv_sep, "flags": {},
    }
    prep_csv = workdir / "prepared_csv"
    fc.convert({**base, "conversion": "csv->csv", "dst_format": "csv",
                "out_prefix": str(prep_csv)})
    prep_metis = workdir / "prepared_metis"
    fc.convert({**base, "conversion": "csv->metis", "dst_format": "metis",
                "out_prefix": str(prep_metis)})
    return {"csv": str(prep_csv) + ".csv", "metis": str(prep_metis) + ".metis"}


def run_worker(job: dict, trial: int, workdir: Path, check_degseq: bool, flags: dict) -> dict:
    src_format, dst_format = _CONVERSIONS[job["conversion"]]
    csv_path = _resolve(job["graph_path"])
    nodes_path = _resolve(job["graph_nodes"]) or None
    csv_skip = int(job["graph_skip_rows"])
    csv_sep = ","  # the source edge CSV is comma-separated

    workdir.mkdir(parents=True, exist_ok=True)

    if job["comparison"] == "networkit":
        prepared = _prepare_inputs(job, csv_path, csv_sep, csv_skip, nodes_path, workdir)
        src_path = prepared[src_format]
        # The prepared CSV is already headerless and compacted: no skip_rows,
        # no node list needed for either tool.
        spec_skip_rows = 0
        spec_nodes_path = None
    else:
        # icebug's csv->csr: both tools read the real source CSV directly
        # (format_conv via skip_rows + a node list; icebug via its own
        # header-aware DuckDB ingest).
        src_path = csv_path
        spec_skip_rows = csv_skip
        spec_nodes_path = nodes_path

    spec = {
        "conversion": job["conversion"],
        "src_path": src_path,
        "src_format": src_format,
        "dst_format": dst_format,
        "out_prefix": str(workdir / "out"),
        "threads": int(job["threads"]),
        "nodes_path": spec_nodes_path,
        "skip_rows": spec_skip_rows,
        "sep": csv_sep,
        "flags": flags,
        "graph_name": job["graph"],
    }

    # Canonical reference (always from the CSV via format_conv), built before the
    # timed region so it never counts against the tool under test.
    ref_hash = None
    n = m = -1
    if check_degseq:
        from adapters import format_conv as fc

        ref_hash, n, m = fc.reference_degseq(
            {
                "csv_path": csv_path,
                "nodes_path": nodes_path,
                "csv_skip_rows": csv_skip,
                "csv_sep": csv_sep,
                "threads": int(job["threads"]),
            },
            str(workdir / "ref"),
        )

    adapter = _import_adapter(job["tool"])

    t0 = time.perf_counter()
    res = adapter.convert(spec)
    wall_s = time.perf_counter() - t0

    import correctness

    degs = correctness.degseq_from_output(res["out_format"], res["out_paths"], sep=csv_sep)
    if n < 0:
        n, m = len(degs), sum(degs) // 2
    degseq_ok = True if ref_hash is None else (correctness.degseq_hash(degs) == ref_hash)

    self_rss_kb = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss

    return {
        "trial": trial,
        "n": n,
        "m": m,
        "wall_s": round(wall_s, 6),
        "self_rss_kb": self_rss_kb,
        "out_bytes": metrics.out_bytes(res["out_paths"]),
        "degseq_ok": bool(degseq_ok),
    }


# ── orchestrator ───────────────────────────────────────────────────────────────

def run_job(job_line: int) -> Path:
    cfg = load_config()
    check_degseq = bool(cfg["run"].get("check_degseq", True))
    job = read_job(job_line)
    tool_flags = cfg.get("tools", {}).get(job["tool"], {})

    rows = []
    for trial in (1, 2):
        workdir = HERE / "work" / f"job{job_line}_trial{trial}"
        if workdir.exists():
            shutil.rmtree(workdir)
        cmd = [
            sys.executable, str(HERE / "run_job.py"), "--worker",
            "--job-line", str(job_line),
            "--trial", str(trial),
            "--workdir", str(workdir),
            "--check-degseq", "1" if check_degseq else "0",
            "--flags", json.dumps(tool_flags),
        ]
        meas = metrics.run_measured(cmd, cwd=HERE)
        if meas["returncode"] != 0:
            sys.stderr.write(meas["stdout"] + "\n" + meas["stderr"] + "\n")
            raise SystemExit(f"worker failed (job {job_line}, trial {trial})")

        wres = json.loads(meas["stdout"].strip().splitlines()[-1])
        peak = meas["peak_rss_kb"] if meas["peak_rss_kb"] is not None else wres["self_rss_kb"]
        rows.append({
            "job_id": job["job_id"],
            "comparison": job["comparison"],
            "tool": job["tool"],
            "conversion": job["conversion"],
            "threads": job["threads"],
            "trial": trial,
            "graph": job["graph"],
            "n": wres["n"],
            "m": wres["m"],
            "wall_s": wres["wall_s"],
            "peak_rss_kb": peak,
            "out_bytes": wres["out_bytes"],
            "degseq_ok": int(wres["degseq_ok"]),
        })

    (HERE / "results").mkdir(exist_ok=True)
    out = HERE / "results" / f"{job_line}.csv"
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=RESULTS_FIELDS)
        w.writeheader()
        w.writerows(rows)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--job-line", type=int, help="1-based data row of jobs.csv")
    ap.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--trial", type=int)
    ap.add_argument("--workdir")
    ap.add_argument("--check-degseq")
    ap.add_argument("--flags", default="{}")
    args = ap.parse_args()

    if args.worker:
        job = read_job(args.job_line)
        res = run_worker(
            job,
            trial=args.trial,
            workdir=Path(args.workdir),
            check_degseq=(args.check_degseq == "1"),
            flags=json.loads(args.flags),
        )
        print(json.dumps(res))
        return 0

    if args.job_line is None:
        ap.error("--job-line is required (or --worker for internal use)")
    out = run_job(args.job_line)
    print(f"wrote {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
