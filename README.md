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

### Parquet edge lists

`EdgesFormat.EDGELIST_PARQUET` reads and writes an edge list stored as Parquet: a
single file with two id columns, named `source` and `target` after the
[data-specification](https://github.com/illinois-or-research-analytics/data-specification/blob/main/formats.md).
Any other columns in an input file are ignored (and never decoded). Reading
accepts `uint32`, `uint64`, `int32` and `int64` columns; writing emits the CSR's
native index width, or `uint64` when `use_u64_indices` is set.

The path carries no special suffix — output writes `{output_path}.parquet`, and an
input is any `.parquet` file. Note that a CSR `.indices.parquet` also ends in
`.parquet`, so the two Parquet formats are distinguished by the `EdgesFormat`
argument, never by the file name.

```python
opts = fmt.ParseOptions()
opts.source_col = "src"      # optional; defaults to "source"
opts.target_col = "dst"      # optional; defaults to "target"
graph = fmt.GraphDescriptor("edges.parquet", fmt.EdgesFormat.EDGELIST_PARQUET, opts)
fmt.convert(graph, nodes, "out/g", fmt.EdgesFormat.CSR_PARQUET)
```

`sep`, `comment_char` and `skip_rows` describe text parsing and cannot apply to a
columnar input; setting them on a Parquet input raises rather than being silently
ignored.

### Parquet node lists

Node lists must be CSV. A node list is `N` rows rather than `E`, so converting one
externally is cheap even when converting an edge list would not be — for a graph
with 272M nodes it is ~2.7 GB of text, against ~100 GB for a 5B-edge list, which is
why the edge list is supported natively and this is not:

```python
import polars as pl
pl.scan_parquet("nodes.parquet").sink_csv("nodes.csv")
```

### Directed graphs

By default graphs are treated as **undirected**: each parsed edge is symmetrized
(both `u→v` and `v→u` are stored) and the edgelist writer emits each edge once as
`u,v` with `u<v`. Set `ParseOptions.directed = True` to treat edges as
**directed** instead: reading stores only the arc `u→v` (no symmetrization) and
the edgelist writer emits every stored arc.

`directed` is meaningful for `CSV_EDGELIST`, `EDGELIST_PARQUET` and `CSR_PARQUET`.
METIS is an undirected adjacency format, so requesting directed **METIS output**
raises an error. Note that no format carries a direction bit on disk; `directed`
describes how a given `convert` call interprets and emits edges, so set it
consistently across a round trip.

### Separate read and write options

`convert` reads its input using the options on the input `GraphDescriptor`, and
writes using an optional fifth argument, `output_opts`. When `output_opts` is
omitted (or `None`) the input options are reused, so the read and write sides can
differ when needed — for example reading comma-separated and writing
tab-separated, or requesting 64-bit CSR indices only on output:

```python
read_opts = fmt.ParseOptions(); read_opts.skip_rows = 1        # header on input
write_opts = fmt.ParseOptions(); write_opts.use_u64_indices = True  # output-only
fmt.convert(graph, nodes, output_path=out, output_fmt=fmt.EdgesFormat.CSR_PARQUET,
            output_opts=write_opts)
```

`use_u64_indices` is an **output-only** option (it sets the CSR indices column
width); setting it on options used for reading is an error. `sort_neighbors` is a
read-side transform and is always taken from the input options.

### Writing multiple formats from one read

`convert` is overloaded: instead of a single `(output_path, output_fmt)` it also
accepts a **list of output targets**, so the input is parsed and built only once
and then written to each format:

```python
u64 = fmt.ParseOptions(); u64.use_u64_indices = True
fmt.convert(graph, nodes, [
    ("out/dnc",   fmt.EdgesFormat.CSR_PARQUET, u64),   # (path, fmt, opts)
    ("out/dnc32", fmt.EdgesFormat.CSR_PARQUET),        # (path, fmt) — inherit input opts
    ("out/dnc",   fmt.EdgesFormat.METIS),
    ("out/dnc",   fmt.EdgesFormat.CSV_EDGELIST),
])
```

Each target is `(path, fmt)` or `(path, fmt, opts)`; a 2-tuple (or `opts=None`)
inherits the input options, exactly like the single-output form. Any
directed→METIS target is rejected up front, before any file is written, so that
misconfiguration is all-or-nothing; the outputs are otherwise written in list
order.

### Parallelism

While most of this is I/O bound, parallelism may help, depending on the architecture.
Here is an ongoing list of which readers and writers support parallelism:

| Format            | Input | Ouput |
|------------------ |-------|-------|
| Edgelist          | Yes   | No    |
| Edgelist (Parquet)| Yes†  | Yes‡  |
| CSR               | Yes*  | Yes*  |
| METIS             | No    | Yes   |

*Arrow parallelism is system dependant and is not explicitly controlled.

†Row groups are striped across threads, so a single-row-group file reads serially
regardless of `num_threads`.

‡Rows are materialised in parallel, but the file itself is written by a single
writer.

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
