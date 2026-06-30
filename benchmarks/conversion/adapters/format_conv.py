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

from format_conversion.format import (
    EdgesFormat,
    ParseOptions,
    NodeDescriptor,
    GraphDescriptor,
    convert as _convert,
)

import correctness

_SRC_FMT = {"csv": EdgesFormat.CSV_EDGELIST, "metis": EdgesFormat.METIS}
_DST_FMT = {
    "metis": EdgesFormat.METIS,
    "csv": EdgesFormat.CSV_EDGELIST,
    "csr": EdgesFormat.CSR_PARQUET,
}


def _opts(spec: dict) -> ParseOptions:
    o = ParseOptions()
    o.num_threads = int(spec["threads"])
    o.sep = spec.get("sep", ",")
    if spec["src_format"] == "csv":
        o.skip_rows = int(spec.get("skip_rows", 0))
    if spec["dst_format"] == "csr" and spec.get("flags", {}).get(
        "use_u64_indices_for_csr", False
    ):
        o.use_u64_indices = True
    return o


def _out_paths(out_prefix: str, dst_format: str) -> list[str]:
    if dst_format == "metis":
        return [out_prefix + ".metis"]
    if dst_format == "csv":
        return [out_prefix + ".csv"]
    if dst_format == "csr":
        return [out_prefix + ".indices.parquet", out_prefix + ".indptr.parquet"]
    raise ValueError(dst_format)


def convert(spec: dict) -> dict:
    o = _opts(spec)
    gd = GraphDescriptor(str(spec["src_path"]), _SRC_FMT[spec["src_format"]], o)
    # A node list applies only when reading CSV (it remaps raw ids → compact ids).
    nd = None
    if spec["src_format"] == "csv" and spec.get("nodes_path"):
        nd = NodeDescriptor(str(spec["nodes_path"]), o)
    _convert(gd, nd, spec["out_prefix"], _DST_FMT[spec["dst_format"]])
    return {"out_paths": _out_paths(spec["out_prefix"], spec["dst_format"]),
            "out_format": spec["dst_format"]}


def reference_degseq(spec: dict, work_prefix: str) -> tuple[str, int, int]:
    """Build the canonical (hash, n, m) for the graph from its original CSV.

    Always uses the CSV source + node list, independent of the conversion being
    benchmarked, so the reference is identical for every tool in a comparison.
    Returns the relabeling-invariant degree-sequence hash, node count, and
    undirected edge count.
    """
    o = ParseOptions()
    o.num_threads = int(spec["threads"])
    o.sep = spec.get("csv_sep", spec.get("sep", ","))
    o.skip_rows = int(spec.get("csv_skip_rows", 0))
    gd = GraphDescriptor(str(spec["csv_path"]), EdgesFormat.CSV_EDGELIST, o)
    nd = NodeDescriptor(str(spec["nodes_path"]), o) if spec.get("nodes_path") else None
    _convert(gd, nd, work_prefix, EdgesFormat.CSR_PARQUET)

    indptr = correctness.read_indptr_parquet(work_prefix + ".indptr.parquet")
    degs = correctness.degseq_from_csr(indptr)
    n = len(degs)
    m = sum(degs) // 2
    return correctness.degseq_hash(degs), n, m
