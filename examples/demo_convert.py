"""
Usage:
    uv run python examples/demo_convert.py

Template for conversions between edge lists (CSV or Parquet), CSR Parquet, and METIS.
Every spec lists all of its fields at their defaults; delete what you don't need.
"""

from pathlib import Path
import format_conversion.format as fmt

INPUT = Path("input")
OUTPUT = Path("output")


def main() -> None:
    # ── Node list ────────────────────────────────────────────────────────
    # Pass nodes=None for dense mode, where the raw id is the compact id.
    nodes = fmt.NodeDescriptor(
        INPUT / "dnc_nodes.csv",
        fmt.Nodelist.Csv(
            comment_char="#",
            skip_rows=1,            # the first is reused as the header of partition node lists
            base_index=0,           # subtracted from every raw id
        ),
    )

    # ── Input ────────────────────────────────────────────────────────────
    edges_file = INPUT / "dnc_edges.csv"                           ## CHANGEME
    input_spec = fmt.CsvEdgelist.Read(
        sep=",",                    # ',' | '\t' | ' '
        comment_char="#",           # '#' | '%'
        skip_rows=1,
        base_index=0,               # subtracted from every raw id
        keep_self_loops=False,
        directed=False,             # False stores both directions of each edge; True only u->v
    )

    # edges_file = INPUT / "dnc_edges.parquet"
    # input_spec = fmt.EdgelistParquet.Read(
    #     source_col="source",
    #     target_col="target",
    #     base_index=0,
    #     keep_self_loops=False,
    #     directed=False,
    # )

    # edges_file = INPUT / "dnc.indices.parquet"
    # input_spec = fmt.CsrParquet.Read(
    #     indices_col="indices",
    #     indptr_col="indptr",
    #     symmetric=True,           # the file stores both directions of each edge
    # )

    # edges_file = INPUT / "dnc.metis"
    # input_spec = fmt.Metis.Read(
    #     comment_char="#",
    # )

    graph_in = fmt.GraphDescriptor(edges_file, input_spec)

    # ── Output ───────────────────────────────────────────────────────────
    output_path = OUTPUT / "dnc"                                   ## CHANGEME
    output_spec = fmt.CsrParquet.Write(     # -> dnc.indices.parquet, dnc.indptr.parquet
        indices_col="indices",
        indptr_col="indptr",
        u64_indices=False,          # widen the indices column to uint64
    )

    # output_spec = fmt.CsvEdgelist.Write(  # -> dnc.csv
    #     sep=",",
    #     expand_symmetric=False,   # True emits both u,v and v,u for each undirected edge
    # )

    # output_spec = fmt.EdgelistParquet.Write(   # -> dnc.parquet
    #     source_col="source",
    #     target_col="target",
    #     u64_ids=False,            # widen both id columns to uint64
    #     expand_symmetric=False,
    # )

    # output_spec = fmt.Metis.Write()       # -> dnc.metis, undirected output only

    graph_out = fmt.GraphDescriptor(output_path, output_spec)

    # ── Convert ──────────────────────────────────────────────────────────
    output_path.parent.mkdir(exist_ok=True, parents=True)
    fmt.convert(
        graph_in,
        graph_out,
        nodes=nodes,
        num_threads=1,              # build, adjacency sort, and the CSV/METIS/edge-list writers
        sort_neighbors=False,       # sort each vertex's adjacency slice
    )

    # Read once, write several outputs:
    # fmt.convert(graph_in, [graph_out, other_out], nodes=nodes, num_threads=1)


if __name__ == "__main__":
    main()
