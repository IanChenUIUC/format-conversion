# Design notes

This document records the *why* behind the implementation. Source comments are
kept to interface/usage level; the reasoning lives here.

## File layout

| File | Responsibility |
|------|----------------|
| `system.h` | OS/parallel primitives: `MmapFile`, `HugeArray`, `adviseHugePages`, `parallelStripes`. No graph knowledge. |
| `formats.h` | Core types: the ten format specs, the `GraphSpec` variant, `NodeDescriptor`, `GraphDescriptor`, `BuiltGraph`. |
| `graph_read.h` | Read paths → in-memory CSR (`DiGraphCsr`): node-id remapping, the format-agnostic CSR build, METIS/Parquet readers, `sortNeighbors`. |
| `graph_write.h` | Write paths from CSR: METIS, CSV, Parquet. |
| `convert.h` / `partition.h` | User-facing pipelines. |
| `format_wrap.cpp` | pybind11 bindings. |

The in-memory representation throughout is puzzlef's `DiGraphCsr<K=uint32_t,
O=uint64_t>` — CSR with `offsets` (`O`) and `edgeKeys` (`K`). By default the
build stores a symmetric CSR (both directions of each edge); with a read spec's
`directed` it stores only the out-arcs `u→v`.

## One options type per format per direction

`ParseOptions` used to be a single struct of 11 fields serving four readers, four
writers, the node list and the label list. No path used more than seven of them,
and the rest were mostly *silently ignored*: METIS and CSR-Parquet reads dropped
`base_index`, `keep_self_loops`, `directed` and `num_threads` on the floor, while
three other combinations had to be rejected at runtime. A field that a code path
never reads is a place for a wrong value to sit unnoticed.

Now the format tag *is* the options type, split by side: `CsvEdgelistRead`,
`CsvEdgelistWrite`, and so on. Each carries exactly the fields its path consults,
which deleted three runtime validations outright — `use_u64_indices` on a read
spec, text options on a Parquet input, and `directed` on METIS output are all
unrepresentable rather than rejected.

Two consequences worth naming:

- **No shared base struct.** `base_index` / `keep_self_loops` / `directed` are
  declared in both edge-read structs rather than inherited from a common base.
  Only two of the ten specs share them, and duplicating three fields keeps the
  pybind11 keyword constructors flat; `mapRawEdge` and `buildCSRFromEdgeSource`
  became templates on the spec type instead of taking a base reference.
- **One flat variant, not a variant of variants.** `GraphSpec` lists all eight
  graph specs; the read/write distinction is a `.index()` comparison enforced at
  the `convert` / `partition` boundary. Nesting `variant<ReadSpec, WriteSpec>`
  would encode the split in the type, but pybind11 resolves a flat alternative
  list predictably and produces a readable union in the generated stubs.

`num_threads` and `sort_neighbors` are not parse options at all — the first is
used by the build, the sort and three of the four writers, and the second is a
post-build CSR transform. Both are parameters of `convert` / `partition`.

## Where direction lives (`BuiltGraph`)

Whether a CSR holds both directions of each edge is a property of the *data*, not
of the options used to write it. `BuiltGraph` pairs the CSR with that one bit, and
the writers consult it: a symmetric CSR emits one row per edge with `u<v`, a graph
stored as arcs only emits every arc, and METIS refuses the latter outright.

This closes a hole a write-side `directed` flag could not. Previously, reading with
`directed=true` and writing with `directed=false` silently dropped every arc with
`v<u` — on the `dnc` graph, 5236 of 10429. That state is now unrepresentable: the
only write-side knob is `expand_symmetric`, which adds the reverse arc of each
undirected edge and is a no-op on a graph that already stores arcs only.

`symmetric` is a pure function of the read spec, so it is known before the build
and the multi-output pre-validation stays all-or-nothing. Three of the four readers
determine it themselves; a CSR Parquet file records nothing about direction, so
`CsrParquetRead::symmetric` is the caller's declaration. Defaulting it to `true`
would silently reinstate the METIS hole for exactly the round trip most likely to
hit it, which is why it is a field rather than an assumption.

## What is memory-mapped, and for how long

Only the two text formats are mapped, and only for the duration of `buildGraph`:
the CSR is fully materialised into `HugeArray`s before the mapping is dropped, and
nothing retains a pointer into it. The Parquet readers open the file through Arrow
and never map it at all — previously they mapped it and then ignored the mapping.

`NodeDescriptor` is the exception and still maps at construction, held for the
object's lifetime: `NodeMap::file_data` points into it and `writeNodelist` reads
verbatim rows back out long after the build. That is the lifetime rule the
`shared_ptr` on the Python side exists for.

The visible cost is that a bad input path used to fail when the descriptor was
constructed. `GraphDescriptor` keeps that early error with an explicit readability
check for read specs, which is a `stat` rather than a mapping.

## Edge source → CSR build (`buildCSRFromEdgeSource`)

The degree-count / prefix-sum / scatter skeleton is format-agnostic: it takes a
`forEachStripe(t, T, cb)` callable that yields thread `t`'s share of the already
remapped and filtered edges. CSV supplies byte blocks, Parquet supplies row groups.
Two passes, parallel over `num_threads`, with **no atomics on the per-edge path**:

1. The input is tiled into units — 1 MB line-aligned blocks for CSV, row groups for
   Parquet. Thread `t` owns units `t, t+T, 2T, …` — a round-robin assignment that
   interleaves dense and sparse regions so one hub-heavy region can't stall a pass
   (social-network inputs are highly skewed). The assignment depends only on
   `(t, T)`, so it is identical in both passes.
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
`t` maps to the same thread (hence the same units and cursor row) across passes.

Scratch memory is `T × N × sizeof(O)`. This is the main scaling cost: at very
high thread counts and node counts it grows large (the path to ~60B edges would
want per-*block* rather than per-*thread* accumulation — deferred).

Every reader funnels its raw endpoint pairs through `mapRawEdge`, which applies
`base_index`, the narrowing check against `K`, the `NodeMap` lookup and the
self-loop rule. Keeping it in one place is what stops the filtering rules from
drifting apart between formats.

## Parquet edge list

A single file with two id columns (`source`/`target` by default, overridable);
other columns are never decoded because the reader passes explicit column indices
to `ReadRowGroup`.

**The file is decoded twice** — once to count degrees, once to scatter — rather
than decoded once into a buffer. Decoding twice is the more expensive option in
CPU, but buffering the decoded edge list would add `2 × E × sizeof(K)` resident
bytes, roughly doubling the edge-side footprint next to `edgeKeys` itself. At the
target scale (~5B edges) memory is the binding constraint, not CPU, so the
resident cost buys more than the decode does. The single-decode variant (with an
optional spill to a caller-chosen directory) is deferred until the double-decode
cost has actually been measured on a real graph.

Each thread opens **its own `parquet::arrow::FileReader`**: concurrent
`ReadRowGroup` on a shared reader is not documented as thread-safe. A file with
one row group therefore reads serially no matter the thread count.

Dense mode (no node list) needs `max(id)+1` up front, and row-group statistics
carry it in the footer for no decode at all. They are only *usable*, though, when
they cannot disagree with a scan: statistics describe every row, whereas a scan
sees only the edges that survive filtering, so the two differ whenever the largest
id appears solely on a dropped edge. In dense mode an edge is dropped only by the
self-loop rule or a `base_index` underflow, so the footer is taken as
authoritative exactly when `keep_self_loops` is set and `base_index` is zero, and
otherwise `N` is discovered by scanning — which is what the CSV path does.

Without that guard the same file yields a different `N` depending on whether its
*writer* happened to record statistics, which is not a property the reader should
be sensitive to. The cost is that the default configuration pays one extra decode
pass in dense mode; with a node list, `N` comes from the node list and no such
pass exists either way.

Note also that Parquet has no unsigned physical types — a `uint32` column is
stored as `INT32` with an unsigned *logical* type — so the statistic is typed on
the physical type and must be **reinterpreted** rather than sign-extended, or ids
above `2^31` come back negative.

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

`K` (index type) caps node ids at `2^(8·sizeof(K))`. `mapRawEdge` checks this when
narrowing a parsed 64-bit id to `K` (compiled out for `K=uint64`) and raises rather
than silently truncating.

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
their native `K` width (uint32 by default), not widened to uint64. Requesting
`u64_indices` is the one case that cannot be zero-copy, and it streams rather
than copying (see below).

These files are the long-term on-disk form, tuned for size:

- **No dictionary** — indices are high-cardinality; at large `N` the index codes
  are barely narrower than the raw values, so a dictionary is overhead.
- **Delta (`DELTA_BINARY_PACKED`) + zstd** — `indptr` is monotonic and collapses
  to its gaps (degrees). `indices` benefit from delta's frame-of-reference, and
  more so when adjacency is sorted (`sort_neighbors`); on highly
  skewed (power-law) graphs the hub lists compress well. The gain is degree- and
  distribution-dependent — modest on near-uniform low-degree graphs, larger on
  social-network-style inputs.
- **Statistics kept** — cheap, and enable reader-side page skipping.

For a low-latency / zero-copy consumer, convert these to Feather/Arrow IPC, which
is the throughput-oriented format; that conversion is standard and need not be
optimised here.

## Writing columns that do not exist in memory (`writeParquetIdColumns`)

Zero-copy works only when a column already *is* a contiguous array. Two outputs
are not:

- `u64_indices` with `K = uint32`, where the widened values exist nowhere;
- the Parquet edge list, whose `source`/`target` columns are a re-derivation of
  the CSR and are not stored anywhere in that shape.

Both stream instead: `fillChunk(row_begin, row_end, bufs)` materialises a window
into reused scratch, which is wrapped zero-copy and handed to
`parquet::arrow::FileWriter::WriteRecordBatch`. Peak scratch is
`PARQUET_CHUNK_ROWS × ncols × sizeof(Id)` (~16 MB for a two-column uint64 write)
rather than a whole second copy of `edgeKeys`.

Chunk size is *not* row-group size. `WriteRecordBatch` accumulates batches into a
row group up to `max_row_group_length`, so writing 1Mi-row chunks under a 16Mi-row
group limit still produces 16Mi-row groups — the on-disk layout is unchanged from
the single `WriteTable` this replaced.

The edge-list writer places rows the same way `writeLinesMmap` places bytes: count
rows per vertex, prefix-sum to absolute positions, fill in parallel. Undirected
counts only `v > u` (matching the CSV writer's dedup); directed emits every arc,
so `g.offsets` already *is* the row mapping and no counting pass is needed. A
vertex whose rows straddle a chunk boundary is visited from both chunks and writes
only the part inside the current one. The file itself is written serially — one
`FileWriter`, no concurrent row-group writes.

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
- The Parquet edge list decodes twice. A single-decode read, buffering the decoded
  edges in memory or spilling them to a caller-chosen directory, is deferred until
  the double-decode cost is measured on a real graph.
