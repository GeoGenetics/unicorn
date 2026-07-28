from __future__ import annotations

import copy
from pathlib import Path

import pytest
from fastapi import HTTPException
from fastapi.responses import FileResponse

from unicorn_agent.routes import (
    PROVIDER_API_KEY_HEADER,
    create_agent_router,
)

from .helpers import valid_browser_turn_request


class RecordingOrchestrator:
    def __init__(self) -> None:
        self.calls: list[tuple[dict, str | None]] = []

    def run(
        self,
        request: dict,
        *,
        api_key: str | None = None,
    ) -> dict:
        self.calls.append((copy.deepcopy(request), api_key))
        return {
            "schema_version": "unicorn_agent_result_v1",
            "turn_id": request["turn_id"],
            "status": "completed",
            "answer": "Fixture answer.",
            "tools_used": [],
            "error": None,
            "trace_id": "trace_route_fixture",
        }


class RecordingTraceStore:
    def __init__(self, path: Path | None = None) -> None:
        self.path = path

    def trace_path(self, trace_id: str) -> Path:
        if self.path is None:
            raise KeyError(trace_id)
        return self.path


def _turn_endpoint(router):
    route = next(route for route in router.routes if route.path == "/agent/turn")
    assert route.methods == {"POST"}
    return route.endpoint


def _trace_endpoint(router):
    route = next(
        route
        for route in router.routes
        if route.path == "/agent/traces/{trace_id}"
    )
    assert route.methods == {"GET"}
    return route.endpoint


def test_router_exposes_post_turn_and_forwards_ephemeral_api_key() -> None:
    orchestrator = RecordingOrchestrator()
    router = create_agent_router(
        store=object(),
        registry=object(),
        orchestrator=orchestrator,
        trace_store=RecordingTraceStore(),
    )
    endpoint = _turn_endpoint(router)
    request = valid_browser_turn_request()
    secret = "fixture-route-secret"

    result = endpoint(request, f"  {secret}  ")

    assert result["status"] == "completed"
    assert orchestrator.calls == [(request, secret)]
    assert "api_key" not in orchestrator.calls[0][0]
    assert PROVIDER_API_KEY_HEADER == "X-Unicorn-Provider-API-Key"


def test_router_maps_contract_failure_to_structured_422() -> None:
    orchestrator = RecordingOrchestrator()
    endpoint = _turn_endpoint(
        create_agent_router(
            store=object(),
            registry=object(),
            orchestrator=orchestrator,
            trace_store=RecordingTraceStore(),
        )
    )
    request = valid_browser_turn_request()
    request["schema_version"] = "unicorn_agent_turn_v2"

    with pytest.raises(HTTPException) as caught:
        endpoint(request, None)

    assert caught.value.status_code == 422
    assert caught.value.detail["code"] == "contract_validation_error"
    assert caught.value.detail["contract"] == "browser_turn_request"
    assert orchestrator.calls == []


def test_router_maps_unexpected_failure_without_exposing_credentials() -> None:
    class FailingOrchestrator:
        def run(self, request, *, api_key=None):
            raise RuntimeError(f"provider rejected {api_key}")

    endpoint = _turn_endpoint(
        create_agent_router(
            store=object(),
            registry=object(),
            orchestrator=FailingOrchestrator(),
            trace_store=RecordingTraceStore(),
        )
    )
    secret = "fixture-route-secret"

    with pytest.raises(HTTPException) as caught:
        endpoint(valid_browser_turn_request(), secret)

    assert caught.value.status_code == 500
    assert caught.value.detail == {
        "code": "agent_internal_error",
        "message": "Agent turn failed at the backend boundary.",
    }
    assert secret not in str(caught.value.detail)


def test_router_serves_trace_inline_or_as_download(tmp_path: Path) -> None:
    trace_path = tmp_path / "turn_fixture.jsonl"
    trace_path.write_text('{"event":"turn_completed"}\n', encoding="utf-8")
    endpoint = _trace_endpoint(
        create_agent_router(
            store=object(),
            registry=object(),
            orchestrator=RecordingOrchestrator(),
            trace_store=RecordingTraceStore(trace_path),
        )
    )

    inline = endpoint("trace_fixture", False)
    download = endpoint("trace_fixture", True)

    assert isinstance(inline, FileResponse)
    assert Path(inline.path) == trace_path
    assert inline.headers["content-disposition"].startswith("inline;")
    assert download.headers["content-disposition"].startswith("attachment;")


@pytest.mark.parametrize(
    ("trace_id", "status_code", "code"),
    [
        ("not-a-trace", 422, "invalid_trace_id"),
        ("trace_missing", 404, "trace_not_found"),
    ],
)
def test_router_rejects_invalid_or_unknown_trace(
    trace_id: str,
    status_code: int,
    code: str,
) -> None:
    endpoint = _trace_endpoint(
        create_agent_router(
            store=object(),
            registry=object(),
            orchestrator=RecordingOrchestrator(),
            trace_store=RecordingTraceStore(),
        )
    )

    with pytest.raises(HTTPException) as caught:
        endpoint(trace_id, False)

    assert caught.value.status_code == status_code
    assert caught.value.detail["code"] == code


@pytest.mark.parametrize(
    "dependency",
    ["store", "registry", "orchestrator", "trace_store"],
)
def test_router_rejects_missing_dependencies(dependency: str) -> None:
    arguments = {
        "store": object(),
        "registry": object(),
        "orchestrator": RecordingOrchestrator(),
        "trace_store": RecordingTraceStore(),
    }
    arguments[dependency] = None

    with pytest.raises(ValueError, match=dependency):
        create_agent_router(**arguments)
