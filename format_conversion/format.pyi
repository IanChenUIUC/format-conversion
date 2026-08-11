from __future__ import annotations
import collections.abc
import os
import typing

__all__: list[str] = ['CsrParquet', 'CsvEdgelist', 'EdgelistParquet', 'GraphDescriptor', 'Labels', 'Metis', 'NodeDescriptor', 'Nodelist', 'convert', 'partition']

class CsvEdgelist:
    class Read:
        sep: str
        comment_char: str
        keep_self_loops: bool
        directed: bool
        def __init__(self, *, sep: str = ',', comment_char: str = '#', skip_rows: typing.SupportsInt = 1, base_index: typing.SupportsInt = 0, keep_self_loops: bool = False, directed: bool = False) -> None:
            ...
        @property
        def skip_rows(self) -> int:
            ...
        @skip_rows.setter
        def skip_rows(self, arg0: typing.SupportsInt) -> None:
            ...
        @property
        def base_index(self) -> int:
            ...
        @base_index.setter
        def base_index(self, arg0: typing.SupportsInt) -> None:
            ...
    class Write:
        sep: str
        source_col: str
        target_col: str
        header: bool
        expand_symmetric: bool
        def __init__(self, *, sep: str = ',', source_col: str = 'source', target_col: str = 'target', base_index: typing.SupportsInt = 0, header: bool = True, expand_symmetric: bool = False) -> None:
            ...
        @property
        def base_index(self) -> int:
            ...
        @base_index.setter
        def base_index(self, arg0: typing.SupportsInt) -> None:
            ...

class Metis:
    class Read:
        comment_char: str
        def __init__(self, *, comment_char: str = '#', base_index: typing.SupportsInt = 1) -> None:
            ...
        @property
        def base_index(self) -> int:
            ...
        @base_index.setter
        def base_index(self, arg0: typing.SupportsInt) -> None:
            ...
    class Write:
        def __init__(self, *, base_index: typing.SupportsInt = 1) -> None:
            ...
        @property
        def base_index(self) -> int:
            ...
        @base_index.setter
        def base_index(self, arg0: typing.SupportsInt) -> None:
            ...

class CsrParquet:
    class Read:
        indices_col: str
        indptr_col: str
        symmetric: bool
        def __init__(self, *, indices_col: str = 'indices', indptr_col: str = 'indptr', base_index: typing.SupportsInt = 0, symmetric: bool = True) -> None:
            ...
        @property
        def base_index(self) -> int:
            ...
        @base_index.setter
        def base_index(self, arg0: typing.SupportsInt) -> None:
            ...
    class Write:
        indices_col: str
        indptr_col: str
        u64_indices: bool
        def __init__(self, *, indices_col: str = 'indices', indptr_col: str = 'indptr', base_index: typing.SupportsInt = 0, u64_indices: bool = False) -> None:
            ...
        @property
        def base_index(self) -> int:
            ...
        @base_index.setter
        def base_index(self, arg0: typing.SupportsInt) -> None:
            ...

class EdgelistParquet:
    class Read:
        source_col: str
        target_col: str
        keep_self_loops: bool
        directed: bool
        def __init__(self, *, source_col: str = 'source', target_col: str = 'target', base_index: typing.SupportsInt = 0, keep_self_loops: bool = False, directed: bool = False) -> None:
            ...
        @property
        def base_index(self) -> int:
            ...
        @base_index.setter
        def base_index(self, arg0: typing.SupportsInt) -> None:
            ...
    class Write:
        source_col: str
        target_col: str
        u64_ids: bool
        expand_symmetric: bool
        def __init__(self, *, source_col: str = 'source', target_col: str = 'target', base_index: typing.SupportsInt = 0, u64_ids: bool = False, expand_symmetric: bool = False) -> None:
            ...
        @property
        def base_index(self) -> int:
            ...
        @base_index.setter
        def base_index(self, arg0: typing.SupportsInt) -> None:
            ...

class Nodelist:
    class Csv:
        comment_char: str
        def __init__(self, *, comment_char: str = '#', skip_rows: typing.SupportsInt = 0, base_index: typing.SupportsInt = 0) -> None:
            ...
        @property
        def skip_rows(self) -> int:
            ...
        @skip_rows.setter
        def skip_rows(self, arg0: typing.SupportsInt) -> None:
            ...
        @property
        def base_index(self) -> int:
            ...
        @base_index.setter
        def base_index(self, arg0: typing.SupportsInt) -> None:
            ...

class Labels:
    class Csv:
        comment_char: str
        def __init__(self, *, comment_char: str = '#', skip_rows: typing.SupportsInt = 0) -> None:
            ...
        @property
        def skip_rows(self) -> int:
            ...
        @skip_rows.setter
        def skip_rows(self, arg0: typing.SupportsInt) -> None:
            ...

# The C++ side holds one flat variant over all eight specs; the read/write split is
# enforced at the convert()/partition() boundary.
ReadSpec: typing.TypeAlias = CsvEdgelist.Read | Metis.Read | CsrParquet.Read | EdgelistParquet.Read
WriteSpec: typing.TypeAlias = CsvEdgelist.Write | Metis.Write | CsrParquet.Write | EdgelistParquet.Write
GraphSpec: typing.TypeAlias = ReadSpec | WriteSpec

class NodeDescriptor:
    def __init__(self, path: os.PathLike | str | bytes, spec: Nodelist.Csv = ...) -> None:
        ...

class GraphDescriptor:
    def __init__(self, path: os.PathLike | str | bytes, spec: GraphSpec) -> None:
        ...
    @property
    def path(self) -> str:
        ...
    @property
    def spec(self) -> GraphSpec:
        ...

@typing.overload
def convert(input: GraphDescriptor, output: GraphDescriptor, *, nodes: NodeDescriptor | None = None, num_threads: typing.SupportsInt = 1, sort_neighbors: bool = False) -> None:
    ...
@typing.overload
def convert(input: GraphDescriptor, outputs: collections.abc.Sequence[GraphDescriptor], *, nodes: NodeDescriptor | None = None, num_threads: typing.SupportsInt = 1, sort_neighbors: bool = False) -> None:
    ...

def partition(input: GraphDescriptor, labels_path: os.PathLike | str | bytes, output_dir: os.PathLike | str | bytes, output_spec: GraphSpec, *, nodes: NodeDescriptor | None = None, label_spec: Labels.Csv = ..., num_threads: typing.SupportsInt = 1, sort_neighbors: bool = False, batch_size: typing.SupportsInt = 18446744073709551615) -> None:
    ...
