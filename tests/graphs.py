"""
Graph specifications used as test fixtures.
All expected values are derived programmatically from the spec.
"""

from __future__ import annotations
from dataclasses import dataclass
from pathlib import Path
from .helpers import write_edgelist, write_nodelist, write_labels


@dataclass
class GraphSpec:
    name:             str
    nodes:            list[int]            # raw IDs, defines compact ordering
    edges:            list[tuple[int,int]] # undirected, raw IDs
    labels:           list[int]            # labels[i] is the label of nodes[i]
    node_attrs:       list[list[str]] | None = None  # extra columns per node
    node_attr_header: list[str]      | None = None  # names for extra columns

    # ── Derived quantities ─────────────────────────────────────────────────

    def _idx(self) -> dict[int, int]:
        return {n: i for i, n in enumerate(self.nodes)}

    def compact_edges(self) -> frozenset:
        idx = self._idx()
        return frozenset(
            (min(idx[u], idx[v]), max(idx[u], idx[v]))
            for u, v in self.edges
        )

    def intra_compact(self) -> dict[int, frozenset]:
        idx = self._idx()
        result: dict[int, set] = {}
        for u_raw, v_raw in self.edges:
            u, v = idx[u_raw], idx[v_raw]
            if self.labels[u] == self.labels[v]:
                lab = self.labels[u]
                result.setdefault(lab, set()).add((min(u, v), max(u, v)))
        return {lab: frozenset(es) for lab, es in result.items()}

    def intra_local(self) -> dict[int, frozenset]:
        """Intra-label edges in local sub-graph IDs (sorted by global compact ID)."""
        intra = self.intra_compact()
        result: dict[int, frozenset] = {}
        for lab, edges in intra.items():
            label_verts = sorted(i for i, l in enumerate(self.labels) if l == lab)
            g2l = {g: l for l, g in enumerate(label_verts)}
            result[lab] = frozenset(
                (min(g2l[u], g2l[v]), max(g2l[u], g2l[v]))
                for u, v in edges
            )
        return result

    def label_nodes(self) -> dict[int, list[int]]:
        """Raw node IDs per label, in original nodelist order."""
        result: dict[int, list] = {}
        for node, label in zip(self.nodes, self.labels):
            result.setdefault(label, []).append(node)
        return result

    def label_node_rows(self) -> dict[int, list[list[str]]]:
        """
        Full nodelist rows per label (node ID + any extra columns), in
        original nodelist order. Used to verify verbatim row preservation.
        """
        result: dict[int, list] = {}
        for i, (node, label) in enumerate(zip(self.nodes, self.labels)):
            row = [str(node)]
            if self.node_attrs:
                row.extend(self.node_attrs[i])
            result.setdefault(label, []).append(row)
        return result

    # ── Fixture helper ─────────────────────────────────────────────────────

    def write(self, tmp: Path) -> dict:
        write_edgelist(tmp / "edges.csv", self.edges)
        write_nodelist(tmp / "nodes.csv", self.nodes,
                       node_attrs=self.node_attrs,
                       attr_header=self.node_attr_header)
        write_labels(tmp / "labels.txt", self.labels)
        return {
            "spec":   self,
            "edges":  tmp / "edges.csv",
            "nodes":  tmp / "nodes.csv",
            "labels": tmp / "labels.txt",
            "tmp":    tmp,
        }


# ── Graph library ──────────────────────────────────────────────────────────────

GRAPHS: list[GraphSpec] = [

    GraphSpec(
        name   = "triangle",
        nodes  = [0, 1, 2],
        edges  = [(0, 1), (1, 2), (0, 2)],
        labels = [0, 0, 0],
    ),

    GraphSpec(
        name   = "simple",
        nodes  = [0, 1, 2, 3, 4],
        edges  = [(0, 1), (1, 2), (2, 3), (3, 4), (0, 4), (1, 3)],
        labels = [0, 0, 1, 1, 0],
    ),

    GraphSpec(
        name   = "path_alternating",
        nodes  = [0, 1, 2, 3],
        edges  = [(0, 1), (1, 2), (2, 3)],
        labels = [0, 1, 0, 1],   # no intra-label edges
    ),

    GraphSpec(
        name   = "two_components",
        nodes  = [0, 1, 2, 3, 4, 5],
        edges  = [(0, 1), (1, 2), (0, 2), (3, 4), (4, 5), (3, 5)],
        labels = [0, 0, 0, 1, 1, 1],
    ),

    GraphSpec(
        name   = "sparse_ids",
        nodes  = [100, 200, 300, 400, 500],
        edges  = [(100, 200), (200, 300), (300, 400), (400, 500), (100, 500), (200, 400)],
        labels = [0, 0, 1, 1, 0],
    ),

    GraphSpec(
        name             = "multicolumn_nodes",
        nodes            = [0, 1, 2, 3],
        edges            = [(0, 1), (1, 2), (2, 3), (0, 3)],
        labels           = [0, 0, 1, 1],
        node_attrs       = [["alice", "1.5"], ["bob", "2.0"],
                            ["carol", "0.5"], ["dave", "3.0"]],
        node_attr_header = ["name", "weight"],
        # Verifies that extra nodelist columns are copied verbatim to each partition.
    ),

]

GRAPH_IDS = [g.name for g in GRAPHS]
