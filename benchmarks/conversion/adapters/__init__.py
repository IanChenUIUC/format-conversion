"""Tool adapters for the cross-library conversion benchmark.

Each adapter exposes a single `convert(spec) -> dict` entry point with a uniform
signature and return shape, so run_job.py can drive any tool the same way:

    convert(spec: ConvSpec) -> {"out_paths": [...], "out_format": "metis|csv|csr"}

where ConvSpec carries the source path/format, destination format, thread count,
graph metadata (nodes path, skip_rows, sep) and the tool-specific flags block.

format_conv additionally exposes `reference_degseq(spec)`, the canonical
relabeling-invariant degree-sequence hash every other tool is gated against.
"""
