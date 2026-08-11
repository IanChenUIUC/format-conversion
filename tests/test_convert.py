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
                      read_edgelist_parquet_arcs, read_csr_parquet)

try:
    import format_conversion.format as fmt
    from format_conversion.format import NodeDescriptor, GraphDescriptor, convert
    HAS_MODULE = True
except ImportError:
    HAS_MODULE = False

pytestmark = pytest.mark.skipif(not HAS_MODULE, reason="C++ module not built")

OUTPUT_FORMATS = list(FORMATS.values())
FORMAT_IDS     = list(FORMATS.keys())


# ── Helpers ───────────────────────────────────────────────────────────────────

def _csv_in(g: dict, **kw) -> GraphDescriptor:
    kw.setdefault("skip_rows", 1)  # fixture edge files always have a src,dst header
    return GraphDescriptor(str(g["edges"]), fmt.CsvEdgelist.Read(**kw))

def _node(g: dict) -> NodeDescriptor:
    return NodeDescriptor(str(g["nodes"]))

def _csv_out(path, **kw) -> GraphDescriptor:
    return GraphDescriptor(str(path), fmt.CsvEdgelist.Write(**kw))

def _csr_out(path, **kw) -> GraphDescriptor:
    return GraphDescriptor(str(path), fmt.CsrParquet.Write(**kw))

def _metis_out(path, **kw) -> GraphDescriptor:
    return GraphDescriptor(str(path), fmt.Metis.Write(**kw))

def _metis_in(path, **kw) -> GraphDescriptor:
    return GraphDescriptor(str(path), fmt.Metis.Read(**kw))

def _csr_in(path, **kw) -> GraphDescriptor:
    return GraphDescriptor(str(path) + ".indices.parquet", fmt.CsrParquet.Read(**kw))

def _pq_out(path, **kw) -> GraphDescriptor:
    return GraphDescriptor(str(path), fmt.EdgelistParquet.Write(**kw))

def _pq_in(path, **kw) -> GraphDescriptor:
    return GraphDescriptor(str(path), fmt.EdgelistParquet.Read(**kw))

def _plain_csv_in(path, **kw) -> GraphDescriptor:
    kw.setdefault("skip_rows", 0)  # these fixtures are written without a header
    return GraphDescriptor(str(path), fmt.CsvEdgelist.Read(**kw))


# ── CSV → each output format ──────────────────────────────────────────────────

@pytest.mark.parametrize("out_fmt", OUTPUT_FORMATS, ids=FORMAT_IDS)
class TestCSVToFormat:
    def test_edge_set(self, graph, out_fmt: Format, tmp_path):
        """Edge set is preserved after converting from CSV to any output format."""
        out = tmp_path / "out"
        convert(_csv_in(graph), out_fmt.as_output(out), nodes=_node(graph))
        assert out_fmt.read(out) == graph["spec"].compact_edges()

    def test_edge_count(self, graph, out_fmt: Format, tmp_path):
        out = tmp_path / "out"
        convert(_csv_in(graph), out_fmt.as_output(out), nodes=_node(graph))
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
    convert(_csv_in(graph), metis_fmt.as_output(direct), nodes=_node(graph))

    # Indirect: CSV → via_fmt → METIS
    mid = tmp_path / "mid"
    convert(_csv_in(graph), via_fmt.as_output(mid), nodes=_node(graph))

    back = tmp_path / "back"
    convert(via_fmt.as_input(mid), metis_fmt.as_output(back))

    assert metis_fmt.read(back) == metis_fmt.read(direct)


# ── Dense (no node file) ──────────────────────────────────────────────────────

@pytest.mark.parametrize("out_fmt", OUTPUT_FORMATS, ids=FORMAT_IDS)
def test_dense_no_node_file(out_fmt: Format, tmp_path):
    """When nodes is None, IDs are assumed already 0-indexed (dense mode)."""
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, [(0, 1), (1, 2), (0, 2)], header=False)
    out = tmp_path / "out"
    convert(_plain_csv_in(edges_path), out_fmt.as_output(out))
    assert len(out_fmt.read(out)) == 3


# ── Read-spec knobs ───────────────────────────────────────────────────────────
#
# These test the knobs, not the format dispatch, so one output format suffices.

@pytest.mark.parametrize("out_fmt", OUTPUT_FORMATS, ids=FORMAT_IDS)
def test_parallel_csv_read(graph, out_fmt: Format, tmp_path):
    """num_threads > 1 produces the same edge set as the default single-threaded path."""
    seq_out = tmp_path / "seq"
    par_out = tmp_path / "par"
    convert(_csv_in(graph), out_fmt.as_output(seq_out), nodes=_node(graph))
    convert(_csv_in(graph), out_fmt.as_output(par_out), nodes=_node(graph), num_threads=4)
    assert out_fmt.read(par_out) == out_fmt.read(seq_out)

