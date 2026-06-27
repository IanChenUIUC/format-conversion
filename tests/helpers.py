"""
Utilities for writing graph fixtures and reading back graph files in various formats.
All edge sets are returned as frozenset of (min, max) pairs (undirected, 0-indexed).
"""

from __future__ import annotations
from pathlib import Path


# ── Writers ──────────────────────────────────────────────────────────────────

def write_edgelist(path: Path, edges: list[tuple[int, int]],
                   header: bool = True, sep: str = ",") -> None:
    with open(path, "w") as f:
        if header:
            f.write(f"src{sep}dst\n")
        for u, v in edges:
            f.write(f"{u}{sep}{v}\n")


def write_nodelist(path: Path, nodes: list[int],
                   node_attrs: list[list[str]] | None = None,
                   attr_header: list[str] | None = None,
                   header: bool = True) -> None:
    with open(path, "w") as f:
        if header:
            extra = ("," + ",".join(attr_header)) if attr_header else ""
            f.write(f"node_id{extra}\n")
        for i, n in enumerate(nodes):
            extra = ("," + ",".join(node_attrs[i])) if node_attrs else ""
            f.write(f"{n}{extra}\n")


def write_labels(path: Path, labels: list[int]) -> None:
    """One label per line, ordered by compact node ID (i.e. position in nodelist)."""
    with open(path, "w") as f:
        for label in labels:
            f.write(f"{label}\n")


# ── Readers ───────────────────────────────────────────────────────────────────

def _edge(u: int, v: int) -> tuple[int, int]:
    return (min(u, v), max(u, v))


def read_edgelist(path: Path, sep: str = ",", header: bool = True) -> frozenset:
    edges: set[tuple[int, int]] = set()
    with open(path) as f:
        if header:
            next(f)
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(sep)
            edges.add(_edge(int(parts[0]), int(parts[1])))
    return frozenset(edges)


def read_metis(path: Path) -> frozenset:
    """
    Read a METIS adjacency-list file.
    Returns undirected edges as (u, v) with u < v, 0-indexed.
    METIS format: first line is "N M", then N lines of 1-indexed space-separated neighbors.
    """
    edges: set[tuple[int, int]] = set()
    with open(path) as f:
        header = f.readline().split()
        n, m = int(header[0]), int(header[1])
        for u, line in enumerate(f):
            for tok in line.split():
                v = int(tok) - 1  # METIS is 1-indexed
                if u < v:
                    edges.add((u, v))
    return frozenset(edges)


def read_csr_parquet(base_path: Path) -> frozenset:
    """
    Read the two-file CSR Parquet representation produced by writeGraph.
    base_path should not include the .indices.parquet / .indptr.parquet suffix.
    """
    import pyarrow.parquet as pq
    indices = pq.read_table(str(base_path) + ".indices.parquet")["indices"].to_pylist()
    indptr  = pq.read_table(str(base_path) + ".indptr.parquet")["indptr"].to_pylist()
    edges: set[tuple[int, int]] = set()
    for u in range(len(indptr) - 1):
        for i in range(indptr[u], indptr[u + 1]):
            v = indices[i]
            if u < v:
                edges.add((u, v))
    return frozenset(edges)


def read_nodelist(path: Path, header: bool = True) -> list[int]:
    """Read the first column of a nodelist CSV as a list of integers."""
    nodes = []
    with open(path) as f:
        if header:
            next(f)
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            nodes.append(int(line.split(",")[0]))
    return nodes


def read_nodelist_header(path: Path) -> list[str]:
    """Return the column names from the first line of a nodelist CSV."""
    with open(path) as f:
        return f.readline().rstrip("\n").split(",")


def read_nodelist_rows(path: Path, header: bool = True) -> list[list[str]]:
    """Read all columns of a nodelist CSV, returning rows as lists of strings."""
    rows = []
    with open(path) as f:
        if header:
            next(f)
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rows.append(line.split(","))
    return rows


def read_graph(path: Path, fmt: str) -> frozenset:
    """Dispatch to the right reader based on format string."""
    if fmt == "metis":
        return read_metis(path)
    elif fmt == "csv":
        return read_edgelist(path)
    elif fmt == "parquet":
        return read_csr_parquet(path)
    else:
        raise ValueError(f"Unknown format: {fmt}")


# ── Compact-ID helpers ────────────────────────────────────────────────────────

def to_compact(edges: list[tuple[int, int]], nodes: list[int]) -> frozenset:
    """
    Remap raw node IDs to compact IDs (position in nodes list) for comparison
    against CSR output, which always uses compact IDs.
    """
    idx = {n: i for i, n in enumerate(nodes)}
    return frozenset(_edge(idx[u], idx[v]) for u, v in edges)


def intra_label_edges(edges: list[tuple[int, int]],
                      nodes: list[int],
                      labels: list[int]) -> dict[int, frozenset]:
    """
    Compute expected intra-label edges in compact-ID space, grouped by label.
    nodes[i] has label labels[i].
    """
    idx = {n: i for i, n in enumerate(nodes)}
    result: dict[int, set] = {}
    for u_raw, v_raw in edges:
        u, v = idx[u_raw], idx[v_raw]
        if labels[u] == labels[v]:
            lab = labels[u]
            result.setdefault(lab, set()).add(_edge(u, v))
    return {lab: frozenset(es) for lab, es in result.items()}


def local_id_edges(edges_compact: frozenset, label_nodes_compact: list[int]) -> frozenset:
    """
    Remap a set of global-compact-ID edges to local IDs within a sub-graph.
    label_nodes_compact: sorted list of global compact IDs that belong to this label.
    The local ID of global compact ID g is label_nodes_compact.index(g).
    """
    g2l = {g: l for l, g in enumerate(sorted(label_nodes_compact))}
    return frozenset(_edge(g2l[u], g2l[v]) for u, v in edges_compact)
