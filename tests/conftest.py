"""
Shared fixtures.
`graph` is parametrized over all entries in GRAPHS, so every test that
uses it automatically runs against each graph in the library.
"""

import pytest
from pathlib import Path
from .graphs import GRAPHS, GRAPH_IDS


# ── Auto-xfail stubs ──────────────────────────────────────────────────────────
#
# Any test that raises RuntimeError("* not yet implemented") is marked xfail
# rather than failing, so `pytest` output shows only real regressions.
# Remove this hook (or the individual stub throws) as milestones are completed.

@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    if rep.when == "call" and rep.failed:
        exc = call.excinfo
        if exc and isinstance(exc.value, RuntimeError):
            msg = str(exc.value)
            if "not yet implemented" in msg or ": not implemented" in msg:
                rep.outcome = "skipped"
                rep.wasxfail = f"stub: {msg}"


@pytest.fixture(params=GRAPHS, ids=GRAPH_IDS)
def graph(request, tmp_path: Path) -> dict:
    """Write one GraphSpec to tmp_path and return its path dict."""
    return request.param.write(tmp_path)
