from __future__ import annotations

from server_app import app


def test_server_registers_only_the_new_agent_and_trace_routes() -> None:
    agent_routes = {
        route.path: route.methods
        for route in app.routes
        if route.path.startswith("/agent/")
    }

    assert agent_routes == {
        "/agent/turn": {"POST"},
        "/agent/traces/{trace_id}": {"GET"},
    }
