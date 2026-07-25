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
from .helpers import (write_edgelist, read_edgelist_arcs, read_csr_arcs,
                      write_edgelist_parquet, read_edgelist_parquet,
                      read_edgelist_parquet_arcs)

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


# ── Robustness: malformed input and option plumbing ──────────────────────────

def test_malformed_input_raises_not_aborts(tmp_path):
    """A non-numeric edge line must raise a catchable exception, not SIGABRT."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\nGARBAGE,2\n")
    nodes = tmp_path / "nodes.csv"
    nodes.write_text("node_id\n0\n1\n2\n")
    opts = ParseOptions(); opts.skip_rows = 1
    gd = GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, opts)
    nd = NodeDescriptor(str(nodes))
    with pytest.raises(Exception):
        convert(gd, nd, tmp_path / "out", EdgesFormat.METIS)


def test_bad_sep_raises(tmp_path):
    """An unsupported separator is rejected up front with a catchable error."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n")
    opts = ParseOptions(); opts.skip_rows = 1; opts.sep = "|"
    gd = GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, opts)
    with pytest.raises(Exception):
        convert(gd, None, tmp_path / "out", EdgesFormat.METIS)


def test_keep_self_loops_option(tmp_path):
    """keep_self_loops is reachable from Python and changes the output."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,0\n0,1\n")          # one self-loop, one real edge
    nodes = tmp_path / "nodes.csv"
    nodes.write_text("node_id\n0\n1\n")
    metis = FORMATS["metis"]

    # default: self-loop dropped
    o1 = ParseOptions(); o1.skip_rows = 1
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, o1),
            NodeDescriptor(str(nodes)), tmp_path / "drop", EdgesFormat.METIS)
    dropped = metis.read(tmp_path / "drop")

    # keep_self_loops=True: self-loop retained somewhere in the CSR
    o2 = ParseOptions(); o2.skip_rows = 1; o2.keep_self_loops = True
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, o2),
            NodeDescriptor(str(nodes)), tmp_path / "keep", EdgesFormat.METIS)
    # METIS reader drops u==v pairs, so compare total neighbor counts via indptr/CSR instead:
    import pyarrow.parquet as pq
    o3 = ParseOptions(); o3.skip_rows = 1; o3.keep_self_loops = True
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, o3),
            NodeDescriptor(str(nodes)), tmp_path / "keep_pq", EdgesFormat.CSR_PARQUET)
    kept_total = pq.read_table(str(tmp_path / "keep_pq") + ".indptr.parquet")["indptr"].to_pylist()[-1]

    o4 = ParseOptions(); o4.skip_rows = 1
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, o4),
            NodeDescriptor(str(nodes)), tmp_path / "drop_pq", EdgesFormat.CSR_PARQUET)
    dropped_total = pq.read_table(str(tmp_path / "drop_pq") + ".indptr.parquet")["indptr"].to_pylist()[-1]

    # self-loop adds 2 directed entries (u->u twice in symmetric CSR) vs dropped
    assert kept_total > dropped_total


def test_use_u64_indices_option(tmp_path):
    """use_u64_indices emits a uint64 indices column with values identical to the
    default uint32 path (only the dtype changes; indptr is unaffected)."""
    import pyarrow.parquet as pq

    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n1,2\n2,0\n")
    nodes = tmp_path / "nodes.csv"
    nodes.write_text("node_id\n0\n1\n2\n")

    # default: uint32 indices
    o32 = ParseOptions(); o32.skip_rows = 1
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, o32),
            NodeDescriptor(str(nodes)), tmp_path / "u32", EdgesFormat.CSR_PARQUET)
    t32 = pq.read_table(str(tmp_path / "u32") + ".indices.parquet")
    p32 = pq.read_table(str(tmp_path / "u32") + ".indptr.parquet")

    # use_u64_indices=True on OUTPUT opts: uint64 indices, same values
    o_in = ParseOptions(); o_in.skip_rows = 1
    o_out = ParseOptions(); o_out.use_u64_indices = True
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, o_in),
            NodeDescriptor(str(nodes)), tmp_path / "u64", EdgesFormat.CSR_PARQUET, o_out)
    t64 = pq.read_table(str(tmp_path / "u64") + ".indices.parquet")
    p64 = pq.read_table(str(tmp_path / "u64") + ".indptr.parquet")

    import pyarrow as pa
    assert t32.schema.field("indices").type == pa.uint32()
    assert t64.schema.field("indices").type == pa.uint64()
    # values identical despite differing dtype
    assert t64["indices"].to_pylist() == t32["indices"].to_pylist()
    # indptr unchanged (still uint64 in both cases)
    assert p64.schema.field("indptr").type == pa.uint64()
    assert p64["indptr"].to_pylist() == p32["indptr"].to_pylist()


# ── directed graphs ────────────────────────────────────────────────────────────

def test_directed_csv_preserves_arcs(tmp_path):
    """directed=true keeps each arc as-is: no symmetrizing, no de-dup, direction
    preserved. Undirected (default) symmetrizes each edge instead."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n1,0\n2,3\n")  # 0<->1 (two arcs), 2->3

    ri = ParseOptions(); ri.skip_rows = 1; ri.directed = True
    wo = ParseOptions(); wo.directed = True
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, ri),
            None, tmp_path / "dir", EdgesFormat.CSV_EDGELIST, wo)
    assert read_edgelist_arcs(tmp_path / "dir.csv") == [(0, 1), (1, 0), (2, 3)]

    # Undirected default: each edge symmetrized; 0,1 and 1,0 are the same
    # undirected edge, emitted once with u<v per input occurrence (2 copies).
    ru = ParseOptions(); ru.skip_rows = 1
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, ru),
            None, tmp_path / "und", EdgesFormat.CSV_EDGELIST)
    assert read_edgelist_arcs(tmp_path / "und.csv") == [(0, 1), (0, 1), (2, 3)]