def test_base_index(tmp_path):
    """1-indexed edge list is shifted to 0-indexed compact IDs."""
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, [(1, 2), (2, 3), (1, 3)], header=False)
    out = tmp_path / "out"
    convert(_plain_csv_in(edges_path, base_index=1), _metis_out(out))
    assert FORMATS["metis"].read(out) == frozenset([(0, 1), (1, 2), (0, 2)])

def test_comment_char(tmp_path):
    edges_path = tmp_path / "e.csv"
    edges_path.write_text("# comment\n0,1\n# another\n1,2\n2,3\n")
    out = tmp_path / "out"
    convert(_plain_csv_in(edges_path), _metis_out(out))
    assert FORMATS["metis"].read(out) == frozenset([(0, 1), (1, 2), (2, 3)])

def test_skip_rows(tmp_path):
    edges_path = tmp_path / "e.csv"
    edges_path.write_text("src,dst\n0,1\n1,2\n2,3\n")
    out = tmp_path / "out"
    convert(_plain_csv_in(edges_path, skip_rows=1), _metis_out(out))
    assert FORMATS["metis"].read(out) == frozenset([(0, 1), (1, 2), (2, 3)])

def test_tsv_separator(tmp_path):
    edges_path = tmp_path / "e.tsv"
    edges_path.write_text("0\t1\n1\t2\n2\t3\n")
    out = tmp_path / "out"
    convert(_plain_csv_in(edges_path, sep="\t"), _metis_out(out))
    assert FORMATS["metis"].read(out) == frozenset([(0, 1), (1, 2), (2, 3)])


# ── Robustness: malformed input and option plumbing ──────────────────────────

def test_malformed_input_raises_not_aborts(tmp_path):
    """A non-numeric edge line must raise a catchable exception, not SIGABRT."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\nGARBAGE,2\n")
    nodes = tmp_path / "nodes.csv"
    nodes.write_text("node_id\n0\n1\n2\n")
    with pytest.raises(Exception):
        convert(_plain_csv_in(edges, skip_rows=1), _metis_out(tmp_path / "out"),
                nodes=NodeDescriptor(str(nodes)))


def test_bad_sep_raises(tmp_path):
    """An unsupported separator is rejected up front with a catchable error."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n")
    with pytest.raises(Exception):
        convert(_plain_csv_in(edges, skip_rows=1, sep="|"), _metis_out(tmp_path / "out"))


def test_keep_self_loops_option(tmp_path):
    """keep_self_loops is reachable from Python and changes the output."""
    import pyarrow.parquet as pq
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,0\n0,1\n")          # one self-loop, one real edge
    nodes = tmp_path / "nodes.csv"
    nodes.write_text("node_id\n0\n1\n")

    def total(name, **kw):
        convert(_plain_csv_in(edges, skip_rows=1, **kw), _csr_out(tmp_path / name),
                nodes=NodeDescriptor(str(nodes)))
        return pq.read_table(str(tmp_path / name) + ".indptr.parquet")["indptr"].to_pylist()[-1]

    # self-loop adds 2 directed entries (u->u twice in symmetric CSR) vs dropped
    assert total("keep_pq", keep_self_loops=True) > total("drop_pq")


def test_u64_indices_option(tmp_path):
    """u64_indices emits a uint64 indices column with values identical to the
    default uint32 path (only the dtype changes; indptr is unaffected)."""
    import pyarrow as pa, pyarrow.parquet as pq

    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n1,2\n2,0\n")
    nodes = tmp_path / "nodes.csv"
    nodes.write_text("node_id\n0\n1\n2\n")

    def run(name, **kw):
        convert(_plain_csv_in(edges, skip_rows=1), _csr_out(tmp_path / name, **kw),
                nodes=NodeDescriptor(str(nodes)))
        return (pq.read_table(str(tmp_path / name) + ".indices.parquet"),
                pq.read_table(str(tmp_path / name) + ".indptr.parquet"))

    t32, p32 = run("u32")
    t64, p64 = run("u64", u64_indices=True)

    assert t32.schema.field("indices").type == pa.uint32()
    assert t64.schema.field("indices").type == pa.uint64()
    # values identical despite differing dtype
    assert t64["indices"].to_pylist() == t32["indices"].to_pylist()
    # indptr unchanged (still uint64 in both cases)
    assert p64.schema.field("indptr").type == pa.uint64()
    assert p64["indptr"].to_pylist() == p32["indptr"].to_pylist()


def test_csr_custom_column_names(tmp_path):
    """CSR column names are configurable on both sides, so a CSR written with
    another tool's names (e.g. icebug's target/ptr) can be read back."""
    import pyarrow.parquet as pq
    edges = tmp_path / "edges.csv"
    edges.write_text("0,1\n1,2\n2,0\n")

    out = tmp_path / "named"
    convert(_plain_csv_in(edges), _csr_out(out, indices_col="target", indptr_col="ptr"))
    assert pq.read_table(str(out) + ".indices.parquet").schema.names == ["target"]
    assert pq.read_table(str(out) + ".indptr.parquet").schema.names == ["ptr"]

    back = tmp_path / "back"
    convert(GraphDescriptor(str(out) + ".indices.parquet",
                            fmt.CsrParquet.Read(indices_col="target", indptr_col="ptr")),
            _metis_out(back))
    assert FORMATS["metis"].read(back) == frozenset([(0, 1), (1, 2), (0, 2)])


