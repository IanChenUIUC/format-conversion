"""
Partition tests.

Parametrized over output format — adding a new format is one line in FORMATS,
no test changes needed.

Sub-graph edges are in local compact IDs (vertices renumbered 0..N_L-1 sorted
by their global compact ID); `graph["spec"].intra_local()` computes this.
"""

from __future__ import annotations
import sys
import pytest
from pathlib import Path
from .formats import FORMATS, Format
from .helpers import write_edgelist, read_nodelist, read_nodelist_rows, read_nodelist_header

try:
    from format_conversion.format import (
        EdgesFormat, ParseOptions,
        NodeDescriptor, GraphDescriptor,
        partition,
    )
    HAS_MODULE = True
except ImportError:
    HAS_MODULE = False

pytestmark = pytest.mark.skipif(not HAS_MODULE, reason="C++ module not built")

OUTPUT_FORMATS = list(FORMATS.values())
FORMAT_IDS     = list(FORMATS.keys())


# ── Helpers ───────────────────────────────────────────────────────────────────

def _csv_in(g: dict, **kw) -> GraphDescriptor:
    opts = ParseOptions()
    opts.skip_rows = 1  # fixture edge files always have a src,dst header
    for k, v in kw.items():
        setattr(opts, k, v)
    return GraphDescriptor(str(g["edges"]), EdgesFormat.CSV_EDGELIST, opts)

def _node(g: dict) -> NodeDescriptor:
    return NodeDescriptor(str(g["nodes"]))

def _read_label(out_dir: Path, label: int, fmt: Format) -> frozenset:
    return fmt.read(out_dir / str(label) / "graph")


# ── Core correctness ──────────────────────────────────────────────────────────

@pytest.mark.parametrize("out_fmt", OUTPUT_FORMATS, ids=FORMAT_IDS)
class TestPartitionCorrectness:
    def test_per_label_edge_count(self, graph, out_fmt: Format, tmp_path):
        """Each label's sub-graph has the expected number of intra-label edges."""
        out = tmp_path / "parts"
        partition(_csv_in(graph), _node(graph),
                  graph["labels"], out, out_fmt.fmt)
        for label, expected in graph["spec"].intra_local().items():
            result = _read_label(out, label, out_fmt)
            assert len(result) == len(expected), (
                f"label {label}: expected {len(expected)} edges, got {len(result)}"
            )

    def test_per_label_edge_set(self, graph, out_fmt: Format, tmp_path):
        """Each label's sub-graph contains exactly the correct local-ID edges."""
        out = tmp_path / "parts"
        partition(_csv_in(graph), _node(graph),
                  graph["labels"], out, out_fmt.fmt)
        for label, expected in graph["spec"].intra_local().items():
            assert _read_label(out, label, out_fmt) == expected

    def test_total_edge_count(self, graph, out_fmt: Format, tmp_path):
        """Sum of partition edge counts equals total intra-label edge count."""
        out = tmp_path / "parts"
        partition(_csv_in(graph), _node(graph),
                  graph["labels"], out, out_fmt.fmt)
        total    = sum(_read_label(out, lab, out_fmt).__len__()
                       for lab in graph["spec"].intra_local())
        expected = sum(len(es) for es in graph["spec"].intra_local().values())
        assert total == expected

    def test_nodelist_written(self, graph, out_fmt: Format, tmp_path):
        """
        Each partition directory contains a nodelist with:
          - the correct node IDs (first column, always)
          - all extra columns preserved verbatim (when input has multiple columns)
        """
        out = tmp_path / "parts"
        partition(_csv_in(graph), _node(graph),
                  graph["labels"], out, out_fmt.fmt)

        spec = graph["spec"]
        for label, expected_nodes in spec.label_nodes().items():
            nodelist_path = out / str(label) / "nodes.csv"
            assert nodelist_path.exists(), f"label {label}: nodes.csv missing"

            # First column: node IDs
            written_ids = read_nodelist(nodelist_path, header=True)
            assert sorted(written_ids) == sorted(expected_nodes), (
                f"label {label}: nodelist node IDs mismatch"
            )

            # All columns: verbatim row preservation (only checked for multi-column specs)
            if spec.node_attrs is not None:
                # The output header must carry all column names, not just "node_id".
                expected_header = ["node_id"] + (spec.node_attr_header or [])
                written_header = read_nodelist_header(nodelist_path)
                assert written_header == expected_header, (
                    f"label {label}: header {written_header!r} != expected {expected_header!r}"
                )

                expected_rows = {
                    row[0]: row
                    for row in spec.label_node_rows()[label]
                }
                for written_row in read_nodelist_rows(nodelist_path, header=True):
                    node_id = written_row[0]
                    assert written_row == expected_rows[node_id], (
                        f"label {label}: columns for node {node_id} not preserved verbatim"
                    )

    def test_partition_count(self, graph, out_fmt: Format, tmp_path):
        """Output contains exactly one sub-directory per unique label."""
        out = tmp_path / "parts"
        partition(_csv_in(graph), _node(graph),
                  graph["labels"], out, out_fmt.fmt)
        unique_labels = {str(l) for l in set(graph["spec"].labels)}
        subdirs = {p.name for p in out.iterdir() if p.is_dir()}
        assert subdirs == unique_labels


# ── Batch size ────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("batch_size", [1, 2, sys.maxsize], ids=["seq", "batch2", "all"])
def test_batch_size_consistent(graph, batch_size, tmp_path):
    """Any batch_size produces the same result as the default (all at once)."""
    fmt = FORMATS["metis"]
    out_ref = tmp_path / "ref"
    out_bat = tmp_path / f"b{batch_size}"

    g, n = _csv_in(graph), _node(graph)
    partition(g, n, graph["labels"], out_ref, fmt.fmt)
    partition(g, n, graph["labels"], out_bat, fmt.fmt, batch_size=batch_size)

    for label in graph["spec"].intra_local():
        assert _read_label(out_bat, label, fmt) == _read_label(out_ref, label, fmt), (
            f"batch_size={batch_size}, label {label}"
        )


# ── Label file options ────────────────────────────────────────────────────────

def test_label_skip_rows(graph, tmp_path):
    labels_hdr = tmp_path / "labels_hdr.txt"
    labels_hdr.write_text("label\n" + "\n".join(map(str, graph["spec"].labels)) + "\n")

    opts = ParseOptions()
    opts.skip_rows = 1
    out = tmp_path / "parts"
    partition(_csv_in(graph), _node(graph),
              labels_hdr, out, EdgesFormat.METIS, label_opts=opts)

    for label, expected in graph["spec"].intra_local().items():
        assert len(_read_label(out, label, FORMATS["metis"])) == len(expected)

def test_label_comment_char(graph, tmp_path):
    labels_comments = tmp_path / "labels_c.txt"
    lines = []
    for lab in graph["spec"].labels:
        lines += [f"# label for next node", str(lab)]
    labels_comments.write_text("\n".join(lines) + "\n")

    out = tmp_path / "parts"
    partition(_csv_in(graph), _node(graph),
              labels_comments, out, EdgesFormat.METIS)

    total = sum(_read_label(out, lab, FORMATS["metis"]).__len__()
                for lab in graph["spec"].intra_local())
    assert total == sum(len(es) for es in graph["spec"].intra_local().values())
