"""icebug adapter (lazy import + subprocess).

icebug pipeline for csv->csr (verified against icebug-format==1.0.0):

    1. Ingest the edge list (and node list) into a source DuckDB database as
       tables `edges_<name>` / `nodes_<name>` (icebug auto-discovers tables by
       the `edges`/`nodes` prefix).
    2. Run the standalone `icebug-format` CLI:
           icebug-format --source-db src.duckdb --output-db out.duckdb
                         [--add-reverse-edges]
       which builds the CSR into out.duckdb AND exports Parquet to a directory
       named after the output db (out.duckdb -> ./out/):
           out/indices_<name>.parquet   (column "target", uint64)
           out/indptr_<name>.parquet    (column "ptr",    uint64)
       --add-reverse-edges makes the adjacency symmetric, matching our CSR.

icebug-format is a separate package (PyPI: icebug-format==1.0.0) that does not
shadow networkit. duckdb and the CLI are only touched inside convert(), so the
harness imports fine without them.

Note on threads: icebug-format spawns its own DuckDB and exposes no thread knob
(only --memory-limit), so the `threads` level is not directly controllable for
this tool; it is recorded in results for provenance but does not bind icebug.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


def _cli() -> str:
    """Locate the icebug-format entry point.

    Prefer PATH, but fall back to the directory of the running interpreter
    (e.g. .venv/bin/icebug-format), which is where pip/uv install console
    scripts even when that dir is not on PATH in the worker's environment.
    """
    found = shutil.which("icebug-format")
    if found:
        return found
    cand = Path(sys.executable).parent / "icebug-format"
    if cand.exists():
        return str(cand)
    return "icebug-format"  # let subprocess raise a clear error


def _ingest_duckdb(db_path: str, name: str, spec: dict) -> None:
    import duckdb

    con = duckdb.connect(db_path)
    try:
        con.execute(f"PRAGMA threads={int(spec['threads'])}")
        # Override column names + types; with header=true the header row is
        # skipped regardless of its actual names, so this is header-agnostic.
        con.execute(
            f"""
            CREATE OR REPLACE TABLE edges_{name} AS
            SELECT source, target
            FROM read_csv(?, header=true,
                          columns={{'source':'BIGINT','target':'BIGINT'}})
            """,
            [str(spec["src_path"])],
        )
        if spec.get("nodes_path"):
            con.execute(
                f"""
                CREATE OR REPLACE TABLE nodes_{name} AS
                SELECT node_id
                FROM read_csv(?, header=true, columns={{'node_id':'BIGINT'}})
                """,
                [str(spec["nodes_path"])],
            )
    finally:
        con.close()


def convert(spec: dict) -> dict:
    if spec["dst_format"] != "csr":
        raise ValueError(f"icebug comparison only covers csv->csr, got {spec['conversion']!r}")

    name = spec["graph_name"]
    out_dir = Path(spec["out_prefix"]).parent
    out_dir.mkdir(parents=True, exist_ok=True)
    src_db = str(out_dir / f"{name}_src.duckdb")
    out_db = str(out_dir / f"{name}_out.duckdb")
    # icebug exports Parquet to the output-db path minus the .duckdb extension.
    parquet_dir = Path(out_db[: -len(".duckdb")])

    _ingest_duckdb(src_db, name, spec)

    flags = spec.get("flags", {})
    cmd = [_cli(), "--source-db", src_db, "--output-db", out_db]
    if flags.get("add_reverse_edges", False):
        cmd.append("--add-reverse-edges")
    subprocess.run(cmd, cwd=str(out_dir), check=True, capture_output=True, text=True)

    indices = str(parquet_dir / f"indices_{name}.parquet")
    indptr = str(parquet_dir / f"indptr_{name}.parquet")
    if not (os.path.exists(indices) and os.path.exists(indptr)):
        raise FileNotFoundError(
            f"icebug did not produce expected Parquet in {parquet_dir} "
            f"(have: {list(parquet_dir.glob('*')) if parquet_dir.exists() else 'no dir'})"
        )
    return {"out_paths": [indices, indptr], "out_format": "csr"}
