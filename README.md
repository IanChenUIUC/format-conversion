# About

High-performance utilities for converting between various graph formats.
Compatible with [data-specification](https://github.com/illinois-or-research-analytics/data-specification/blob/main/formats.md) and [icebug-format](https://github.com/Ladybug-Memory/icebug-format).

## Dependencies

- pyarrow
- omp

## External Libraries

Fast edgelist loading is inspired by and uses code from:

```bibtex
@software{Sahu_GVEL_Fast_Graph_2023,
  author = {Sahu, Subhajit},
  doi = {10.48550/arXiv.2311.14650},
  month = nov,
  title = {{GVEL: Fast Graph Loading in Edgelist and Compressed Sparse Row (CSR) formats}},
  version = {1.0.0},
  year = {2023}
}
```

Robinhood provides a faster alternative to `unordered_map`.

## Usage

The main patterns are described in the `examples/` folder, with format conversion as well as extracting subgraphs according to a partition.
Any input formats can be read or written to through the python or cpp API.

### Parallelism

While most of this is I/O bound, parallelism may help, depending on the architecture.
Here is an ongoing list of which readers and writers support parallelism:

| Format   | Input | Ouput |
|--------- |-------|-------|
| Edgelist | Yes   | No    |
| CSR      | Yes*  | Yes*  |
| METIS    | No    | Yes   |

*Arrow parallelism is system dependant and is not explicitly controlled.

Set `ParseOptions.num_threads` to the core count to parallelise the CSV→CSR build
(and the neighbor sort, when `sort_neighbors` is enabled).

### Performance on large runs

Two settings are left to the runtime rather than baked into the code:

- **Transparent huge pages.** The build advises its large arrays for 2 MB pages to
  cut TLB misses on the random scatter. Ensure
  `/sys/kernel/mm/transparent_hugepage/enabled` is `always` or `madvise`.
- **NUMA.** On multi-socket nodes, launch under `numactl --interleave=all` so the
  random traffic to the indices array draws bandwidth from all memory controllers.
  Single-socket nodes need nothing. The code does no NUMA-aware placement by design.

See `DESIGN.md` for the rationale behind these and the rest of the implementation.

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

Compressed Sparse Row format is a compact representation of the (directed) adjacency matrix.
We store the indices as the columns in which each row has data.
The non-zero entries denote the boundaries of the rows.

Here, we use 64 bit nodeIDs, to be consistent with [icebug](https://github.com/Ladybug-Memory/icebug/).

### METIS

METIS format, also known as chaco format, stores undirected unweighted graphs in plaintext.

n is number of vertices, m is number of undirected edges, vertex ids are 1-indexed:
```
n m
v2 v3 v4
v1 v3 v5 v6 v8
v1 v2 
...
```

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

# Benchmarks

This repository was meant to be easily extensible for future formats, as well as memory efficient.
Having high throughput generally follows from the above principles, and parallel I/O readers and writers are implemented alongside that.

I do not know of any other repos that provide a simple format conversion.
Here, we show that for many combinations of input and output formats, we are faster and more efficient than other parsers.
These experiments are non-extensive.

## Comparison to icebug-format

TODO; minhyuk's duckdb icebug pipeline does edgelist to csr

## Conversion to NetworKit

TODO; compare to NetworKit reader/writer, which handles edgelists and metis

# AI Usage Declaration

Much of this codebase was implemented with assistance of Claude Code.