def test_csr_default_column_names_do_not_match_custom(tmp_path):
    """Reading a custom-named CSR with the default names fails loudly."""
    edges = tmp_path / "edges.csv"
    edges.write_text("0,1\n1,2\n")
    out = tmp_path / "named"
    convert(_plain_csv_in(edges), _csr_out(out, indices_col="target", indptr_col="ptr"))
    with pytest.raises(Exception, match="not found"):
        convert(GraphDescriptor(str(out) + ".indices.parquet", fmt.CsrParquet.Read()),
                _metis_out(tmp_path / "back"))


# ── directed graphs ────────────────────────────────────────────────────────────

def test_directed_csv_preserves_arcs(tmp_path):
    """directed=True keeps each arc as-is: no symmetrizing, no de-dup, direction
    preserved. Undirected (default) symmetrizes each edge instead."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n1,0\n2,3\n")  # 0<->1 (two arcs), 2->3

    convert(_plain_csv_in(edges, skip_rows=1, directed=True), _csv_out(tmp_path / "dir"))
    assert read_edgelist_arcs(tmp_path / "dir.csv", header=True) == [(0, 1), (1, 0), (2, 3)]

    # Undirected default: each edge symmetrized; 0,1 and 1,0 are the same
    # undirected edge, emitted once with u<v per input occurrence (2 copies).
    convert(_plain_csv_in(edges, skip_rows=1), _csv_out(tmp_path / "und"))
    assert read_edgelist_arcs(tmp_path / "und.csv", header=True) == [(0, 1), (0, 1), (2, 3)]


def test_directed_csr_out_degrees(tmp_path):
    """A directed CSR stores only out-arcs: indptr gives out-degrees, indices the
    targets. Contrast with the undirected build which doubles the arc count."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n0,2\n2,0\n")

    convert(_plain_csv_in(edges, skip_rows=1, directed=True), _csr_out(tmp_path / "dir"))
    assert read_csr_arcs(tmp_path / "dir") == [(0, 1), (0, 2), (2, 0)]  # 3 arcs

    convert(_plain_csv_in(edges, skip_rows=1), _csr_out(tmp_path / "und"))
    assert len(read_csr_arcs(tmp_path / "und")) == 6  # symmetrized: 2x arcs


def test_directed_self_loop_kept_once(tmp_path):
    """With keep_self_loops, a directed self-loop u->u contributes exactly one
    arc (the undirected build would add two)."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n1,1\n")

    convert(_plain_csv_in(edges, skip_rows=1, directed=True, keep_self_loops=True),
            _csr_out(tmp_path / "dir"))
    assert read_csr_arcs(tmp_path / "dir") == [(1, 1)]


def test_directed_metis_output_errors(tmp_path):
    """METIS is undirected-only, so a graph read as arcs only is rejected."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n2,3\n")
    with pytest.raises(Exception, match="undirected-only"):
        convert(_plain_csv_in(edges, skip_rows=1, directed=True), _metis_out(tmp_path / "m"))


