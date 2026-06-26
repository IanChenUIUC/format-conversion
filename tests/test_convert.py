"""
Convert tests.

Structure: parametrize over output format (and sometimes input format for
round-trip tests).  Adding a new format means it's automatically covered
— no new test classes or functions needed.
"""

from __future__ import annotations
import pytest
from pathlib import Path
from .formats import FORMATS, Format
from .helpers import write_edgelist

try:
    from format_conversion.format import (
        EdgesFormat, ParseOptions,
        NodeDescriptor, GraphDescriptor,
        convert,
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


# ── CSV → each output format ──────────────────────────────────────────────────

@pytest.mark.parametrize("out_fmt", OUTPUT_FORMATS, ids=FORMAT_IDS)
class TestCSVToFormat:
    def test_edge_set(self, graph, out_fmt: Format, tmp_path):
        """Edge set is preserved after converting from CSV to any output format."""
        out = tmp_path / "out"
        convert(_csv_in(graph), _node(graph), out, out_fmt.fmt)
        assert out_fmt.read(out) == graph["spec"].compact_edges()

    def test_edge_count(self, graph, out_fmt: Format, tmp_path):
        out = tmp_path / "out"
        convert(_csv_in(graph), _node(graph), out, out_fmt.fmt)
        assert len(out_fmt.read(out)) == len(graph["spec"].edges)


# ── Round-trip: CSV → X → back to METIS ──────────────────────────────────────
#
# Convert CSV → intermediate format, then convert that back to METIS and
# compare with a direct CSV → METIS conversion.  The METIS output is used
# as common ground because it's the simplest text format to read.

@pytest.mark.parametrize("via_fmt", OUTPUT_FORMATS, ids=FORMAT_IDS)
def test_round_trip_via_format(graph, via_fmt: Format, tmp_path):
    metis_fmt = FORMATS["metis"]

    # Ground truth: CSV → METIS directly
    direct = tmp_path / "direct"
    convert(_csv_in(graph), _node(graph), direct, metis_fmt.fmt)

    # Indirect: CSV → via_fmt → METIS
    mid = tmp_path / "mid"
    convert(_csv_in(graph), _node(graph), mid, via_fmt.fmt)

    back = tmp_path / "back"
    convert(via_fmt.as_input(mid), None, back, metis_fmt.fmt)

    assert metis_fmt.read(back) == metis_fmt.read(direct)


# ── Dense (no node file) ──────────────────────────────────────────────────────

@pytest.mark.parametrize("out_fmt", OUTPUT_FORMATS, ids=FORMAT_IDS)
def test_dense_no_node_file(out_fmt: Format, tmp_path):
    """When nodes_file is None, IDs are assumed already 0-indexed (dense mode)."""
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, [(0, 1), (1, 2), (0, 2)], header=False)
    out = tmp_path / "out"
    g = GraphDescriptor(str(edges_path), EdgesFormat.CSV_EDGELIST, ParseOptions())
    convert(g, None, out, out_fmt.fmt)
    assert len(out_fmt.read(out)) == 3


# ── ParseOptions ──────────────────────────────────────────────────────────────
#
# These test the knobs, not the format dispatch, so one output format suffices.

@pytest.mark.parametrize("out_fmt", OUTPUT_FORMATS, ids=FORMAT_IDS)
def test_parallel_csv_read(graph, out_fmt: Format, tmp_path):
    """num_threads > 1 produces the same edge set as the default single-threaded path."""
    seq_out = tmp_path / "seq"
    par_out = tmp_path / "par"
    convert(_csv_in(graph),                    _node(graph), seq_out, out_fmt.fmt)
    convert(_csv_in(graph, num_threads=4),     _node(graph), par_out, out_fmt.fmt)
    assert out_fmt.read(par_out) == out_fmt.read(seq_out)

def test_base_index(tmp_path):
    """1-indexed edge list is shifted to 0-indexed compact IDs."""
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, [(1, 2), (2, 3), (1, 3)], header=False)
    opts = ParseOptions()
    opts.base_index = 1
    g = GraphDescriptor(str(edges_path), EdgesFormat.CSV_EDGELIST, opts)
    out = tmp_path / "out"
    convert(g, None, out, EdgesFormat.METIS)
    assert FORMATS["metis"].read(out) == frozenset([(0, 1), (1, 2), (0, 2)])

def test_comment_char(tmp_path):
    edges_path = tmp_path / "e.csv"
    edges_path.write_text("# comment\n0,1\n# another\n1,2\n2,3\n")
    g = GraphDescriptor(str(edges_path), EdgesFormat.CSV_EDGELIST, ParseOptions())
    out = tmp_path / "out"
    convert(g, None, out, EdgesFormat.METIS)
    assert FORMATS["metis"].read(out) == frozenset([(0, 1), (1, 2), (2, 3)])

def test_skip_rows(tmp_path):
    edges_path = tmp_path / "e.csv"
    edges_path.write_text("src,dst\n0,1\n1,2\n2,3\n")
    opts = ParseOptions()
    opts.skip_rows = 1
    g = GraphDescriptor(str(edges_path), EdgesFormat.CSV_EDGELIST, opts)
    out = tmp_path / "out"
    convert(g, None, out, EdgesFormat.METIS)
    assert FORMATS["metis"].read(out) == frozenset([(0, 1), (1, 2), (2, 3)])

def test_tsv_separator(tmp_path):
    edges_path = tmp_path / "e.tsv"
    edges_path.write_text("0\t1\n1\t2\n2\t3\n")
    opts = ParseOptions()
    opts.sep = "\t"
    g = GraphDescriptor(str(edges_path), EdgesFormat.CSV_EDGELIST, opts)
    out = tmp_path / "out"
    convert(g, None, out, EdgesFormat.METIS)
    assert FORMATS["metis"].read(out) == frozenset([(0, 1), (1, 2), (2, 3)])
