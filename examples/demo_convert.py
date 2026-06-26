"""
Usage:
    uv run python examples/demo_convert.py

Example template for conversions between edgelists, CSR, and metis.
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

    if output_fmt == fmt.EdgesFormat.CSV_EDGELIST:
        output_path = OUTPUT / "dnc_edges"
    if output_fmt == fmt.EdgesFormat.CSR_PARQUET:
        output_path = OUTPUT / "dnc"
    if output_fmt == fmt.EdgesFormat.METIS:
        output_path = OUTPUT / "dnc"

    # ── Convert ───────────────────────────────────────────
    graph = fmt.GraphDescriptor(edges_file, input_fmt, edge_opts)
    nodes = fmt.NodeDescriptor(nodes_file, node_opts)
    fmt.convert(graph, nodes, output_path=output_path, output_fmt=output_fmt)


if __name__ == "__main__":
    main()