def test_directed_csr_roundtrip(tmp_path):
    """A directed CSR round-trips: write directed CSR, read it back declaring
    symmetric=False, write CSV; the arcs survive unchanged."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n1,0\n1,2\n2,1\n0,2\n")

    convert(_plain_csv_in(edges, skip_rows=1, directed=True), _csr_out(tmp_path / "csr"))
    expected = read_csr_arcs(tmp_path / "csr")

    convert(GraphDescriptor(str(tmp_path / "csr") + ".indices.parquet",
                            fmt.CsrParquet.Read(symmetric=False)),
            _csv_out(tmp_path / "back"))
    assert read_edgelist_arcs(tmp_path / "back.csv", header=True) == expected


def test_csr_symmetric_declaration_gates_metis(tmp_path):
    """A CSR file records no direction, so the caller declares it. symmetric=False
    rejects a METIS target; symmetric=True (the default) writes it."""
    edges = tmp_path / "edges.csv"
    edges.write_text("0,1\n1,2\n2,0\n")
    csr = tmp_path / "csr"
    convert(_plain_csv_in(edges), _csr_out(csr))
    indices = str(csr) + ".indices.parquet"

    with pytest.raises(Exception, match="undirected-only"):
        convert(GraphDescriptor(indices, fmt.CsrParquet.Read(symmetric=False)),
                _metis_out(tmp_path / "bad"))

    convert(GraphDescriptor(indices, fmt.CsrParquet.Read(symmetric=True)),
            _metis_out(tmp_path / "ok"))
    assert FORMATS["metis"].read(tmp_path / "ok") == frozenset([(0, 1), (1, 2), (0, 2)])


# ── expand_symmetric ───────────────────────────────────────────────────────────

@pytest.mark.parametrize("writer,reader", [
    ("csv", lambda p: read_edgelist_arcs(Path(str(p) + ".csv"), header=True)),
    ("edgelist_parquet", lambda p: read_edgelist_parquet_arcs(Path(str(p) + ".parquet"))),
], ids=["csv", "edgelist_parquet"])
def test_expand_symmetric_emits_both_directions(tmp_path, writer, reader):
    """expand_symmetric emits both u,v and v,u for each undirected edge: exactly
    the symmetric closure of the default output, twice as many rows."""
    edges = tmp_path / "edges.csv"
    edges.write_text("0,1\n1,2\n0,2\n")
    f = FORMATS[writer]

    convert(_plain_csv_in(edges), f.as_output(tmp_path / "plain"))
    convert(_plain_csv_in(edges), f.as_output(tmp_path / "exp", expand_symmetric=True))

    plain, expanded = reader(tmp_path / "plain"), reader(tmp_path / "exp")
    assert len(expanded) == 2 * len(plain)
    assert expanded == sorted([e for u, v in plain for e in ((u, v), (v, u))])


def test_expand_symmetric_is_noop_on_arcs_only_graph(tmp_path):
    """A graph read as arcs only already emits every arc, so the flag changes
    nothing rather than duplicating arcs."""
    edges = tmp_path / "edges.csv"
    edges.write_text("0,1\n1,0\n2,3\n")

    convert(_plain_csv_in(edges, directed=True), _csv_out(tmp_path / "plain"))
    convert(_plain_csv_in(edges, directed=True), _csv_out(tmp_path / "exp", expand_symmetric=True))
    assert read_edgelist_arcs(tmp_path / "exp.csv", header=True) == \
           read_edgelist_arcs(tmp_path / "plain.csv", header=True)


# ── read / write spec separation ───────────────────────────────────────────────

def test_write_spec_is_independent_of_read_spec(tmp_path):
    """The write spec supplies its own settings; nothing carries over from the
    read side. Here: read comma, write tab."""
    edges = tmp_path / "edges.csv"
    edges.write_text("src,dst\n0,1\n1,2\n")
    convert(_plain_csv_in(edges, skip_rows=1, sep=","), _csv_out(tmp_path / "out", sep="\t"))
    first = (tmp_path / "out.csv").read_text().splitlines()[0]
    assert "\t" in first and "," not in first


def test_tab_input_writes_comma_by_default(tmp_path):
    """A tab-separated input does not make the output tab-separated: the write
    spec's own default applies."""
    edges = tmp_path / "edges.tsv"
    edges.write_text("0\t1\n1\t2\n")
    convert(_plain_csv_in(edges, sep="\t"), _csv_out(tmp_path / "out"))
    assert "," in (tmp_path / "out.csv").read_text().splitlines()[0]


def test_swapped_descriptors_raise(tmp_path):
    """Passing a write descriptor as the input (or vice versa) is rejected, naming
    the offending spec."""
    edges = tmp_path / "edges.csv"
    edges.write_text("0,1\n")
    src = _plain_csv_in(edges)
    dst = _csr_out(tmp_path / "out")

    with pytest.raises(Exception, match=r"CsrParquet\.Write"):
        convert(dst, src)
    with pytest.raises(Exception, match=r"CsvEdgelist\.Read"):
        convert(src, src)


def test_descriptor_checks_path_only_for_read_specs(tmp_path):
    """A read descriptor fails early on a missing file; a write descriptor names a
    path that does not exist yet."""
    with pytest.raises(Exception, match="Cannot open"):
        GraphDescriptor(str(tmp_path / "nope.csv"), fmt.CsvEdgelist.Read())
    assert GraphDescriptor(str(tmp_path / "nope"), fmt.CsvEdgelist.Write()) is not None


@pytest.mark.parametrize("spec_factory,bad_kw", [
    (lambda **kw: fmt.CsvEdgelist.Write(**kw), {"skip_rows": 1}),
    (lambda **kw: fmt.CsvEdgelist.Write(**kw), {"comment_char": "%"}),
    (lambda **kw: fmt.EdgelistParquet.Read(**kw), {"sep": "\t"}),
    (lambda **kw: fmt.EdgelistParquet.Read(**kw), {"u64_ids": True}),
    (lambda **kw: fmt.CsvEdgelist.Read(**kw), {"u64_indices": True}),
    (lambda **kw: fmt.Metis.Write(**kw), {"directed": True}),
])
def test_specs_reject_fields_their_path_does_not_use(spec_factory, bad_kw):
    """A spec carries only the fields its code path reads, so a setting that does
    not apply cannot be constructed at all."""
    with pytest.raises(TypeError):
        spec_factory(**bad_kw)


# ── multi-output convert (read once, write many) ───────────────────────────────

