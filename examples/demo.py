"""
Demo: convert an edge list to METIS, then partition it by node label.

Usage:
    uv run python examples/demo.py

Expects in input/:
    edges.csv    — comma-separated edge list (src,dst header optional)
    nodes.csv    — single-column node ID list (node_id header optional)
    labels.txt   — one integer label per line, one per node in nodelist order

Writes to output/:
    graph.metis             — full graph in METIS format
    partitions/{label}/graph.csv  — per-label edge lists (dense compact IDs)
"""

from pathlib import Path
import format_conversion.format as fmt

INPUT = Path("input")
OUTPUT = Path("output")


def main() -> None:
    OUTPUT.mkdir(exist_ok=True)
    (OUTPUT / "partitions").mkdir(exist_ok=True)

    # ── Descriptors ──────────────────────────────────────────────────────────

    edge_opts = fmt.ParseOptions()
    edge_opts.skip_rows = 1  # skip "source,target" header

    graph = fmt.GraphDescriptor(
        INPUT / "edges.csv", fmt.EdgesFormat.CSV_EDGELIST, edge_opts
    )
    nodes = fmt.NodeDescriptor(INPUT / "nodes.csv")

    # ── Convert: edge list → METIS ───────────────────────────────────────────

    metis_out = OUTPUT / "graph"
    fmt.convert(graph, nodes, metis_out, fmt.EdgesFormat.METIS)
    print(f"Wrote {metis_out}.metis")

    # ── Partition: edge list → per-label CSV edge lists ──────────────────────
    #
    # Output edgelists use dense compact IDs (0-indexed within each sub-graph).
    # Each sub-graph is written to output/partitions/{label}/graph.csv.

    label_opts = fmt.ParseOptions()

    fmt.partition(
        graph,
        nodes,
        labels_path=INPUT / "labels.txt",
        label_opts=label_opts,
        output_dir=OUTPUT / "partitions",
        output_fmt=fmt.EdgesFormat.CSV_EDGELIST,
    )

    for label_dir in sorted((OUTPUT / "partitions").iterdir()):
        edge_file = label_dir / "graph.csv"
        node_file = label_dir / "nodes.csv"
        if edge_file.exists():
            n_edges = sum(1 for _ in open(edge_file))
            n_nodes = sum(1 for _ in open(node_file)) - 1  # subtract header
            print(f"  label {label_dir.name}: {n_nodes=}, {n_edges=} ")


if __name__ == "__main__":
    main()
