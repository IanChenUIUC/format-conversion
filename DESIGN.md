# Design notes

This document records the *why* behind the implementation. Source comments are
kept to interface/usage level; the reasoning lives here.

## File layout

| File | Responsibility |
|------|----------------|
| `system.h` | OS/parallel primitives: `MmapFile`, `HugeArray`, `adviseHugePages`, `parallelStripes`. No graph knowledge. |
| `formats.h` | Core types: `EdgesFormat`, `ParseOptions`, `NodeDescriptor`, `GraphDescriptor`. |
| `graph_read.h` | Read paths → in-memory CSR (`DiGraphCsr`): node-id remapping, the CSV→CSR build, METIS/Parquet readers, `sortNeighbors`. |
| `graph_write.h` | Write paths from CSR: METIS, CSV, Parquet. |
| `convert.h` / `partition.h` | User-facing pipelines. |
| `format_wrap.cpp` | pybind11 bindings. |

The in-memory representation throughout is puzzlef's `DiGraphCsr<K=uint32_t,
O=uint64_t>` — symmetric CSR (both directions stored), `offsets` (`O`) and
`edgeKeys` (`K`).

## CSV → CSR build (`buildCSRFromCSV`)

Two passes over the mmap'd text, parallel over `num_threads`, with
**no atomics on the per-edge path**:

1. The file is tiled into 1 MB line-aligned blocks. Thread `t` owns blocks
   `t, t+T, 2T, …` — a round-robin assignment that interleaves dense and sparse
   regions so one hub-heavy region can't stall a pass (social-network inputs are
   highly skewed). The assignment depends only on `(t, T)`, so it is identical in
   both passes.
2. **Pass 1** counts degrees into a private per-thread row of a flat `T×N`
   table (`tdeg`), so no two threads touch the same counter. Row-major by thread
   keeps each thread's row contiguous and avoids false sharing.
3. A prefix sum over total per-vertex degree yields `offsets`; the same `tdeg`
   table is then rewritten in place so `tdeg[t·N+v]` becomes thread `t`'s write
   cursor into `v`'s adjacency slice.
4. **Pass 2** scatters: because each thread's contributions to a vertex occupy a
   private, disjoint slice computed in step 3, writes need no atomics. Every slot
   is written exactly once, so `edgeKeys` needs no zeroing.

Correctness depends on `parallelStripes` using `schedule(static)` so iteration
`t` maps to the same thread (hence the same blocks and cursor row) across passes.

Scratch memory is `T × N × sizeof(O)`. This is the main scaling cost: at very
high thread counts and node counts it grows large (the path to ~60B edges would
want per-*block* rather than per-*thread* accumulation — deferred).

## Node-id remapping (`NodeMap`)

Three representations, chosen when the node list is scanned:

- **Dense** — no node list; the raw id is the compact id, `N` discovered while
  scanning edges. No memory, no lookup.
- **Array** — `remap[raw - min_id]`. For "compact but offset" id spaces (raw ids
  span a range within `MAX_REMAP_SPAN_RATIO`× of `N`). One subtract + one load per
  lookup; measured ~1.8× faster on the read than the hash map for such inputs.
- **Hash** — robin_hood map; fallback when the id span dwarfs `N` (e.g. genuinely
  sparse 64-bit ids), where an array would waste too much memory.

The compact id is always the row's position in the node list (file order).

`K` (index type) caps node ids at `2^(8·sizeof(K))`. `forEachValidEdge` checks
this when narrowing a parsed 64-bit id to `K` (compiled out for `K=uint64`) and
raises rather than silently truncating.

## Memory, huge pages, and NUMA

The large arrays (`edgeKeys`, the scratch table) are random-access and far larger
than cache. Two independent concerns:

- **TLB reach** — 4 KB pages can't map enough of a multi-GB array, so the random
  scatter pays page-table walks. The scratch is 2 MB-aligned and both it and
  `edgeKeys` are `madvise(MADV_HUGEPAGE)`'d. Requires transparent huge pages
  enabled (`/sys/kernel/mm/transparent_hugepage/enabled` = `always` or `madvise`).
- **NUMA** — left entirely to the runtime. On multi-socket nodes, launch under
  `numactl --interleave=all` so the random traffic to `edgeKeys` draws bandwidth
  from all memory controllers. There is intentionally no NUMA-aware placement in
  the code: for random-access arrays, interleave is the right policy and it
  overrides any first-touch placement anyway. Single-socket nodes need nothing.

## Parquet output encoding (`writeGraphToParquet`)

Two single-column files (`.indices.parquet`, `.indptr.parquet`). Both columns are
wrapped zero-copy from the CSR vectors (no cast/copy); indices are written at
their native `K` width (uint32 by default), not widened to uint64.

These files are the long-term on-disk form, tuned for size:

- **No dictionary** — indices are high-cardinality; at large `N` the index codes
  are barely narrower than the raw values, so a dictionary is overhead.
- **Delta (`DELTA_BINARY_PACKED`) + zstd** — `indptr` is monotonic and collapses
  to its gaps (degrees). `indices` benefit from delta's frame-of-reference, and
  more so when adjacency is sorted (`ParseOptions::sort_neighbors`); on highly
  skewed (power-law) graphs the hub lists compress well. The gain is degree- and
  distribution-dependent — modest on near-uniform low-degree graphs, larger on
  social-network-style inputs.
- **Statistics kept** — cheap, and enable reader-side page skipping.

For a low-latency / zero-copy consumer, convert these to Feather/Arrow IPC, which
is the throughput-oriented format; that conversion is standard and need not be
optimised here.

## Adjacency sort (`sortNeighbors`)

A general, format-independent CSR operation: sorts each vertex's adjacency slice
in place, parallel over vertices with dynamic scheduling (degree skew). Useful for
downstream binary-search edge lookups, set-intersection kernels, traversal
locality, and the compressibility of delta-encoded output. Output adjacency order
is otherwise unspecified (it depends on thread/block scheduling), so byte-identical
output across thread counts requires this sort.

## Partition (`extractSubgraphs`)

Materialises sub-CSRs for a batch of labels in two passes (degree count, then
scatter), reusing the same CSR skeleton as the build. Currently serial. `batch_size`
caps how many sub-CSRs are built at once; peak `edgeKeys` across a full pass is
bounded by total intra-label edges ≤ |E|.

## Robustness

Worker exceptions are surfaced as normal catchable errors: `parallelStripes`
captures the first exception thrown in any thread and rethrows it after the region
(an exception cannot cross an OpenMP region boundary, so without this a malformed
input would terminate the process). Parser-option validation happens once, before
threads spawn.

## Known ceilings / future work

- `K=uint32` caps node count at ~4.29B (fine for current targets; widen `K` past
  that).
- Build scratch is `T×N×8`
- `partition`, the CSV writer, and the METIS reader are single-threaded cold paths.