def test_convert_multi_matches_single(tmp_path):
    """Writing several formats in one read-once call yields byte-identical output
    to separate single-output convert calls."""
    edges = tmp_path / "edges.csv"; edges.write_text("src,dst\n0,1\n1,2\n2,0\n")
    nodes = tmp_path / "nodes.csv"; nodes.write_text("node_id\n0\n1\n2\n")

    def gd(): return _plain_csv_in(edges, skip_rows=1)
    def nd(): return NodeDescriptor(str(nodes))

    convert(gd(), [_csr_out(tmp_path / "m"), _metis_out(tmp_path / "m"),
                   _csv_out(tmp_path / "m")], nodes=nd())
    convert(gd(), _csr_out(tmp_path / "s"), nodes=nd())
    convert(gd(), _metis_out(tmp_path / "s"), nodes=nd())
    convert(gd(), _csv_out(tmp_path / "s"), nodes=nd())

    for suffix in (".indices.parquet", ".indptr.parquet", ".metis", ".csv"):
        assert (tmp_path / f"m{suffix}").read_bytes() == (tmp_path / f"s{suffix}").read_bytes()


def test_convert_multi_per_output_specs(tmp_path):
    """Each output carries its own spec: uint64 vs uint32 CSR and a tab-separated
    CSV, all from one read."""
    import pyarrow as pa, pyarrow.parquet as pq
    edges = tmp_path / "edges.csv"; edges.write_text("src,dst\n0,1\n1,2\n2,0\n")
    nodes = tmp_path / "nodes.csv"; nodes.write_text("node_id\n0\n1\n2\n")

    convert(_plain_csv_in(edges, skip_rows=1), [
        _csr_out(tmp_path / "u64", u64_indices=True),
        _csr_out(tmp_path / "u32"),
        _csv_out(tmp_path / "tab", sep="\t"),
    ], nodes=NodeDescriptor(str(nodes)))

    assert pq.read_table(str(tmp_path / "u64.indices.parquet")).schema.field("indices").type == pa.uint64()
    assert pq.read_table(str(tmp_path / "u32.indices.parquet")).schema.field("indices").type == pa.uint32()
    first = (tmp_path / "tab.csv").read_text().splitlines()[0]
    assert "\t" in first and "," not in first


def test_convert_multi_metis_is_all_or_nothing(tmp_path):
    """A METIS output anywhere in the list makes the whole call raise before any
    file is written (pre-validation), even outputs listed earlier."""
    edges = tmp_path / "edges.csv"; edges.write_text("src,dst\n0,1\n2,3\n")

    with pytest.raises(Exception, match="undirected-only"):
        convert(_plain_csv_in(edges, skip_rows=1, directed=True), [
            _csv_out(tmp_path / "ok"),          # valid, listed first
            _metis_out(tmp_path / "bad"),       # invalid
        ])
    assert not (tmp_path / "ok.csv").exists()


def test_convert_multi_rejects_read_spec_target(tmp_path):
    """A read spec among the outputs is rejected before anything is written."""
    edges = tmp_path / "edges.csv"; edges.write_text("0,1\n")
    with pytest.raises(Exception, match=r"CsvEdgelist\.Read"):
        convert(_plain_csv_in(edges), [_csv_out(tmp_path / "ok"), _plain_csv_in(edges)])
    assert not (tmp_path / "ok.csv").exists()


def test_convert_multi_empty_is_noop(tmp_path):
    """An empty output list writes nothing and does not raise."""
    edges = tmp_path / "edges.csv"; edges.write_text("src,dst\n0,1\n")
    convert(_plain_csv_in(edges, skip_rows=1), [])
    assert list(tmp_path.glob("*.csv")) == [edges]


# ── EDGELIST_PARQUET ──────────────────────────────────────────────────────────
#
# The format registry already covers round trips, edge sets and counts. These
# pin the behaviour specific to a columnar edge list.

def test_edgelist_parquet_custom_column_names(tmp_path):
    """Column names are configurable on both the read and the write side."""
    import pyarrow.parquet as pq
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 1), (1, 2), (0, 2)], source_col="src", target_col="dst")

    out = tmp_path / "out"
    convert(_pq_in(src, source_col="src", target_col="dst"),
            _pq_out(out, source_col="src", target_col="dst"))

    assert pq.read_table(str(out) + ".parquet").schema.names == ["src", "dst"]
    assert read_edgelist_parquet(Path(str(out) + ".parquet"), "src", "dst") == \
        frozenset([(0, 1), (1, 2), (0, 2)])


def test_edgelist_parquet_missing_column_raises(tmp_path):
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 1)])
    with pytest.raises(Exception, match="not found"):
        convert(_pq_in(src, source_col="nope"), _csv_out(tmp_path / "o"))


def test_edgelist_parquet_directed_preserves_arcs(tmp_path):
    """directed=True stores only u->v and emits every stored arc."""
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 1), (2, 1), (1, 3)])
    out = tmp_path / "out"
    convert(_pq_in(src, directed=True), _pq_out(out))
    assert read_edgelist_parquet_arcs(Path(str(out) + ".parquet")) == [(0, 1), (1, 3), (2, 1)]


