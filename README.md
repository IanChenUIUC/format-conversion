# About

High-performance utilities for converting between various graph formats.
Compatible with [data-specification](https://github.com/illinois-or-research-analytics/data-specification/blob/main/formats.md) and [icebug-format](https://github.com/Ladybug-Memory/icebug-format).

## Dependencies

## External Libraries

Fast edgelist loading is inspired by and uses code from:
`bibtex
@software{Sahu_GVEL_Fast_Graph_2023,
  author = {Sahu, Subhajit},
  doi = {10.48550/arXiv.2311.14650},
  month = nov,
  title = {{GVEL: Fast Graph Loading in Edgelist and Compressed Sparse Row (CSR) formats}},
  version = {1.0.0},
  year = {2023}
}
`

Robinhood provides a faster alternative to `unordered_map`.

# Formats

## Edges

### edgelist

An edgelist is a character-delimited text file containing at least two columns:

| Column | Description |
|--------|-------------|
| source | Node ID representing the source of an edge (in directed graphs) or one endpoint (in undirected graphs) |
| target | Node ID representing the target of an edge (in directed graphs) or the other endpoint (in undirected graphs) |

Nodes may not need to have contiguous IDs.

### CSR (parquet)

See icebug-format.
0-indexed.

### metis

Undirected only.
1-indexed.

## Nodes

A nodelist is a character-delimited text file containing at least one column:

| Column | Description |
|--------|-------------|
| node_id | Value that **uniquely** identifies a node within a specific graph |

Here, we restrict node_ids to be non-negative integers (not necessarily contiguous).

### Additional Node Attributes

Additional columns may be included to represent node attributes such as:
- type
- fitness
- other custom attributes

The `node_id` column should be the first column, no matter if there are any additional attributes.
