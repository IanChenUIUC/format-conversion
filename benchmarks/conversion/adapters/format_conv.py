"""format-conversion adapter — also the reference oracle for correctness.

`convert(spec)` runs one conversion through the pybind `format` module.
`reference_degseq(spec)` builds the canonical degree-sequence hash for a graph
(from the original CSV via a CSR build), which every other tool is checked
against.

spec is a dict with keys:
    conversion   "csv->metis" | "metis->csv" | "csv->csr"
    src_path     path to the source file (in the format named by src_format)
    src_format   "csv" | "metis"
    dst_format   "metis" | "csv" | "csr"
    out_prefix   output path prefix (suffixes added per format)
    threads      worker thread count
    nodes_path   node list for id remapping, or None
    skip_rows    header rows to skip in CSV inputs (0 for the prepared,
                 already-headerless CSV; >0 for the real source CSV)
    sep          field separator for CSV inputs
    flags        tool-specific flags block (dict)
"""

from __future__ import annotations

import format_conversion.format as fmt
from format_conversion.format import (
    NodeDescriptor,
    GraphDescriptor,
    convert as _convert,
)

import correctness


def _read_spec(spec: dict):
    """Read spec for the source format."""
    if spec["src_format"] == "csv":
        return fmt.CsvEdgelist.Read(
            sep=spec.get("sep", ","), skip_rows=int(spec.get("skip_rows", 0))
        )
    if spec["src_format"] == "metis":
        return fmt.Metis.Read()
    raise ValueError(spec["src_format"])


def _write_spec(spec: dict):
    """Write spec for the destination format. u64 indices are a per-run flag."""
    dst = spec["dst_format"]
    if dst == "metis":
        return fmt.Metis.Write()
    if dst == "csv":
        return fmt.CsvEdgelist.Write(sep=spec.get("sep", ","))
    if dst == "csr":
        return fmt.CsrParquet.Write(
            u64_indices=bool(spec.get("flags", {}).get("use_u64_indices_for_csr", False))
        )
    raise ValueError(dst)


def _out_paths(out_prefix: str, dst_format: str) -> list[str]:
    if dst_format == "metis":
        return [out_prefix + ".metis"]
    if dst_format == "csv":
        return [out_prefix + ".csv"]
    if dst_format == "csr":
        return [out_prefix + ".indices.parquet", out_prefix + ".indptr.parquet"]
    raise ValueError(dst_format)


def convert(spec: dict) -> dict:
    gd = GraphDescriptor(str(spec["src_path"]), _read_spec(spec))
    out = GraphDescriptor(str(spec["out_prefix"]), _write_spec(spec))
    # A node list applies only when reading CSV (it remaps raw ids → compact ids).
    nd = None
    if spec["src_format"] == "csv" and spec.get("nodes_path"):
        nd = NodeDescriptor(
            str(spec["nodes_path"]),
            fmt.Nodelist.Csv(skip_rows=int(spec.get("skip_rows", 0))),
        )
    _convert(gd, out, nodes=nd, num_threads=int(spec["threads"]))
    return {"out_paths": _out_paths(spec["out_prefix"], spec["dst_format"]),
            "out_format": spec["dst_format"]}


def reference_degseq(spec: dict, work_prefix: str) -> tuple[str, int, int]:
    """Build the canonical (hash, n, m) for the graph from its original CSV.

    Always uses the CSV source + node list, independent of the conversion being
    benchmarked, so the reference is identical for every tool in a comparison.
    Returns the relabeling-invariant degree-sequence hash, node count, and
    undirected edge count.
    """
    skip = int(spec.get("csv_skip_rows", 0))
    gd = GraphDescriptor(
        str(spec["csv_path"]),
        fmt.CsvEdgelist.Read(sep=spec.get("csv_sep", spec.get("sep", ",")), skip_rows=skip),
    )
    nd = (NodeDescriptor(str(spec["nodes_path"]), fmt.Nodelist.Csv(skip_rows=skip))
          if spec.get("nodes_path") else None)
    _convert(gd, GraphDescriptor(str(work_prefix), fmt.CsrParquet.Write()),
             nodes=nd, num_threads=int(spec["threads"]))

    indptr = correctness.read_indptr_parquet(work_prefix + ".indptr.parquet")
    degs = correctness.degseq_from_csr(indptr)
    n = len(degs)
    m = sum(degs) // 2
    return correctness.degseq_hash(degs), n, m
