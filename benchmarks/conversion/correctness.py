"""Relabeling-invariant correctness gate for the conversion benchmark.

The only property all tools must agree on, regardless of internal id ordering,
is the *sorted degree sequence* of the undirected simple graph. Two conversions
of the same input that produce the same sorted degree sequence are treated as
equivalent for the purposes of this benchmark (a full isomorphism check is
neither necessary nor cheap; the degree sequence catches dropped/duplicated
edges, self-loop mishandling, and missing reverse edges).

Each `degseq_from_*` returns an *unsorted* list of per-node degrees; `degseq_hash`
sorts and hashes, so the hash is invariant to node relabeling.
"""

from __future__ import annotations

import hashlib
import struct
from pathlib import Path


def degseq_hash(degrees: list[int]) -> str:
    """SHA-256 of the ascending-sorted *non-zero* degree sequence.

    Zero-degree (isolated) vertices are dropped before hashing because edge-list
    formats cannot represent them: a metis/CSR side encodes isolated vertices but
    an edge-list side silently omits them. Comparing only non-zero degrees makes
    the gate consistent across all three formats while still catching every
    edge-level discrepancy (any real degree change is preserved). The hash
    remains invariant to node relabeling.
    """
    nz = sorted(d for d in degrees if d > 0)
    buf = struct.pack(f"<{len(nz)}Q", *nz)
    return hashlib.sha256(buf).hexdigest()


def degseq_from_csr(indptr: list[int]) -> list[int]:
    """Degrees from a CSR indptr array: degree[u] = indptr[u+1] - indptr[u]."""
    return [indptr[i + 1] - indptr[i] for i in range(len(indptr) - 1)]


def degseq_from_metis(path: str | Path) -> list[int]:
    """Degrees from a METIS adjacency file.

    Header line: `<n> <m> [fmt]`. Each subsequent non-comment line lists a
    vertex's neighbors (1-indexed); its degree is the neighbor count. Blank
    lines denote isolated vertices (degree 0).
    """
    degs: list[int] = []
    with open(path, "r") as f:
        header_seen = False
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("%"):
                if header_seen:
                    degs.append(0)  # blank body line = isolated vertex
                continue
            if not header_seen:
                header_seen = True
                continue
            degs.append(len(line.split()))
    return degs


def degseq_from_edgelist(path: str | Path, sep: str = ",", skip_rows: int = 0) -> list[int]:
    """Degrees from an undirected edge-list CSV (one `u<sep>v` per line).

    Builds an adjacency set per node so duplicate edges and self-loops do not
    inflate the degree. Node ids are taken as-is (relabeling-invariant anyway).
    """
    adj: dict[int, set[int]] = {}
    sep = "\t" if sep == "\t" else sep
    with open(path, "r") as f:
        for _ in range(skip_rows):
            next(f, None)
        for raw in f:
            line = raw.strip()
            if not line or line[0] in "#%":
                continue
            parts = line.split(sep) if sep != " " else line.split()
            if len(parts) < 2:
                continue
            u, v = int(parts[0]), int(parts[1])
            if u == v:
                adj.setdefault(u, set())
                continue
            adj.setdefault(u, set()).add(v)
            adj.setdefault(v, set()).add(u)
    return [len(nees) for nees in adj.values()]


def read_indptr_parquet(path: str | Path) -> list[int]:
    """Read a single-column indptr Parquet file by position (column 0).

    Reading by position rather than by name tolerates differing column names
    across tools (we write "indptr"; another tool may name it otherwise).
    """
    import pyarrow.parquet as pq

    return pq.read_table(str(path)).column(0).to_pylist()


def degseq_from_output(out_format: str, out_paths: list[str], sep: str = ",") -> list[int]:
    """Dispatch to the right degree-sequence extractor for a tool's output."""
    if out_format == "metis":
        return degseq_from_metis(out_paths[0])
    if out_format == "csv":
        return degseq_from_edgelist(out_paths[0], sep=sep, skip_rows=0)
    if out_format == "csr":
        # Accept both our `<prefix>.indptr.parquet` and icebug's
        # `indptr_<name>.parquet` by matching "indptr" in the basename.
        indptr_path = next(
            p for p in out_paths if "indptr" in Path(p).name
        )
        return degseq_from_csr(read_indptr_parquet(indptr_path))
    raise ValueError(f"unknown out_format: {out_format!r}")