def test_directed_csr_out_degrees(tmp_path):
    """A directed CSR stores only out-arcs: indptr gives out-degrees, indices the
    targets. Contrast with the undirected build which doubles the arc count."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n0,2\n2,0\n")

    ri = ParseOptions(); ri.skip_rows = 1; ri.directed = True
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, ri),
            None, tmp_path / "dir", EdgesFormat.CSR_PARQUET)
    assert read_csr_arcs(tmp_path / "dir") == [(0, 1), (0, 2), (2, 0)]  # 3 arcs

    ru = ParseOptions(); ru.skip_rows = 1
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, ru),
            None, tmp_path / "und", EdgesFormat.CSR_PARQUET)
    assert len(read_csr_arcs(tmp_path / "und")) == 6  # symmetrized: 2x arcs


def test_directed_self_loop_kept_once(tmp_path):
    """With keep_self_loops, a directed self-loop u->u contributes exactly one
    arc (the undirected build would add two)."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n1,1\n")

    ri = ParseOptions(); ri.skip_rows = 1; ri.directed = True; ri.keep_self_loops = True
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, ri),
            None, tmp_path / "dir", EdgesFormat.CSR_PARQUET)
    assert read_csr_arcs(tmp_path / "dir") == [(1, 1)]


def test_directed_metis_output_errors(tmp_path):
    """METIS is undirected-only; requesting directed METIS output raises."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n2,3\n")
    ri = ParseOptions(); ri.skip_rows = 1; ri.directed = True
    wo = ParseOptions(); wo.directed = True
    with pytest.raises(Exception):
        convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, ri),
                None, tmp_path / "m", EdgesFormat.METIS, wo)


def test_directed_csr_roundtrip(tmp_path):
    """A directed CSR round-trips: write directed CSR, read it back, write directed
    CSV; the arcs survive unchanged."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n1,0\n1,2\n2,1\n0,2\n")

    ri = ParseOptions(); ri.skip_rows = 1; ri.directed = True
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, ri),
            None, tmp_path / "csr", EdgesFormat.CSR_PARQUET)
    expected = read_csr_arcs(tmp_path / "csr")

    # CSR_PARQUET carries no direction bit; the caller sets directed on both sides.
    wo = ParseOptions(); wo.directed = True
    convert(GraphDescriptor(str(tmp_path / "csr") + ".indices.parquet",
                            EdgesFormat.CSR_PARQUET, ParseOptions()),
            None, tmp_path / "back", EdgesFormat.CSV_EDGELIST, wo)
    assert read_edgelist_arcs(tmp_path / "back.csv") == expected


# ── separate input / output options ────────────────────────────────────────────

