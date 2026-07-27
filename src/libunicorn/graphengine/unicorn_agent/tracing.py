"""Ordered, secret-free in-memory tracing for Unicorn Agent turns."""

from __future__ import annotations

import copy
import re
from collections.abc import Iterable, Mapping
from dataclasses import dataclass, field
from datetime import datetime, timezone
from threading import RLock
from time import monotonic
from typing import Any

from unicorn_agent.contracts import validate_trace_event


_TURN_ID = re.compile(r"^turn_[A-Za-z0-9._-]+$")
_FORBIDDEN_KEYS = {
    "api_key",
    "apikey",
    "authorization",
    "x_unicorn_provider_api_key",
}


@dataclass(repr=False)
class _TraceState:
    turn_id: str
    started_at: float
    secrets: tuple[str, ...] = field(repr=False)
    events: list[dict[str, Any]] = field(default_factory=list)


class InMemoryTraceStore:
    """Record validated turn events without writing to persistent storage."""

    def __init__(self) -> None:
        self._traces: dict[str, _TraceState] = {}
        self._lock = RLock()

    def start_trace(
        self,
        turn_id: str,
        *,
        secrets: Iterable[str | None] = (),
    ) -> str:
        if not _TURN_ID.fullmatch(turn_id):
            raise ValueError("Trace turn_id does not satisfy the V1 contract.")

        base = turn_id.removeprefix("turn_")
        trace_id = f"trace_{base}"
        with self._lock:
            if trace_id in self._traces:
                suffix = 2
                while f"{trace_id}_{suffix}" in self._traces:
                    suffix += 1
                trace_id = f"{trace_id}_{suffix}"
            self._traces[trace_id] = _TraceState(
                turn_id=turn_id,
                started_at=monotonic(),
                secrets=tuple(
                    value
                    for value in secrets
                    if isinstance(value, str) and value
                ),
            )
        return trace_id

    def record(
        self,
        trace_id: str,
        *,
        iteration: int,
        event: str,
        data: Mapping[str, Any],
    ) -> dict[str, Any]:
        with self._lock:
            state = self._traces.get(trace_id)
            if state is None:
                raise KeyError(f"Unknown Agent trace: {trace_id}")

            trace_event = {
                "schema_version": "unicorn_agent_trace_v1",
                "trace_id": trace_id,
                "turn_id": state.turn_id,
                "iteration": iteration,
                "sequence": len(state.events) + 1,
                "timestamp": _utc_timestamp(),
                "elapsed_ms": max(
                    0.0,
                    (monotonic() - state.started_at) * 1000.0,
                ),
                "event": event,
                "data": _sanitize_trace_data(data, state.secrets),
            }
            validate_trace_event(trace_event)
            state.events.append(copy.deepcopy(trace_event))
            return copy.deepcopy(trace_event)

    def events_for(self, trace_id: str) -> list[dict[str, Any]]:
        with self._lock:
            state = self._traces.get(trace_id)
            return copy.deepcopy(state.events) if state is not None else []


def _utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _sanitize_trace_data(value: Any, secrets: tuple[str, ...]) -> Any:
    if isinstance(value, Mapping):
        sanitized: dict[str, Any] = {}
        for key, child in value.items():
            clean_key = str(key)
            normalized_key = clean_key.casefold().replace("-", "_")
            if normalized_key in _FORBIDDEN_KEYS:
                continue
            sanitized[clean_key] = _sanitize_trace_data(child, secrets)
        return sanitized
    if isinstance(value, (list, tuple)):
        return [_sanitize_trace_data(item, secrets) for item in value]
    if isinstance(value, str):
        sanitized_text = value
        for secret in secrets:
            sanitized_text = sanitized_text.replace(secret, "[REDACTED]")
        return sanitized_text
    if value is None or isinstance(value, (bool, int, float)):
        return value
    return _sanitize_trace_data(str(value), secrets)
