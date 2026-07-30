from __future__ import annotations

import sys
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

import server_app
import unicorn_backend.app as backend_app


def test_historical_uvicorn_target_reexports_factory_app() -> None:
    assert server_app.app is backend_app.app


def test_compatibility_entrypoint_does_not_own_backend_dependencies() -> None:
    assert not hasattr(server_app, "STORE")
    assert not hasattr(server_app, "AGENT_RUNTIME")
    assert not hasattr(server_app, "BACKEND_CONFIG")

    source = Path(server_app.__file__).read_text(encoding="utf-8")
    for forbidden in (
        "FastAPI(",
        "GraphEngineStore",
        "create_agent_runtime",
        "include_router",
        "@app.get(",
        "@app.post(",
    ):
        assert forbidden not in source


def test_direct_script_main_uses_factory_configuration(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    calls: list[dict[str, Any]] = []

    def fake_run(application, *, host: str, port: int) -> None:
        calls.append({
            "application": application,
            "host": host,
            "port": port,
        })

    monkeypatch.setitem(
        sys.modules,
        "uvicorn",
        SimpleNamespace(run=fake_run),
    )

    server_app.main()

    config = backend_app.app.state.backend_config
    assert calls == [{
        "application": backend_app.app,
        "host": config.host,
        "port": config.port,
    }]
