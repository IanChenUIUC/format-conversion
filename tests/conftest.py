"""
Shared fixtures.
`graph` is parametrized over all entries in GRAPHS, so every test that
uses it automatically runs against each graph in the library.
"""

import pytest
from pathlib import Path
from .graphs import GRAPHS, GRAPH_IDS


@pytest.fixture(params=GRAPHS, ids=GRAPH_IDS)
def graph(request, tmp_path: Path) -> dict:
    """Write one GraphSpec to tmp_path and return its path dict."""
    return request.param.write(tmp_path)
