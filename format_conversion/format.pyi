from __future__ import annotations
import typing
__all__: list[str] = ['CSR_PARQUET', 'CSV_EDGELIST', 'EdgesFormat', 'GraphDescriptor', 'METIS', 'NodeDescriptor', 'ParseOptions', 'convert', 'partition']
class EdgesFormat:
    """
    Members:
    
      CSV_EDGELIST
    
      METIS
    
      CSR_PARQUET
    """
    CSR_PARQUET: typing.ClassVar[EdgesFormat]  # value = <EdgesFormat.CSR_PARQUET: 2>
    CSV_EDGELIST: typing.ClassVar[EdgesFormat]  # value = <EdgesFormat.CSV_EDGELIST: 0>
    METIS: typing.ClassVar[EdgesFormat]  # value = <EdgesFormat.METIS: 1>
    __members__: typing.ClassVar[dict[str, EdgesFormat]]  # value = {'CSV_EDGELIST': <EdgesFormat.CSV_EDGELIST: 0>, 'METIS': <EdgesFormat.METIS: 1>, 'CSR_PARQUET': <EdgesFormat.CSR_PARQUET: 2>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class GraphDescriptor:
    def __init__(self, path: os.PathLike | str | bytes, fmt: EdgesFormat, opts: ParseOptions = ...) -> None:
        ...
class NodeDescriptor:
    def __init__(self, path: os.PathLike | str | bytes, opts: ParseOptions = ...) -> None:
        ...
class ParseOptions:
    comment_char: str
    sep: str
    def __init__(self) -> None:
        ...
    @property
    def base_index(self) -> int:
        ...
    @base_index.setter
    def base_index(self, arg0: typing.SupportsInt) -> None:
        ...
    @property
    def id_column(self) -> int:
        ...
    @id_column.setter
    def id_column(self, arg0: typing.SupportsInt) -> None:
        ...
    @property
    def label_column(self) -> int:
        ...
    @label_column.setter
    def label_column(self, arg0: typing.SupportsInt) -> None:
        ...
    @property
    def num_threads(self) -> int:
        ...
    @num_threads.setter
    def num_threads(self, arg0: typing.SupportsInt) -> None:
        ...
    @property
    def skip_rows(self) -> int:
        ...
    @skip_rows.setter
    def skip_rows(self, arg0: typing.SupportsInt) -> None:
        ...
def convert(input: GraphDescriptor, nodes: NodeDescriptor = None, output_path: os.PathLike | str | bytes, output_fmt: EdgesFormat) -> None:
    ...
def partition(input: GraphDescriptor, nodes: NodeDescriptor = None, labels_path: os.PathLike | str | bytes, output_dir: os.PathLike | str | bytes, output_fmt: EdgesFormat, label_opts: ParseOptions = ..., batch_size: typing.SupportsInt = 18446744073709551615) -> None:
    ...
CSR_PARQUET: EdgesFormat  # value = <EdgesFormat.CSR_PARQUET: 2>
CSV_EDGELIST: EdgesFormat  # value = <EdgesFormat.CSV_EDGELIST: 0>
METIS: EdgesFormat  # value = <EdgesFormat.METIS: 1>
