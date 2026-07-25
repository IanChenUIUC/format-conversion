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

### Format specs

Every format has a spec type per direction, and the spec *is* the format tag: there
is no separate format argument. Each spec carries exactly the settings its code
path reads, so a setting that does not apply to a format is absent rather than
silently ignored.

| | Read spec | Write spec |
|---|---|---|
| CSV edge list     | `CsvEdgelist.Read(sep, comment_char, skip_rows, base_index, keep_self_loops, directed)` | `CsvEdgelist.Write(sep, base_index, expand_symmetric)` |
| METIS             | `Metis.Read(comment_char, base_index)` | `Metis.Write(base_index)` |
| CSR Parquet       | `CsrParquet.Read(indices_col, indptr_col, base_index, symmetric)` | `CsrParquet.Write(indices_col, indptr_col, base_index, u64_indices)` |
| Parquet edge list | `EdgelistParquet.Read(source_col, target_col, base_index, keep_self_loops, directed)` | `EdgelistParquet.Write(source_col, target_col, base_index, u64_ids, expand_symmetric)` |
| Node list         | `Nodelist.Csv(comment_char, skip_rows, base_index)` | — |
| Label list        | `Labels.Csv(comment_char, skip_rows)` | — |

`GraphDescriptor(path, spec)` describes both sides. With a read spec the path is an
existing file and is checked at construction; with a write spec it is the prefix
each writer appends its own extension to (`.csv`, `.metis`, `.parquet`,
`.indices.parquet` + `.indptr.parquet`).

`num_threads` and `sort_neighbors` are pipeline settings rather than format
settings, so they are keyword arguments of `convert` and `partition`.

```python
import format_conversion.format as fmt

graph = fmt.GraphDescriptor("edges.csv", fmt.CsvEdgelist.Read(skip_rows=1))
nodes = fmt.NodeDescriptor("nodes.csv", fmt.Nodelist.Csv(skip_rows=1))
out   = fmt.GraphDescriptor("out/dnc", fmt.CsrParquet.Write(u64_indices=True))

fmt.convert(graph, out, nodes=nodes, num_threads=16, sort_neighbors=True)
```

Passing a write spec where a read spec belongs (or the reverse) is rejected with a
message naming the offending spec. `examples/demo_convert.py` lists every field of
every spec at its default value.

### `base_index`

`base_index` is the id of the first vertex as it appears in the file. Reading
subtracts it, writing adds it, so `Metis.Read(base_index=b)` →
`Metis.Write(base_index=b)` and `CsrParquet.Write(base_index=k)` →
`CsrParquet.Read(base_index=k)` are both round trips. It defaults to 0 everywhere
except the two METIS specs, where it defaults to 1.

```python
# 1-indexed CSV in, 1-indexed CSR Parquet out
fmt.convert(
    fmt.GraphDescriptor("edges.csv", fmt.CsvEdgelist.Read(base_index=1)),
    fmt.GraphDescriptor("out/g",     fmt.CsrParquet.Write(base_index=1)),
)
```

Three format-specific notes:

- **METIS accepts only 0 or 1.** A METIS line carries no vertex id — line *i* is
  vertex *i* + `base_index` — so any other value would desynchronise the line
  order from the neighbour ids on the line.
- **`CsrParquet.Write(base_index=k)`** also prepends *k* zero entries to `indptr`,
  which is what makes the ids consistent with the offsets. `CsrParquet.Read`
  reverses that, and requires those leading vertices to have no edges rather than
  discarding them.
- **Edge lists cannot represent isolated vertices**, so writing one with a
  non-zero `base_index` shifts the ids and nothing else.

Ids are 32-bit, so a `base_index` that would push the largest id past `UINT32_MAX`
is rejected before anything is written. `partition()` accepts only each format's
default, since a shifted numbering would break the correspondence between a
label's `nodes.csv` rows and its local ids.

### Parquet edge lists