def test_edgelist_parquet_multi_row_group_thread_invariance(tmp_path):
    """Row groups are striped across threads, so the result must not depend on
    thread count. row_group_size=2 forces many row groups over few edges."""
    edges = [(i, i + 1) for i in range(50)]
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, edges, row_group_size=2)

    seq, par = tmp_path / "seq", tmp_path / "par"
    convert(_pq_in(src), _csr_out(seq), sort_neighbors=True)
    convert(_pq_in(src), _csr_out(par), sort_neighbors=True, num_threads=8)
    assert read_csr_arcs(par) == read_csr_arcs(seq)
    assert FORMATS["parquet"].read(par) == frozenset((u, v) for u, v in edges)


@pytest.mark.parametrize("statistics", [True, False], ids=["stats", "no_stats"])
@pytest.mark.parametrize("keep_self_loops", [True, False], ids=["keep_loops", "drop_loops"])
def test_edgelist_parquet_dense_mode(tmp_path, statistics, keep_self_loops):
    """Dense-mode N must not depend on whether the file's writer recorded
    statistics. The statistics fast path is only taken where it provably agrees
    with a scan, so all four combinations give the same N."""
    import pyarrow.parquet as pq
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 1), (1, 2)], statistics=statistics)
    out = tmp_path / "out"
    convert(_pq_in(src, keep_self_loops=keep_self_loops), _csr_out(out))

    assert pq.read_table(str(out) + ".indptr.parquet").num_rows == 4  # N=3 -> N+1 offsets
    assert FORMATS["parquet"].read(out) == frozenset([(0, 1), (1, 2)])


@pytest.mark.parametrize("statistics", [True, False], ids=["stats", "no_stats"])
def test_edgelist_parquet_dense_n_ignores_statistics(tmp_path, statistics):
    """Vertex 3 appears only in a self-loop, which is dropped by default. Statistics
    describe every row and would report N=4; a scan sees only surviving edges and
    reports N=2. N must not vary with the presence of statistics, and must match
    what the CSV reader produces for the same graph."""
    import pyarrow.parquet as pq
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 1), (3, 3)], statistics=statistics)
    out = tmp_path / "out"
    convert(_pq_in(src), _csr_out(out))

    csv_src = tmp_path / "in.csv"
    write_edgelist(csv_src, [(0, 1), (3, 3)], header=False)
    csv_out = tmp_path / "csv_out"
    convert(_plain_csv_in(csv_src), _csr_out(csv_out))

    n = pq.read_table(str(out) + ".indptr.parquet").num_rows
    assert n == pq.read_table(str(csv_out) + ".indptr.parquet").num_rows
    assert n == 3  # N=2 -> N+1 offsets


def test_edgelist_parquet_u64_ids(tmp_path):
    """u64_ids widens the emitted id columns, and 64-bit ids round trip."""
    import pyarrow as pa, pyarrow.parquet as pq
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 1), (1, 2)], dtype="uint64")
    out = tmp_path / "out"
    convert(_pq_in(src), _pq_out(out, u64_ids=True))

    schema = pq.read_table(str(out) + ".parquet").schema
    assert schema.field("source").type == pa.uint64()
    assert schema.field("target").type == pa.uint64()


def test_edgelist_parquet_self_loops(tmp_path):
    """Self-loops follow the same rule as every other reader."""
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(0, 0), (0, 1)])

    dropped, kept = tmp_path / "dropped", tmp_path / "kept"
    convert(_pq_in(src), _csr_out(dropped))
    convert(_pq_in(src, keep_self_loops=True), _csr_out(kept))
    assert read_csr_arcs(dropped) == [(0, 1), (1, 0)]
    assert (0, 0) in read_csr_arcs(kept)


def test_edgelist_parquet_empty(tmp_path):
    """An empty edge list writes a well-formed, readable file."""
    import pyarrow.parquet as pq
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [])
    out = tmp_path / "out"
    convert(_pq_in(src), _pq_out(out))

    t = pq.read_table(str(out) + ".parquet")
    assert t.num_rows == 0 and t.schema.names == ["source", "target"]


def test_u64_csr_indices_match_u32(tmp_path):
    """The streaming widening path must produce the same values as the zero-copy
    native-width path, only wider."""
    import pyarrow as pa, pyarrow.parquet as pq
    src = tmp_path / "in.parquet"
    write_edgelist_parquet(src, [(i, (i * 7) % 40) for i in range(200)], row_group_size=16)

    narrow, wide = tmp_path / "narrow", tmp_path / "wide"
    convert(_pq_in(src), _csr_out(narrow), sort_neighbors=True)
    convert(_pq_in(src), _csr_out(wide, u64_indices=True), sort_neighbors=True)

    n = pq.read_table(str(narrow) + ".indices.parquet")
    w = pq.read_table(str(wide) + ".indices.parquet")
    assert n.schema.field(0).type == pa.uint32()
    assert w.schema.field(0).type == pa.uint64()
    assert n.column(0).to_pylist() == w.column(0).to_pylist()


