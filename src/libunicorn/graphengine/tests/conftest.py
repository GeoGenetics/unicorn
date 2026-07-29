from __future__ import annotations

import os
from pathlib import Path
from tempfile import TemporaryDirectory


# Set runtime paths before test modules import server_app or tracing.
_TEST_RUNTIME = TemporaryDirectory(prefix="unicorn-graphengine-pytest-")
_RUNTIME_DIR = Path(_TEST_RUNTIME.name)

os.environ["UNICORN_GRAPHENGINE_RUNTIME_DIR"] = str(_RUNTIME_DIR)
os.environ["UNICORN_GRAPHENGINE_UPLOAD_DIR"] = str(_RUNTIME_DIR / "uploads")
os.environ["UNICORN_GRAPHENGINE_AGENT_TRACE_DIR"] = str(
    _RUNTIME_DIR / "logs" / "agent_trace"
)
