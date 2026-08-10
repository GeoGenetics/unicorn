from __future__ import annotations

from pathlib import Path

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

import unicorn_backend.app as backend_app
from unicorn_backend.config import BackendConfig


EXPECTED_ROUTES = {
    ("/agent/turn", frozenset({"POST"})),
    ("/agent/traces/{trace_id}", frozenset({"GET"})),
    ("/ping", frozenset({"GET"})),
    ("/upload", frozenset({"POST"})),
    ("/metadata/upload", frozenset({"POST"})),
    ("/metadata/status", frozenset({"GET"})),
    ("/datasets", frozenset({"GET"})),
    ("/taxonomy/status", frozenset({"GET"})),
    ("/model/status", frozenset({"GET"})),
    ("/render-data", frozenset({"GET"})),
    ("/tree-model", frozenset({"GET"})),
    ("/root-view", frozenset({"GET"})),
    ("/tree-view", frozenset({"POST"})),
    ("/expand-node", frozenset({"GET"})),
    ("/node-tooltip", frozenset({"GET"})),
    ("/damage/node", frozenset({"POST"})),
    ("/damage/selected", frozenset({"POST"})),
    ("/table-view", frozenset({"GET"})),
    ("/subtree-report", frozenset({"GET"})),
    ("/subtree-report", frozenset({"POST"})),
    ("/rank-report", frozenset({"GET"})),
    ("/rank-report", frozenset({"POST"})),
    ("/compute/barplot", frozenset({"POST"})),
    ("/uncollapse-to-tips", frozenset({"POST"})),
    ("/compute/pcoa", frozenset({"POST"})),
}


def _application_routes(
    application: FastAPI = backend_app.app,
) -> set[tuple[str, frozenset[str]]]:
    documentation_paths = {
        "/openapi.json",
        "/docs",
        "/docs/oauth2-redirect",
        "/redoc",
    }
    return {
        (route.path, frozenset(route.methods or set()))
        for route in application.routes
        if route.path not in documentation_paths
    }


def _test_config(tmp_path: Path, name: str) -> BackendConfig:
    runtime_dir = tmp_path / name
    upload_dir = runtime_dir / "uploads"
    return BackendConfig(
        host="127.0.0.1",
        port=8000,
        runtime_dir=runtime_dir,
        upload_dir=upload_dir,
        agent_trace_dir=runtime_dir / "logs" / "agent_trace",
        nodes_filename="nodes.dmp",
        names_filename="names.dmp",
        metadata_filename="metadata.txt",
    )


def test_backend_application_identity_and_middleware_are_frozen() -> None:
    assert isinstance(backend_app.app, FastAPI)
    assert backend_app.app.title == "Unicorn Graph Engine Prototype API"
    assert any(
        middleware.cls is CORSMiddleware
        for middleware in backend_app.app.user_middleware
    )


def test_backend_route_inventory_is_frozen() -> None:
    assert _application_routes() == EXPECTED_ROUTES


def test_agent_runtime_shares_the_application_store() -> None:
    assert (
        backend_app.app.state.agent_runtime._store
        is backend_app.app.state.graphengine_store
    )


def test_create_app_builds_isolated_dependency_graphs(tmp_path: Path) -> None:
    first_config = _test_config(tmp_path, "first")
    second_config = _test_config(tmp_path, "second")

    first_app = backend_app.create_app(first_config)
    second_app = backend_app.create_app(second_config)

    assert first_app is not second_app
    assert first_app.state.backend_config is first_config
    assert second_app.state.backend_config is second_config
    assert (
        first_app.state.graphengine_store
        is not second_app.state.graphengine_store
    )
    assert first_app.state.graphengine_store.config is first_config
    assert second_app.state.graphengine_store.config is second_config
    assert (
        first_app.state.agent_runtime._store
        is first_app.state.graphengine_store
    )
    assert (
        second_app.state.agent_runtime._store
        is second_app.state.graphengine_store
    )
    assert (
        first_app.state.agent_runtime.trace_store.root_dir
        == first_config.agent_trace_dir
    )
    assert (
        second_app.state.agent_runtime.trace_store.root_dir
        == second_config.agent_trace_dir
    )
    assert first_config.upload_dir.is_dir()
    assert second_config.upload_dir.is_dir()
    assert not first_config.agent_trace_dir.exists()
    assert not second_config.agent_trace_dir.exists()
    assert _application_routes(first_app) == EXPECTED_ROUTES
    assert _application_routes(second_app) == EXPECTED_ROUTES


def test_backend_app_does_not_import_legacy_server() -> None:
    source = Path(backend_app.__file__).read_text(encoding="utf-8")

    assert "server_app" not in source


def test_non_agent_routes_are_owned_by_backend_router_modules() -> None:
    for route in backend_app.app.routes:
        if route.path.startswith(("/docs", "/redoc", "/openapi.json")):
            continue
        if route.path.startswith("/agent/"):
            continue
        assert route.endpoint.__module__.startswith(
            "unicorn_backend.routers."
        )
