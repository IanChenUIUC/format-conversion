"""
Format registry.

Each Format knows how to:
  - read back edges written by writeGraph (for test assertions)
  - produce a GraphDescriptor for use as convert/partition input

Adding a new format = one new Format(...) entry in FORMATS.
No test changes required.
"""

from __future__ import annotations
from dataclasses import dataclass
from pathlib import Path
from typing import Callable
from .helpers import read_metis, read_edgelist, read_csr_parquet, read_edgelist_parquet

try:
    import format_conversion.format as _fmt
    from format_conversion.format import GraphDescriptor
    _LOADED = True
except ImportError:
    _LOADED = False


@dataclass
class Format:
    name:       str
    extension:  str                             # appended by writeGraph/writeGraphToX
    _read:      Callable[[Path], frozenset]
    _read_spec: Callable[..., object]           # kwargs -> a *.Read spec
    _write_spec: Callable[..., object]          # kwargs -> a *.Write spec

    # ── Public API used by tests ──────────────────────────────────────────

    def read(self, base: Path) -> frozenset:
        """Read edges from the output file(s) at base (without extension)."""
        return self._read(base)

    def read_spec(self, **kw):
        """A read spec for this format, e.g. CsvEdgelist.Read(**kw)."""
        return self._read_spec(**kw)

    def write_spec(self, **kw):
        """A write spec for this format, e.g. CsvEdgelist.Write(**kw)."""
        return self._write_spec(**kw)

    def as_input(self, base: Path, **kw) -> "GraphDescriptor":
        """GraphDescriptor for reading this format as convert/partition input.
        Appends the format-specific extension so the path matches what was written."""
        if not _LOADED:
            raise ImportError("C++ module not built")
        return GraphDescriptor(str(base) + self.extension, self.read_spec(**kw))

    def as_output(self, base: Path, **kw) -> "GraphDescriptor":
        """GraphDescriptor for writing this format. The path is a prefix."""
        if not _LOADED:
            raise ImportError("C++ module not built")
        return GraphDescriptor(str(base), self.write_spec(**kw))

    def __repr__(self) -> str:
        return f"Format({self.name})"


# ── Registry ──────────────────────────────────────────────────────────────────
#
# _read callables receive the *base* output path (no extension).
# They are responsible for appending the right extension(s).

def _read_metis(base: Path) -> frozenset:
    return read_metis(Path(str(base) + ".metis"))

def _read_csv(base: Path) -> frozenset:
    return read_edgelist(Path(str(base) + ".csv"), header=True)

def _read_parquet(base: Path) -> frozenset:
    return read_csr_parquet(base)      # helper already knows the two-file convention

def _read_edgelist_parquet(base: Path) -> frozenset:
    return read_edgelist_parquet(Path(str(base) + ".parquet"))


def _make_formats() -> dict[str, Format]:
    if not _LOADED:
        return {}
    return {
        "metis":   Format("metis", ".metis", _read_metis,
                          _fmt.Metis.Read, _fmt.Metis.Write),
        "csv":     Format("csv", ".csv", _read_csv,
                          _fmt.CsvEdgelist.Read, _fmt.CsvEdgelist.Write),
        "parquet": Format("parquet", ".indices.parquet", _read_parquet,
                          _fmt.CsrParquet.Read, _fmt.CsrParquet.Write),
        "edgelist_parquet": Format("edgelist_parquet", ".parquet", _read_edgelist_parquet,
                                   _fmt.EdgelistParquet.Read, _fmt.EdgelistParquet.Write),
    }


FORMATS: dict[str, Format] = _make_formats()
