"""
Parallel graph filter C++ extension
"""
from __future__ import annotations
import typing
__all__: list[str] = ['partition']
def partition(nodelist_path: os.PathLike | str | bytes, labels_path: os.PathLike | str | bytes, metis_path: os.PathLike | str | bytes, out_dir: os.PathLike | str | bytes) -> None:
    """
    Filter a METIS graph into label-specific sub-edgelists
    """
