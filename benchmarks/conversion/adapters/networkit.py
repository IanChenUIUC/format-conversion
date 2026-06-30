"""NetworKit adapter (lazy import; verified against networkit==11.2.1).

Uses NetworKit's reader/writer classes directly — EdgeListReader/EdgeListWriter
and METISGraphReader/METISGraphWriter — rather than graphio.convertGraph(), so
the comma separator and 0-indexing match format-conversion's own CSV_EDGELIST
exactly; no separate edge-list dialect is introduced.

Comparisons: csv->metis, metis->csv, both against a comma-separated, headerless,
compacted CSV (the prepared input both tools share; see run_job._prepare_source).

EdgeListReader's `continuous` flag is set False so it tolerates id gaps the same
way format-conversion's own CSV reader does, although the prepared input is
already compact. NetworKit's EdgeListReader has no header-skip option and
mis-parses a header row as a phantom edge between two string-labeled nodes, so
callers MUST pass an already-headerless CSV (never the raw source file).

networkit is imported lazily inside convert() so the rest of the harness (and
the format_conv-only smoke test) runs without networkit installed.
"""

from __future__ import annotations

SEP = ","       # matches format-conversion's CSV_EDGELIST separator
FIRST_NODE = 0  # both sides are 0-indexed


def convert(spec: dict) -> dict:
    import networkit as nk

    nk.setNumberOfThreads(int(spec["threads"]))

    if spec["src_format"] == "csv" and spec["dst_format"] == "metis":
        reader = nk.graphio.EdgeListReader(SEP, FIRST_NODE, "#", False, False)
        g = reader.read(str(spec["src_path"]))
        out_path = spec["out_prefix"] + ".metis"
        nk.graphio.METISGraphWriter().write(g, out_path)
        return {"out_paths": [out_path], "out_format": "metis"}

    if spec["src_format"] == "metis" and spec["dst_format"] == "csv":
        g = nk.graphio.METISGraphReader().read(str(spec["src_path"]))
        out_path = spec["out_prefix"] + ".csv"
        nk.graphio.EdgeListWriter(SEP, FIRST_NODE, False).write(g, out_path)
        return {"out_paths": [out_path], "out_format": "csv"}

    raise ValueError(f"networkit comparison does not cover {spec['conversion']!r}")
