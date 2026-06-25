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
from .helpers import read_metis, read_edgelist, read_csr_parquet

try:
    from format_conversion.format import EdgesFormat, ParseOptions, GraphDescriptor
    _LOADED = True
except ImportError:
    _LOADED = False


@dataclass
class Format:
    name:  str
    _fmt:  object                   # EdgesFormat value; kept as object for pre-import safety
    _read: Callable[[Path], frozenset]

    # ── Public API used by tests ──────────────────────────────────────────

    @property
    def fmt(self):
        return self._fmt

    def read(self, base: Path) -> frozenset:
        """Read edges from the output file(s) at base (without extension)."""
        return self._read(base)

    def as_input(self, base: Path, opts: "ParseOptions | None" = None) -> "GraphDescriptor":
        """GraphDescriptor for reading this format as convert/partition input.
        Appends the format-specific extension so the path matches what writeGraph wrote."""
        if not _LOADED:
            raise ImportError("C++ module not built")
        o = opts or ParseOptions()
        # Must match the extensions appended by writeGraph/writeGraphToX
        extensions = {
            EdgesFormat.METIS:        ".metis",
            EdgesFormat.CSV_EDGELIST: ".csv",
            EdgesFormat.CSR_PARQUET:  ".indices.parquet",
        }
        path = str(base) + extensions.get(self._fmt, "")
        return GraphDescriptor(path, self._fmt, o)

    def __repr__(self) -> str:
        return f"Format({self.name})"


# ── Registry ──────────────────────────────────────────────────────────────────
#
# _read callables receive the *base* output path (no extension).
# They are responsible for appending the right extension(s).

def _read_metis(base: Path) -> frozenset:
    return read_metis(Path(str(base) + ".metis"))

def _read_csv(base: Path) -> frozenset:
    return read_edgelist(Path(str(base) + ".csv"), header=False)

def _read_parquet(base: Path) -> frozenset:
    return read_csr_parquet(base)      # helper already knows the two-file convention


def _make_formats() -> dict[str, Format]:
    if not _LOADED:
        return {}
    return {
        "metis":   Format("metis",   EdgesFormat.METIS,        _read_metis),
        "csv":     Format("csv",     EdgesFormat.CSV_EDGELIST,  _read_csv),
        "parquet": Format("parquet", EdgesFormat.CSR_PARQUET,   _read_parquet),
    }


FORMATS: dict[str, Format] = _make_formats()
