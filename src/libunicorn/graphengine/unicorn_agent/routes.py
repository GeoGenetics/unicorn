"""Expose the Agent API without leaking its implementation into server_app."""

from __future__ import annotations

import re
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol

from fastapi import APIRouter, Body, Header, HTTPException, Query
from fastapi.responses import FileResponse

from unicorn_agent.contracts import (
    ContractValidationError,
    validate_browser_turn_request,
)


PROVIDER_API_KEY_HEADER = "X-Unicorn-Provider-API-Key"
_TRACE_ID = re.compile(r"^trace_[A-Za-z0-9._-]+$")


class AgentTurnRunner(Protocol):
    """Narrow orchestrator surface owned by the HTTP boundary."""

    def run(
        self,
        request: Mapping[str, Any],
        *,
        api_key: str | None = None,
    ) -> dict[str, Any]: ...


class AgentTraceReader(Protocol):
    """Narrow persistent-trace access required by the HTTP boundary."""

    def trace_path(self, trace_id: str) -> Path: ...


@dataclass(frozen=True)
class AgentRouterDependencies:
    """Explicit references retained by the router composition boundary."""

    store: Any
    registry: Any
    orchestrator: AgentTurnRunner
    trace_store: AgentTraceReader


def create_agent_router(
    *,
    store: Any,
    registry: Any,
    orchestrator: AgentTurnRunner,
    trace_store: AgentTraceReader,
) -> APIRouter:
    """Create the V1 Agent router from explicitly injected dependencies."""

    dependencies = _validated_dependencies(
        store=store,
        registry=registry,
        orchestrator=orchestrator,
        trace_store=trace_store,
    )
    router = APIRouter(prefix="/agent", tags=["agent"])

    @router.post("/turn")
    def run_agent_turn(
        payload: dict[str, Any] = Body(...),
        provider_api_key: str | None = Header(
            default=None,
            alias=PROVIDER_API_KEY_HEADER,
            include_in_schema=False,
        ),
    ) -> dict[str, Any]:
        api_key = _normalize_api_key(provider_api_key)
        try:
            validate_browser_turn_request(payload)
            return dependencies.orchestrator.run(
                payload,
                api_key=api_key,
            )
        except ContractValidationError as error:
            raise HTTPException(
                status_code=422,
                detail=error.to_dict(),
            ) from None
        except Exception:
            raise HTTPException(
                status_code=500,
                detail={
                    "code": "agent_internal_error",
                    "message": "Agent turn failed at the backend boundary.",
                },
            ) from None

    @router.get("/traces/{trace_id}")
    def read_agent_trace(
        trace_id: str,
        download: bool = Query(default=False),
    ) -> FileResponse:
        if not _TRACE_ID.fullmatch(trace_id):
            raise HTTPException(
                status_code=422,
                detail={
                    "code": "invalid_trace_id",
                    "message": "Agent trace ID does not satisfy the V1 format.",
                },
            )
        try:
            path = dependencies.trace_store.trace_path(trace_id)
        except KeyError:
            raise HTTPException(
                status_code=404,
                detail={
                    "code": "trace_not_found",
                    "message": f"Agent trace was not found: {trace_id}",
                },
            ) from None
        if not path.is_file():
            raise HTTPException(
                status_code=404,
                detail={
                    "code": "trace_not_found",
                    "message": f"Agent trace file was not found: {trace_id}",
                },
            )
        disposition = "attachment" if download else "inline"
        return FileResponse(
            path,
            media_type="application/x-ndjson",
            headers={
                "Content-Disposition": (
                    f'{disposition}; filename="{path.name}"'
                ),
            },
        )

    return router


def _validated_dependencies(
    *,
    store: Any,
    registry: Any,
    orchestrator: AgentTurnRunner,
    trace_store: AgentTraceReader,
) -> AgentRouterDependencies:
    missing = [
        name
        for name, value in (
            ("store", store),
            ("registry", registry),
            ("orchestrator", orchestrator),
            ("trace_store", trace_store),
        )
        if value is None
    ]
    if missing:
        names = ", ".join(missing)
        raise ValueError(f"Agent router dependencies must not be None: {names}.")
    if not callable(getattr(orchestrator, "run", None)):
        raise TypeError("Agent router orchestrator must provide run().")
    if not callable(getattr(trace_store, "trace_path", None)):
        raise TypeError("Agent router trace_store must provide trace_path().")
    return AgentRouterDependencies(
        store=store,
        registry=registry,
        orchestrator=orchestrator,
        trace_store=trace_store,
    )


def _normalize_api_key(value: str | None) -> str | None:
    if value is None:
        return None
    stripped = value.strip()
    return stripped or None
