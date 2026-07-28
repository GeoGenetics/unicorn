"""Ordered, secret-free memory and JSONL tracing for Unicorn Agent turns."""

from __future__ import annotations

import copy
import json
import os
import re
from collections.abc import Iterable, Mapping
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
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
_DEFAULT_TRACE_ROOT = Path(__file__).resolve().parents[1] / "logs" / "agent_trace"
TRACE_ROOT_ENV = "UNICORN_GRAPHENGINE_AGENT_TRACE_DIR"


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
            try:
                self._trace_started(trace_id, self._traces[trace_id])
            except Exception:
                del self._traces[trace_id]
                raise
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
            self._event_recorded(trace_id, trace_event)
            state.events.append(copy.deepcopy(trace_event))
            return copy.deepcopy(trace_event)

    def finish_trace(self, trace_id: str) -> None:
        """Discard transient redaction secrets after the terminal event."""

        with self._lock:
            state = self._traces.get(trace_id)
            if state is None:
                raise KeyError(f"Unknown Agent trace: {trace_id}")
            state.secrets = ()

    def events_for(self, trace_id: str) -> list[dict[str, Any]]:
        with self._lock:
            state = self._traces.get(trace_id)
            return copy.deepcopy(state.events) if state is not None else []

    def _trace_started(self, trace_id: str, state: _TraceState) -> None:
        del trace_id, state

    def _event_recorded(
        self,
        trace_id: str,
        event: Mapping[str, Any],
    ) -> None:
        del trace_id, event


class JsonlTraceStore(InMemoryTraceStore):
    """Persist each validated event as one JSON object per line."""

    def __init__(self, root_dir: str | Path | None = None) -> None:
        super().__init__()
        configured = (
            root_dir
            or os.environ.get(TRACE_ROOT_ENV)
            or _DEFAULT_TRACE_ROOT
        )
        self._root_dir = Path(configured).expanduser().resolve()
        self._paths: dict[str, Path] = {}

    @property
    def root_dir(self) -> Path:
        return self._root_dir

    def trace_path(self, trace_id: str) -> Path:
        with self._lock:
            path = self._paths.get(trace_id)
            if path is None:
                raise KeyError(f"Unknown Agent trace: {trace_id}")
            return path

    def _trace_started(self, trace_id: str, state: _TraceState) -> None:
        date_dir = self._root_dir / _utc_date()
        date_dir.mkdir(parents=True, exist_ok=True)
        path = date_dir / f"{state.turn_id}.jsonl"
        path.touch(exist_ok=True)
        self._paths[trace_id] = path

    def _event_recorded(
        self,
        trace_id: str,
        event: Mapping[str, Any],
    ) -> None:
        path = self._paths[trace_id]
        line = json.dumps(
            event,
            separators=(",", ":"),
            ensure_ascii=True,
        )
        with path.open("a", encoding="utf-8") as handle:
            handle.write(line)
            handle.write("\n")


def _utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _utc_date() -> str:
    return datetime.now(timezone.utc).date().isoformat()


def payload_size_bytes(value: Any) -> int:
    """Return the deterministic UTF-8 JSON size used by trace measurements."""

    encoded = json.dumps(
        value,
        separators=(",", ":"),
        ensure_ascii=True,
        default=str,
    ).encode("utf-8")
    return len(encoded)


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
