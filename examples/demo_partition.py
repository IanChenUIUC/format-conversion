"""
Usage:
    uv run python examples/demo_partition.py

Template for partitioning a graph given a labeling: one sub-graph and node list per
label. Every spec lists all of its fields at their defaults; delete what you don't need.
"""

from pathlib import Path
import format_conversion.format as fmt

INPUT = Path("input")
OUTPUT = Path("output")


def main() -> None:
    nodes = fmt.NodeDescriptor(
        INPUT / "dnc_nodes.csv",
        fmt.Nodelist.Csv(
            comment_char="#",
            skip_rows=1,            # the first is reused as the header of the per-label node lists
            base_index=0,           # subtracted from every raw id
        ),
    )

    # ── Input ────────────────────────────────────────────────────────────
    # See demo_convert.py for the other three read specs.          ## CHANGEME
    graph_in = fmt.GraphDescriptor(
        INPUT / "dnc_edges.csv",
        fmt.CsvEdgelist.Read(
            sep=",",
            comment_char="#",
            skip_rows=1,
            base_index=0,
            keep_self_loops=False,
            directed=False,
        ),
    )

    # ── Labels ───────────────────────────────────────────────────────────
    # One label per line, in compact-id order.
    labels_path = INPUT / "dnc.parts.2"
    label_spec = fmt.Labels.Csv(
        comment_char="#",
        skip_rows=0,
    )

    # ── Output ───────────────────────────────────────────────────────────
    # Written as <output_dir>/<label>/graph.<ext> plus <label>/nodes.csv.
    output_dir = OUTPUT / "dnc.parts.2"                            ## CHANGEME
    output_spec = fmt.CsrParquet.Write(
        indices_col="indices",
        indptr_col="indptr",
        u64_indices=False,
    )

    # ── Partition ────────────────────────────────────────────────────────
    fmt.partition(
        graph_in,
        labels_path,
        output_dir,
        output_spec,
        nodes=nodes,
        label_spec=label_spec,
        num_threads=1,
        batch_size=1000,            # sub-graphs materialised at once; caps peak memory
    )


if __name__ == "__main__":
    main()