`EdgelistParquet` reads and writes an edge list stored as Parquet: a single file
with two id columns, named `source` and `target` after the
[data-specification](https://github.com/illinois-or-research-analytics/data-specification/blob/main/formats.md).
Any other columns in an input file are ignored (and never decoded). Reading
accepts `uint32`, `uint64`, `int32` and `int64` columns; writing emits the CSR's
native index width, or `uint64` when `u64_ids` is set.

The path carries no special suffix — output writes `{output_path}.parquet`, and an
input is any `.parquet` file. Note that a CSR `.indices.parquet` also ends in
`.parquet`, so the two Parquet formats are distinguished by the spec type, never by
the file name.

```python
graph = fmt.GraphDescriptor(
    "edges.parquet",
    fmt.EdgelistParquet.Read(source_col="src", target_col="dst"),
)
fmt.convert(graph, fmt.GraphDescriptor("out/g", fmt.CsrParquet.Write()), nodes=nodes)
```

### CSR column names

`CsrParquet` column names default to `indices` and `indptr` but are overridable on
both sides, so a CSR written by another tool (for example
[icebug-format](https://github.com/Ladybug-Memory/icebug-format), which names them
`target` and `ptr`) can be read directly:

```python
fmt.GraphDescriptor("g.indices.parquet",
                    fmt.CsrParquet.Read(indices_col="target", indptr_col="ptr"))
```

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
(both `u→v` and `v→u` are stored) and the edge-list writers emit each edge once as
`u,v` with `u<v`. Set `directed=True` on a read spec to store only the arc `u→v`;
the writers then emit every stored arc.

Direction lives on the **read** spec only. Whether a writer deduplicates or emits
every arc follows from how the graph was built, so a graph read as arcs only can
never be silently halved by an undirected write. The one write-side knob is
`expand_symmetric`, which emits both `u,v` and `v,u` for each edge of an undirected
graph; on a graph already stored as arcs only it changes nothing.

METIS is an undirected adjacency format, so a graph read with `directed=True`
cannot be written as METIS and is rejected before any file is created.

No format carries a direction bit on disk. For CSV and Parquet edge lists the read
spec says how to interpret the file. A CSR Parquet file cannot say, so
`CsrParquet.Read(symmetric=...)` is the caller's declaration — leave it `True` for a
CSR written from an undirected graph, and set it `False` for one holding out-arcs
only. Getting it wrong is what the METIS guard catches:

```python
fmt.convert(
    fmt.GraphDescriptor("g.indices.parquet", fmt.CsrParquet.Read(symmetric=False)),
    fmt.GraphDescriptor("out/g", fmt.Metis.Write()),
)   # raises: METIS output is undirected-only
```

### Read and write settings are separate

There is no inheritance between the two sides: a write spec supplies its own
settings, so reading tab-separated does not make the output tab-separated. This
also applies to `partition`, which takes its own write spec rather than reusing the
input's.

```python
fmt.convert(
    fmt.GraphDescriptor("edges.tsv", fmt.CsvEdgelist.Read(sep="\t")),
    fmt.GraphDescriptor("out/dnc", fmt.CsvEdgelist.Write(sep=",")),
)
```

### Writing multiple formats from one read

`convert` is overloaded: instead of a single output descriptor it also accepts a
**list** of them, so the input is parsed and built only once and then written to
each format:

```python
fmt.convert(graph, [
    fmt.GraphDescriptor("out/dnc",   fmt.CsrParquet.Write(u64_indices=True)),
    fmt.GraphDescriptor("out/dnc32", fmt.CsrParquet.Write()),
    fmt.GraphDescriptor("out/dnc",   fmt.Metis.Write()),
    fmt.GraphDescriptor("out/dnc",   fmt.CsvEdgelist.Write()),
], nodes=nodes, num_threads=16)
```

Every target is validated before the first byte is written — a rejected METIS
target or a read spec among the outputs makes the whole call raise and leaves no
files behind. The outputs are otherwise written in list order.

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

Pass `num_threads` to `convert` / `partition` to parallelise the CSV→CSR build
(and the neighbor sort, when `sort_neighbors` is enabled). `partition` accepts
`sort_neighbors` but does not yet implement it; passing `True` raises
`NotImplementedError` rather than silently doing nothing.

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