# ── base_index on the write side ──────────────────────────────────────────────

# path graph 0-1-2, as METIS writes it at each of the two permitted bases
_PATH3_EDGES = [(0, 1), (1, 2)]
_PATH3_METIS = {1: "3 2\n2\n1 3\n2\n",
                0: "3 2\n1\n0 2\n1\n"}


@pytest.mark.parametrize("base", [0, 1])
def test_metis_write_base(tmp_path, base):
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, _PATH3_EDGES, header=False)
    out = tmp_path / "out"
    convert(_plain_csv_in(edges_path), _metis_out(out, base_index=base), sort_neighbors=True)
    assert (tmp_path / "out.metis").read_text() == _PATH3_METIS[base]


@pytest.mark.parametrize("base", [0, 1])
def test_metis_round_trip_base(tmp_path, base):
    """Reading and writing at the same base reproduces the file."""
    src = tmp_path / "in.metis"
    src.write_text(_PATH3_METIS[base])
    out = tmp_path / "out"
    convert(_metis_in(src, base_index=base), _metis_out(out, base_index=base))
    assert (tmp_path / "out.metis").read_text() == _PATH3_METIS[base]


def test_metis_base_above_one_rejected(tmp_path):
    """A METIS line carries no vertex id, so only 0 and 1 are representable."""
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, _PATH3_EDGES, header=False)
    src = tmp_path / "in.metis"
    src.write_text(_PATH3_METIS[1])
    out = tmp_path / "out"

    with pytest.raises(RuntimeError, match="positional"):
        convert(_plain_csv_in(edges_path), _metis_out(out, base_index=2))
    with pytest.raises(RuntimeError, match="positional"):
        convert(_metis_in(src, base_index=2), _csr_out(out))


def test_csr_write_base_prepends_isolated_vertices(tmp_path):
    """base_index=k shifts every index and gives indptr k leading zeros, which is
    what keeps the two columns consistent."""
    import pyarrow.parquet as pq
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, _PATH3_EDGES, header=False)

    plain, shifted = tmp_path / "plain", tmp_path / "shifted"
    convert(_plain_csv_in(edges_path), _csr_out(plain), sort_neighbors=True)
    convert(_plain_csv_in(edges_path), _csr_out(shifted, base_index=1), sort_neighbors=True)

    p_ptr = pq.read_table(str(plain) + ".indptr.parquet").column(0).to_pylist()
    s_ptr = pq.read_table(str(shifted) + ".indptr.parquet").column(0).to_pylist()
    s_idx = pq.read_table(str(shifted) + ".indices.parquet").column(0).to_pylist()

    assert s_ptr == [0] + p_ptr
    assert all(v >= 1 for v in s_idx)
    assert read_csr_parquet(shifted, base=1) == read_csr_parquet(plain)


def test_csr_round_trip_base(tmp_path):
    """Writing at base k and reading at base k is the identity."""
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, _PATH3_EDGES, header=False)

    plain, shifted, back = tmp_path / "plain", tmp_path / "shifted", tmp_path / "back"
    convert(_plain_csv_in(edges_path), _csr_out(plain), sort_neighbors=True)
    convert(_plain_csv_in(edges_path), _csr_out(shifted, base_index=1), sort_neighbors=True)
    convert(_csr_in(shifted, base_index=1), _csr_out(back), sort_neighbors=True)

    for suffix in (".indices.parquet", ".indptr.parquet"):
        assert Path(str(back) + suffix).read_bytes() == Path(str(plain) + suffix).read_bytes()


def test_csr_read_base_requires_isolated_prefix(tmp_path):
    """Vertex 0 has edges, so dropping it would silently discard them."""
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, _PATH3_EDGES, header=False)
    csr = tmp_path / "csr"
    convert(_plain_csv_in(edges_path), _csr_out(csr))
    with pytest.raises(RuntimeError, match="no edges"):
        convert(_csr_in(csr, base_index=1), _csv_out(tmp_path / "out"))


def test_csv_write_base_shifts_every_id(tmp_path):
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, _PATH3_EDGES, header=False)

    plain, shifted = tmp_path / "plain", tmp_path / "shifted"
    convert(_plain_csv_in(edges_path), _csv_out(plain), sort_neighbors=True)
    convert(_plain_csv_in(edges_path), _csv_out(shifted, base_index=5), sort_neighbors=True)

    p_lines = (tmp_path / "plain.csv").read_text().splitlines()
    s_lines = (tmp_path / "shifted.csv").read_text().splitlines()
    assert len(p_lines) == len(s_lines)
    assert read_edgelist_arcs(tmp_path / "shifted.csv", header=True, base=5) == \
           read_edgelist_arcs(tmp_path / "plain.csv", header=True)