def test_output_opts_separate_separator(tmp_path):
    """Read comma, write tab in a single convert via output_opts."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n1,2\n")

    ri = ParseOptions(); ri.skip_rows = 1; ri.sep = ","
    wo = ParseOptions(); wo.sep = "\t"
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, ri),
            None, tmp_path / "out", EdgesFormat.CSV_EDGELIST, wo)
    first = (tmp_path / "out.csv").read_text().splitlines()[0]
    assert "\t" in first and "," not in first


def test_output_opts_defaults_to_input(tmp_path):
    """Omitting output_opts reuses the input opts for writing (here: the ','
    separator carries through to the output)."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n1,2\n")
    ri = ParseOptions(); ri.skip_rows = 1; ri.sep = ","
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, ri),
            None, tmp_path / "out", EdgesFormat.CSV_EDGELIST)
    assert "," in (tmp_path / "out.csv").read_text().splitlines()[0]


def test_use_u64_indices_on_read_errors(tmp_path):
    """use_u64_indices is output-only; setting it on the read/input opts errors."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n1,2\n")
    bad = ParseOptions(); bad.skip_rows = 1; bad.use_u64_indices = True
    with pytest.raises(Exception):
        convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, bad),
                None, tmp_path / "x", EdgesFormat.CSR_PARQUET)


# ── multi-output convert (read once, write many) ───────────────────────────────

def test_convert_multi_matches_single(tmp_path):
    """Writing several formats in one read-once call yields byte-identical output
    to separate single-output convert calls."""
    edges = tmp_path / "edges.csv"; edges.write_text("src,dst\n0,1\n1,2\n2,0\n")
    nodes = tmp_path / "nodes.csv"; nodes.write_text("node_id\n0\n1\n2\n")
    ri = ParseOptions(); ri.skip_rows = 1

    def gd(): return GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, ri)
    def nd(): return NodeDescriptor(str(nodes))

    convert(gd(), nd(), [
        (str(tmp_path / "m"), EdgesFormat.CSR_PARQUET),
        (str(tmp_path / "m"), EdgesFormat.METIS),
        (str(tmp_path / "m"), EdgesFormat.CSV_EDGELIST),
    ])
    # references
    convert(gd(), nd(), str(tmp_path / "s"), EdgesFormat.CSR_PARQUET)
    convert(gd(), nd(), str(tmp_path / "s"), EdgesFormat.METIS)
    convert(gd(), nd(), str(tmp_path / "s"), EdgesFormat.CSV_EDGELIST)

    for suffix in (".indices.parquet", ".indptr.parquet", ".metis", ".csv"):
        assert (tmp_path / f"m{suffix}").read_bytes() == (tmp_path / f"s{suffix}").read_bytes()


def test_convert_multi_shorthand_and_per_output_opts(tmp_path):
    """2-tuple shorthand inherits input opts; 3-tuple opts apply per output
    (uint64 vs uint32 CSR, and a tab-separated CSV, from one read)."""
    import pyarrow as pa, pyarrow.parquet as pq
    edges = tmp_path / "edges.csv"; edges.write_text("src,dst\n0,1\n1,2\n2,0\n")
    nodes = tmp_path / "nodes.csv"; nodes.write_text("node_id\n0\n1\n2\n")
    ri = ParseOptions(); ri.skip_rows = 1
    u64 = ParseOptions(); u64.use_u64_indices = True
    tab = ParseOptions(); tab.sep = "\t"

    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, ri), NodeDescriptor(str(nodes)), [
        (str(tmp_path / "u64"), EdgesFormat.CSR_PARQUET, u64),
        (str(tmp_path / "u32"), EdgesFormat.CSR_PARQUET),          # 2-tuple shorthand
        (str(tmp_path / "tab"), EdgesFormat.CSV_EDGELIST, tab),
    ])
    assert pq.read_table(str(tmp_path / "u64.indices.parquet")).schema.field("indices").type == pa.uint64()
    assert pq.read_table(str(tmp_path / "u32.indices.parquet")).schema.field("indices").type == pa.uint32()
    first = (tmp_path / "tab.csv").read_text().splitlines()[0]
    assert "\t" in first and "," not in first


def test_convert_multi_directed_metis_is_all_or_nothing(tmp_path):
    """A directed→METIS output anywhere in the list makes the whole call raise
    before any file is written (pre-validation), even outputs listed earlier."""
    edges = tmp_path / "edges.csv"; edges.write_text("src,dst\n0,1\n2,3\n")
    rd = ParseOptions(); rd.skip_rows = 1; rd.directed = True
    wd = ParseOptions(); wd.directed = True

    with pytest.raises(Exception):
        convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, rd), None, [
            (str(tmp_path / "ok"), EdgesFormat.CSV_EDGELIST, wd),   # valid, listed first
            (str(tmp_path / "bad"), EdgesFormat.METIS, wd),         # invalid
        ])
    # the earlier valid output must not have been written
    assert not (tmp_path / "ok.csv").exists()


def test_convert_multi_empty_is_noop(tmp_path):
    """An empty output list writes nothing and does not raise."""
    edges = tmp_path / "edges.csv"; edges.write_text("src,dst\n0,1\n")
    ri = ParseOptions(); ri.skip_rows = 1
    convert(GraphDescriptor(str(edges), EdgesFormat.CSV_EDGELIST, ri), None, [])
    assert list(tmp_path.glob("*.csv")) == [edges]


# ── EDGELIST_PARQUET ──────────────────────────────────────────────────────────
#
# The format registry already covers round trips, edge sets and counts. These
# pin the behaviour specific to a columnar edge list.

def _opts(**kw) -> ParseOptions:
    o = ParseOptions()
    for k, v in kw.items():
        setattr(o, k, v)
    return o


def _pq_in(path, **kw) -> GraphDescriptor:
    return GraphDescriptor(str(path), EdgesFormat.EDGELIST_PARQUET, _opts(**kw))


def test_edgelist_parquet_custom_column_names(tmp_path):
    """Column names are configurable on both the read and the write side."""
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 1), (1, 2), (0, 2)], source_col="src", target_col="dst")

    out = tmp_path / "out"
    o = ParseOptions(); o.source_col = "src"; o.target_col = "dst"
    convert(_pq_in(src, source_col="src", target_col="dst"), None,
            out, EdgesFormat.EDGELIST_PARQUET, o)

    import pyarrow.parquet as pq
    assert pq.read_table(str(out) + ".parquet").schema.names == ["src", "dst"]
    assert read_edgelist_parquet(Path(str(out) + ".parquet"), "src", "dst") == \
        frozenset([(0, 1), (1, 2), (0, 2)])


def test_edgelist_parquet_missing_column_raises(tmp_path):
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 1)])
    with pytest.raises(Exception, match="not found"):
        convert(_pq_in(src, source_col="nope"), None, tmp_path / "o", EdgesFormat.CSV_EDGELIST)


@pytest.mark.parametrize("opt,value", [("skip_rows", 1), ("sep", "\t"), ("comment_char", "%")])
def test_edgelist_parquet_rejects_text_options(tmp_path, opt, value):
    """Text-parsing options cannot apply to a columnar input, so they raise rather
    than being silently ignored."""
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 1)])
    with pytest.raises(Exception):
        convert(_pq_in(src, **{opt: value}), None, tmp_path / "o", EdgesFormat.CSV_EDGELIST)


def test_edgelist_parquet_directed_preserves_arcs(tmp_path):
    """directed=True stores only u->v and emits every stored arc."""
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 1), (2, 1), (1, 3)])
    out = tmp_path / "out"
    convert(_pq_in(src, directed=True), None, out, EdgesFormat.EDGELIST_PARQUET,
            _opts(directed=True))
    assert read_edgelist_parquet_arcs(Path(str(out) + ".parquet")) == [(0, 1), (1, 3), (2, 1)]


def test_edgelist_parquet_multi_row_group_thread_invariance(tmp_path):
    """Row groups are striped across threads, so the result must not depend on
    thread count. row_group_size=2 forces many row groups over few edges."""
    edges = [(i, i + 1) for i in range(50)]
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, edges, row_group_size=2)

    seq, par = tmp_path / "seq", tmp_path / "par"
    convert(_pq_in(src, sort_neighbors=True), None, seq, EdgesFormat.CSR_PARQUET)
    convert(_pq_in(src, sort_neighbors=True, num_threads=8), None, par, EdgesFormat.CSR_PARQUET)
    assert read_csr_arcs(par) == read_csr_arcs(seq)
    assert FORMATS["parquet"].read(par) == frozenset((u, v) for u, v in edges)


@pytest.mark.parametrize("statistics", [True, False], ids=["stats", "no_stats"])
@pytest.mark.parametrize("keep_self_loops", [True, False], ids=["keep_loops", "drop_loops"])
def test_edgelist_parquet_dense_mode(tmp_path, statistics, keep_self_loops):
    """Dense-mode N must not depend on whether the file's writer recorded
    statistics. The statistics fast path is only taken where it provably agrees
    with a scan, so all four combinations give the same N."""
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 1), (1, 2)], statistics=statistics)
    out = tmp_path / "out"
    convert(_pq_in(src, keep_self_loops=keep_self_loops), None, out, EdgesFormat.CSR_PARQUET)

    import pyarrow.parquet as pq
    assert pq.read_table(str(out) + ".indptr.parquet").num_rows == 4  # N=3 -> N+1 offsets
    assert FORMATS["parquet"].read(out) == frozenset([(0, 1), (1, 2)])


@pytest.mark.parametrize("statistics", [True, False], ids=["stats", "no_stats"])
def test_edgelist_parquet_dense_n_ignores_statistics(tmp_path, statistics):
    """Vertex 3 appears only in a self-loop, which is dropped by default. Statistics
    describe every row and would report N=4; a scan sees only surviving edges and
    reports N=2. N must not vary with the presence of statistics, and must match
    what the CSV reader produces for the same graph."""
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 1), (3, 3)], statistics=statistics)
    out = tmp_path / "out"
    convert(_pq_in(src), None, out, EdgesFormat.CSR_PARQUET)

    csv_src = tmp_path / "in.csv"
    write_edgelist(csv_src, [(0, 1), (3, 3)], header=False)
    csv_out = tmp_path / "csv_out"
    convert(GraphDescriptor(str(csv_src), EdgesFormat.CSV_EDGELIST, ParseOptions()), None,
            csv_out, EdgesFormat.CSR_PARQUET)

    import pyarrow.parquet as pq
    n = pq.read_table(str(out) + ".indptr.parquet").num_rows
    assert n == pq.read_table(str(csv_out) + ".indptr.parquet").num_rows
    assert n == 3  # N=2 -> N+1 offsets


def test_edgelist_parquet_u64_ids(tmp_path):
    """use_u64_indices widens the emitted id columns, and 64-bit ids round trip."""
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 1), (1, 2)], dtype="uint64")
    out = tmp_path / "out"
    convert(_pq_in(src), None, out, EdgesFormat.EDGELIST_PARQUET, _opts(use_u64_indices=True))

    import pyarrow as pa, pyarrow.parquet as pq
    schema = pq.read_table(str(out) + ".parquet").schema
    assert schema.field("source").type == pa.uint64()
    assert schema.field("target").type == pa.uint64()


def test_edgelist_parquet_self_loops(tmp_path):
    """Self-loops follow the same rule as every other reader."""
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 0), (0, 1)])

    dropped, kept = tmp_path / "dropped", tmp_path / "kept"
    convert(_pq_in(src), None, dropped, EdgesFormat.CSR_PARQUET)
    convert(_pq_in(src, keep_self_loops=True), None, kept, EdgesFormat.CSR_PARQUET)
    assert read_csr_arcs(dropped) == [(0, 1), (1, 0)]
    assert (0, 0) in read_csr_arcs(kept)


def test_edgelist_parquet_empty(tmp_path):
    """An empty edge list writes a well-formed, readable file."""
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [])
    out = tmp_path / "out"
    convert(_pq_in(src), None, out, EdgesFormat.EDGELIST_PARQUET)

    import pyarrow.parquet as pq
    t = pq.read_table(str(out) + ".parquet")
    assert t.num_rows == 0 and t.schema.names == ["source", "target"]


def test_u64_csr_indices_match_u32(tmp_path):
    """The streaming widening path must produce the same values as the zero-copy
    native-width path, only wider."""
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(i, (i * 7) % 40) for i in range(200)], row_group_size=16)

    narrow, wide = tmp_path / "narrow", tmp_path / "wide"
    convert(_pq_in(src, sort_neighbors=True), None, narrow, EdgesFormat.CSR_PARQUET)
    convert(_pq_in(src, sort_neighbors=True), None, wide, EdgesFormat.CSR_PARQUET,
            _opts(use_u64_indices=True))

    import pyarrow as pa, pyarrow.parquet as pq
    n = pq.read_table(str(narrow) + ".indices.parquet")
    w = pq.read_table(str(wide) + ".indices.parquet")
    assert n.schema.field(0).type == pa.uint32()
    assert w.schema.field(0).type == pa.uint64()
    assert n.column(0).to_pylist() == w.column(0).to_pylist()
