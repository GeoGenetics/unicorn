"""Expose the Agent API without leaking its implementation into server_app."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any, Protocol

from fastapi import APIRouter, Body, Header, HTTPException

from unicorn_agent.contracts import (
    ContractValidationError,
    validate_browser_turn_request,
)


PROVIDER_API_KEY_HEADER = "X-Unicorn-Provider-API-Key"


class AgentTurnRunner(Protocol):
    """Narrow orchestrator surface owned by the HTTP boundary."""

    def run(
        self,
        request: Mapping[str, Any],
        *,
        api_key: str | None = None,
    ) -> dict[str, Any]: ...


@dataclass(frozen=True)
class AgentRouterDependencies:
    """Explicit references retained by the router composition boundary."""

    store: Any
    registry: Any
    orchestrator: AgentTurnRunner


def create_agent_router(
    *,
    store: Any,
    registry: Any,
    orchestrator: AgentTurnRunner,
) -> APIRouter:
    """Create the V1 Agent router from explicitly injected dependencies."""

    dependencies = _validated_dependencies(
        store=store,
        registry=registry,
        orchestrator=orchestrator,
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

    return router


def _validated_dependencies(
    *,
    store: Any,
    registry: Any,
    orchestrator: AgentTurnRunner,
) -> AgentRouterDependencies:
    missing = [
        name
        for name, value in (
            ("store", store),
            ("registry", registry),
            ("orchestrator", orchestrator),
        )
        if value is None
    ]
    if missing:
        names = ", ".join(missing)
        raise ValueError(f"Agent router dependencies must not be None: {names}.")
    if not callable(getattr(orchestrator, "run", None)):
        raise TypeError("Agent router orchestrator must provide run().")
    return AgentRouterDependencies(
        store=store,
        registry=registry,
        orchestrator=orchestrator,
    )


def _normalize_api_key(value: str | None) -> str | None:
    if value is None:
        return None
    stripped = value.strip()
    return stripped or None
