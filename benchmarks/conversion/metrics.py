"""Measurement helpers: wall time, peak RSS, output size.

On a cluster, `/usr/bin/time -v` is the canonical peak-RSS source. It is not
always installed (e.g. minimal containers), so `run_measured` uses it when
available and otherwise returns peak_rss_kb=None; callers then fall back to the
worker's own `resource.getrusage(RUSAGE_SELF).ru_maxrss` self-report, which is
just as accurate for a single isolated process.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import time
from pathlib import Path

_TIME_V_FIELD = "Maximum resident set size (kbytes):"


def _gnu_time() -> str | None:
    """Path to a GNU time binary that supports `-v`, or None."""
    for cand in ("/usr/bin/time", shutil.which("time")):
        if cand and os.path.exists(cand):
            return cand
    return None


def parse_time_v(stderr: str) -> dict[str, str]:
    """Parse the `key: value` lines emitted by `/usr/bin/time -v`."""
    out: dict[str, str] = {}
    for line in stderr.splitlines():
        line = line.strip()
        if ": " in line:
            k, _, v = line.partition(": ")
            out[k.strip()] = v.strip()
    return out


def peak_rss_kb_from_time_v(stderr: str) -> int | None:
    for line in stderr.splitlines():
        s = line.strip()
        if s.startswith(_TIME_V_FIELD):
            try:
                return int(s.split(":")[-1].strip())
            except ValueError:
                return None
    return None


def run_measured(cmd: list[str], cwd: str | Path | None = None) -> dict:
    """Run `cmd`, returning wall_s, peak_rss_kb (or None), stdout, stderr, rc.

    wall_s is wall-clock around the subprocess via perf_counter. peak_rss_kb is
    parsed from `/usr/bin/time -v` when that binary is present, else None.
    """
    gtime = _gnu_time()
    full = [gtime, "-v", *cmd] if gtime else list(cmd)

    t0 = time.perf_counter()
    proc = subprocess.run(full, cwd=cwd, capture_output=True, text=True)
    wall_s = time.perf_counter() - t0

    peak = peak_rss_kb_from_time_v(proc.stderr) if gtime else None
    return {
        "wall_s": wall_s,
        "peak_rss_kb": peak,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "returncode": proc.returncode,
    }


def out_bytes(paths: list[str]) -> int:
    """Total on-disk size of a tool's output files."""
    return sum(os.path.getsize(p) for p in paths if os.path.exists(p))
