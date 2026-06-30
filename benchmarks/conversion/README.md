# cross-library conversion benchmark

Wall-clock, peak-RSS, and output-size comparison of `format-conversion` against
other graph-format converters, on a SLURM array. Each measurement is gated by a
relabeling-invariant correctness check so only *correct* conversions are
compared.

## What is compared

The design follows a strict **shared-format rule**: a tool is only compared on a
conversion where both the input and output formats are formats that tool natively
reads/writes, so no tool is penalised for a format it was never meant to handle.

| comparison  | tools                  | conversions                
|---          |---                     |---                         
| `networkit` | format_conv, networkit | `csv->metis`, `metis->csv` 
| `icebug`    | format_conv, icebug    | `csv->csr`                 

`format_conv` appears in every comparison and is also the **reference oracle**:
its CSR build defines the canonical degree sequence every tool is checked against.

The networkit adapter uses NetworKit's `EdgeListReader`/`EdgeListWriter` and
`METISGraphReader`/`METISGraphWriter` classes directly (not
`graphio.convertGraph()`), configured with `separator=","`, `firstNode=0` to
match format-conversion's own CSV_EDGELIST exactly — no separate edge-list
dialect is introduced. NetworKit's `EdgeListReader` has no header-skip option
and would mis-parse a header line as a phantom edge, so for the `csv->metis` /
`metis->csv` comparison both tools read a shared, prepared, headerless,
compacted CSV (and a shared prepared METIS file), built once from the real
source via `format_conv` — untimed setup, identical input for both tools.

Tools that fail the shared-format rule for this pairwise design (gapbs,
NetworKitBinary) are intentionally excluded; they can be revisited if a pure
C++ speed yardstick is wanted.

## Correctness gate

`correctness.py` reduces each output to its **sorted non-zero degree sequence**
and hashes it. This is invariant to node relabeling (internal id ordering differs
across tools) but still catches dropped/duplicated edges, mishandled self-loops,
and missing reverse edges. Zero-degree (isolated) vertices are dropped before
hashing because an edge-list format cannot represent them while metis/CSR can;
dropping them keeps the gate consistent across all three formats. `degseq_ok=0`
in the results flags a conversion that produced a structurally different graph;
aggregate.py warns on any such row.

The reference is always built from the original CSV via `format_conv`, before the
timed region, so it never counts against the tool under test.

## Metrics

- **wall_s** — tight `perf_counter` around the adapter's `convert()` call only
  (excludes process start-up, imports, and input preparation), measured inside
  the worker.
- **peak_rss_kb** — from `/usr/bin/time -v` when present (cluster), else the
  worker's own `resource.getrusage(RUSAGE_SELF).ru_maxrss`.
- **out_bytes** — total on-disk size of the output file(s).

Each job runs **two trials**: trial 1 warms caches, trial 2 is the measurement.
Both run in the same SLURM job (same node), each in a fresh worker subprocess so
the heap/page-cache state of one trial does not leak into the next.

## Files

```
config.toml        graphs, thread levels, comparisons, tool flags
gen_jobs.py        config.toml -> jobs.csv (+ prints the sbatch invocation)
run_job.py         --job-line N: run both trials -> results/<N>.csv
                   --worker:     (internal) one isolated, timed conversion
aggregate.py       results/*.csv -> results.csv (+ correctness warnings)
metrics.py         /usr/bin/time -v parsing, wall timing, output sizing
correctness.py     degree-sequence extractors + relabeling-invariant hash
slurm_job.sh       SLURM array template (edit #SBATCH headers per cluster)
adapters/
  format_conv.py   our convert() wrapper + the reference degree sequence
  networkit.py     nk.graphio.convertGraph; nk.setNumberOfThreads (lazy import)
  icebug.py        DuckDB ingest + icebug-format CLI (lazy import)
```

## jobs.csv / results.csv schemas

```
# jobs.csv
job_id,comparison,tool,conversion,threads,graph,graph_path,graph_format,graph_nodes,graph_skip_rows

# results.csv (two rows per job: trial=1 warmup, trial=2 measure)
job_id,comparison,tool,conversion,threads,trial,graph,n,m,wall_s,peak_rss_kb,out_bytes,degseq_ok
```

## Running

### Locally (smoke test, format_conv only — no networkit/icebug needed)

```bash
cd benchmarks/conversion
python gen_jobs.py --tool format_conv -o jobs.csv
for N in $(seq 1 $(($(wc -l < jobs.csv) - 1))); do
    python run_job.py --job-line "$N"
done
python aggregate.py
cat results.csv
```

### On a cluster

```bash
cd benchmarks/conversion
# install the benchmark tools (networkit + icebug-format + duckdb) into the env:
#   uv sync --group benchmarks      # from the repo root
# point BENCH_VENV at that env, then:
python gen_jobs.py -o jobs.csv        # prints: sbatch --array=1-<N> slurm_job.sh
sbatch --array=1-<N> slurm_job.sh
# after the array finishes:
python aggregate.py
```

Set `--cpus-per-task` in `slurm_job.sh` to at least the largest `threads` level
in `config.toml`. `numactl --interleave=all` is applied automatically when
available (see the project DESIGN.md for why interleaving suits the large
random-access `edgeKeys` array).

## Notes

- `csv->metis` / `metis->csv` operate on shared prepared inputs: the worker
  builds a headerless, compacted CSV and a METIS file once from the source CSV
  via `format_conv` (untimed). Both tools then read the identical prepared
  file — this is needed because NetworKit's `EdgeListReader` has no header-skip
  option (it would parse a header line as a phantom edge) and the real source
  has gapped ids.
- icebug (verified against `icebug-format==1.0.0`) writes the CSR into an output
  DuckDB and also exports Parquet to a directory named after that db
  (`<name>_out.duckdb` → `<name>_out/indices_<name>.parquet`,
  `indptr_<name>.parquet`, columns `target`/`ptr`, both `uint64`). The CLI is
  resolved next to the running interpreter, so it works even when `.venv/bin` is
  not on `PATH`. icebug exposes no thread knob, so the `threads` column is
  provenance only for that tool. For `csv->csr`, both tools read the real source
  CSV directly (format_conv via `skip_rows` + a node list; icebug via its own
  header-aware DuckDB ingest).
- icebug emits uint64 CSR indices, so `config.toml` sets
  `use_u64_indices_for_csr = true` for `format_conv` in that comparison to keep
  `out_bytes` an apples-to-apples comparison (this is what the Track 9a
  `use_u64_indices` option was added for).
