"""Trace-store boundary used by the Agent orchestrator.

Complete event construction and persistence belong to Phase 6.8 and Phase 7.
This bootstrap store only allocates stable trace IDs for the turn loop.
"""

from __future__ import annotations

from threading import RLock
from typing import Any


class InMemoryTraceStore:
    """Allocate traces in memory before ordered event recording is implemented."""

    def __init__(self) -> None:
        self._events: dict[str, list[dict[str, Any]]] = {}
        self._lock = RLock()

    def start_trace(self, turn_id: str) -> str:
        base = turn_id.removeprefix("turn_")
        trace_id = f"trace_{base}"
        with self._lock:
            if trace_id in self._events:
                suffix = 2
                while f"{trace_id}_{suffix}" in self._events:
                    suffix += 1
                trace_id = f"{trace_id}_{suffix}"
            self._events[trace_id] = []
        return trace_id

    def events_for(self, trace_id: str) -> list[dict[str, Any]]:
        with self._lock:
            return [dict(event) for event in self._events.get(trace_id, [])]
