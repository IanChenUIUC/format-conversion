from __future__ import annotations
import typing
__all__: list[str] = ['CSR', 'ConvertOptions', 'METIS', 'OutputType', 'convert']
class ConvertOptions:
    edge_header: bool
    node_header: bool
    sep: str
    skip_loops: bool
    type: OutputType
    weighted: bool
    def __init__(self) -> None:
        ...
class OutputType:
    """
    Members:
    
      METIS
    
      CSR
    """
    CSR: typing.ClassVar[OutputType]  # value = <OutputType.CSR: 1>
    METIS: typing.ClassVar[OutputType]  # value = <OutputType.METIS: 0>
    __members__: typing.ClassVar[dict[str, OutputType]]  # value = {'METIS': <OutputType.METIS: 0>, 'CSR': <OutputType.CSR: 1>}
    @typing.overload
    def __eq__(self, other: OutputType) -> bool:
        ...
    @typing.overload
    def __eq__(self, other: typing.SupportsInt | typing.SupportsIndex) -> bool:
        ...
    @typing.overload
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    @typing.overload
    def __ne__(self, other: OutputType) -> bool:
        ...
    @typing.overload
    def __ne__(self, other: typing.SupportsInt | typing.SupportsIndex) -> bool:
        ...
    @typing.overload
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
def convert(nodes_file: os.PathLike | str | bytes, edges_file: os.PathLike | str | bytes, output_file: os.PathLike | str | bytes, opts: ConvertOptions = ...) -> None:
    ...
CSR: OutputType  # value = <OutputType.CSR: 1>
METIS: OutputType  # value = <OutputType.METIS: 0>
