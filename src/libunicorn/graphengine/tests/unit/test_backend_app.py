from __future__ import annotations

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

import server_app


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
    ("/expand-node", frozenset({"GET"})),
    ("/node-tooltip", frozenset({"GET"})),
    ("/table-view", frozenset({"GET"})),
    ("/subtree-report", frozenset({"GET"})),
    ("/subtree-report", frozenset({"POST"})),
    ("/rank-report", frozenset({"GET"})),
    ("/rank-report", frozenset({"POST"})),
    ("/compute/barplot", frozenset({"POST"})),
    ("/uncollapse-to-tips", frozenset({"POST"})),
    ("/compute/pcoa", frozenset({"POST"})),
}


def _application_routes() -> set[tuple[str, frozenset[str]]]:
    documentation_paths = {
        "/openapi.json",
        "/docs",
        "/docs/oauth2-redirect",
        "/redoc",
    }
    return {
        (route.path, frozenset(route.methods or set()))
        for route in server_app.app.routes
        if route.path not in documentation_paths
    }


def test_backend_application_identity_and_middleware_are_frozen() -> None:
    assert isinstance(server_app.app, FastAPI)
    assert server_app.app.title == "Unicorn Graph Engine Prototype API"
    assert any(
        middleware.cls is CORSMiddleware
        for middleware in server_app.app.user_middleware
    )


def test_backend_route_inventory_is_frozen() -> None:
    assert _application_routes() == EXPECTED_ROUTES


def test_agent_runtime_shares_the_application_store() -> None:
    assert server_app.AGENT_RUNTIME._store is server_app.STORE