def test_edgelist_parquet_write_base_shifts_every_id(tmp_path):
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, _PATH3_EDGES, header=False)

    plain, shifted = tmp_path / "plain", tmp_path / "shifted"
    convert(_plain_csv_in(edges_path), _pq_out(plain), sort_neighbors=True)
    convert(_plain_csv_in(edges_path), _pq_out(shifted, base_index=5), sort_neighbors=True)

    assert read_edgelist_parquet_arcs(str(shifted) + ".parquet", base=5) == \
           read_edgelist_parquet_arcs(str(plain) + ".parquet")


@pytest.mark.parametrize("spec_out", ["csv", "csr", "edgelist_parquet"])
def test_base_index_overflow_rejected(tmp_path, spec_out):
    """Emitted ids stay 32-bit, so a base that pushes the largest past UINT32_MAX
    is rejected before anything is written."""
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, _PATH3_EDGES, header=False)
    out = tmp_path / "out"
    maker = {"csv": _csv_out, "csr": _csr_out, "edgelist_parquet": _pq_out}[spec_out]
    with pytest.raises(RuntimeError, match="overflows the 32-bit id range"):
        convert(_plain_csv_in(edges_path), maker(out, base_index=2**32))
    assert not list(tmp_path.glob("out*"))


def test_base_index_overflow_is_all_or_nothing(tmp_path):
    """A bad base on the second target leaves the first unwritten too."""
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, _PATH3_EDGES, header=False)
    good, bad = tmp_path / "good", tmp_path / "bad"
    with pytest.raises(RuntimeError, match="overflows the 32-bit id range"):
        convert(_plain_csv_in(edges_path), [_csv_out(good), _csr_out(bad, base_index=2**32)])
    assert not (tmp_path / "good.csv").exists()


# ── CSV header on the write side ──────────────────────────────────────────────

def test_csv_header_written_by_default(tmp_path):
    """The default names the two columns on a first line; header=False is the opt-out,
    and the two differ by exactly that line."""
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, _PATH3_EDGES, header=False)

    plain, headed = tmp_path / "plain", tmp_path / "headed"
    convert(_plain_csv_in(edges_path), _csv_out(plain, header=False), sort_neighbors=True)
    convert(_plain_csv_in(edges_path), _csv_out(headed), sort_neighbors=True)

    assert (tmp_path / "plain.csv").read_text() == "0,1\n1,2\n"
    assert (tmp_path / "headed.csv").read_text() == \
           "source,target\n" + (tmp_path / "plain.csv").read_text()


def test_csv_header_uses_column_names_and_sep(tmp_path):
    """The header line is source_col + sep + target_col, so it can never disagree
    with the separator the rows use."""
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, _PATH3_EDGES, header=False)
    out = tmp_path / "out"
    convert(_plain_csv_in(edges_path),
            _csv_out(out, sep="\t", source_col="src", target_col="dst"),
            sort_neighbors=True)

    lines = (tmp_path / "out.csv").read_text().splitlines()
    assert lines[0] == "src\tdst"
    assert lines[1] == "0\t1"


def test_csv_header_round_trip_at_defaults(tmp_path):
    """Write and Read default to each other: CsvEdgelist.Read()'s skip_rows=1 consumes
    the header CsvEdgelist.Write() emits, so a convert of a convert is byte-identical."""
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, _PATH3_EDGES, header=False)

    first, second = tmp_path / "first", tmp_path / "second"
    convert(_plain_csv_in(edges_path), _csv_out(first), sort_neighbors=True)
    convert(GraphDescriptor(str(first) + ".csv", fmt.CsvEdgelist.Read()),
            _csv_out(second), sort_neighbors=True)

    assert (tmp_path / "second.csv").read_bytes() == (tmp_path / "first.csv").read_bytes()


def test_csv_header_with_base_index(tmp_path):
    """The header is unaffected by base_index; only the ids shift."""
    edges_path = tmp_path / "e.csv"
    write_edgelist(edges_path, _PATH3_EDGES, header=False)
    out = tmp_path / "out"
    convert(_plain_csv_in(edges_path), _csv_out(out, base_index=1),
            sort_neighbors=True)

    lines = (tmp_path / "out.csv").read_text().splitlines()
    assert lines[0] == "source,target"
    assert read_edgelist_arcs(tmp_path / "out.csv", header=True, base=1) == \
           sorted(_PATH3_EDGES)


def test_csv_header_only_when_no_rows(tmp_path):
    """A graph with nothing to emit still gets its header, rather than the empty
    file the no-header path produces."""
    edges_path = tmp_path / "e.csv"
    edges_path.write_text("")
    empty, headed = tmp_path / "empty", tmp_path / "headed"
    convert(_plain_csv_in(edges_path), _csv_out(empty, header=False))
    convert(_plain_csv_in(edges_path), _csv_out(headed))

    assert (tmp_path / "empty.csv").read_text() == ""
    assert (tmp_path / "headed.csv").read_text() == "source,target\n"
