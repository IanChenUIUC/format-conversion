"""
Usage:
    uv run python examples/demo_partition.py

Example template for partitioning graphs given a labeling.
"""

from pathlib import Path
import format_conversion.format as fmt

INPUT = Path("input")
OUTPUT = Path("output")


def main() -> None:
    # ── Input ──────────────────────────────────────────────────────────
    input_fmt = fmt.EdgesFormat.CSV_EDGELIST  ## CHANGEME

    node_opts = fmt.ParseOptions()
    node_opts.skip_rows = 1  # header
    nodes_file = INPUT / "dnc_nodes.csv"

    label_opts = fmt.ParseOptions()
    labels_path = INPUT / "dnc.parts.2"

    edge_opts = fmt.ParseOptions()
    if input_fmt == fmt.EdgesFormat.CSV_EDGELIST:
        edge_opts.skip_rows = 1  # header
        edges_file = INPUT / "dnc_edges.csv"
    if input_fmt == fmt.EdgesFormat.CSR_PARQUET:
        edges_file = INPUT / "dnc.indices.parquet"
    if input_fmt == fmt.EdgesFormat.METIS:
        edges_file = INPUT / "dnc.metis"

    # ── Output ───────────────────────────────────────────
    output_fmt = fmt.EdgesFormat.CSR_PARQUET  ## CHANGEME
    output_dir = OUTPUT / "dnc.parts.2"

    # ── Partition ──────────────────────
    graph = fmt.GraphDescriptor(edges_file, input_fmt, edge_opts)
    nodes = fmt.NodeDescriptor(nodes_file, node_opts)
    fmt.partition(
        graph,
        nodes,
        labels_path=labels_path,
        label_opts=label_opts,
        output_dir=output_dir,
        output_fmt=output_fmt,
    )


if __name__ == "__main__":
    main()
