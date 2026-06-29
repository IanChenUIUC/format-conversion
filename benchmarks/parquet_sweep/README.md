# Parquet encoding/compression size sweep

Measures the **on-disk size** of a graph's edge data stored as Parquet under a
matrix of encodings, compressions, and sort orders. Sizes only — no timing, no
read-back, nothing written to disk except the results CSV (each config is encoded
to an in-memory buffer and its size recorded).

## What it sweeps

| dimension     | values |
|---------------|--------|
| `repr`        | `csr` (the `indices` column; indptr/nodelist ignored), `edgelist` (`src`,`dst`) |
| `encoding`    | `dictionary`, `plain`, `delta` |
| `compression` | `none`, `snappy`, `zstd:3`, `zstd:19`, `gzip` |
| `sorted`      | `0`, `1` (adjacency / edge order sorted before writing) |

`csr.indices` holds `2m` neighbor ids; `edgelist` holds `m` rows of `(src,dst)` for
`u<v`. Statistics are disabled so sizes reflect encoded data, not page metadata.

## Run

```bash
# 1. generate the config matrix (applies the size-invariant prune; see below)
python gen_sweep.py -o sweep.csv

# 2. build (needs an Arrow/Parquet C++ install + submodules checked out)
cmake -S . -B build && cmake --build build -j

# 3. run over the graphs in the manifest
./build/bench_parquet --manifest graphs.csv --sweep sweep.csv --results results.csv
```

`--build-threads N` controls only CSR construction (default: all cores); the sweep
itself is serial.

## Manifest (`graphs.csv`)

One row per graph; add a row to add a graph.

```
name,path,format,nodes,skip_rows
dnc,../../input/dnc_edges.csv,csv,../../input/dnc_nodes.csv,1
```

- `format`: `csv`, `metis`, or `csr_parquet`.
- `nodes`: optional node list for id remapping (empty → dense ids).
- `skip_rows`: header lines to skip.

## Results (`results.csv`)

```
graph,n,m,repr,encoding,compression,sorted,bytes
```

`n` = nodes, `m` = undirected edges, `bytes` = encoded size of the representation.
Everything else (bytes/edge, ratios vs a baseline) is derivable in your own R
script.

## The prune

`gen_sweep.py` drops `sorted=1` rows where `encoding=plain` and `compression=none`,
since raw fixed-width sizes are independent of order — they equal their `sorted=0`
twins exactly. Regenerate them in R by copying the `sorted=0` `bytes` for the
matching `(graph, repr, encoding, compression)`. Pass `--no-prune` to emit them.
No other combination is pruned (dictionary and compressed encodings can change with
order).
