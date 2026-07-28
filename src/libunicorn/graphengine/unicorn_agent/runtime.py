"""Compose the request-scoped backend Agent runtime."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from typing import Any

from unicorn_agent.context import BackendContextBuilder, BackendContextStore
from unicorn_agent.orchestrator import AgentOrchestrator
from unicorn_agent.providers.fake import DeterministicFakeProviderAdapter
from unicorn_agent.registry import ToolRegistry
from unicorn_agent.tools import create_read_only_registry
from unicorn_agent.tracing import JsonlTraceStore


RegistryFactory = Callable[..., ToolRegistry]


class BackendAgentRuntime:
    """Build one isolated registry and orchestrator for each browser turn."""

    def __init__(
        self,
        *,
        store: BackendContextStore,
        trace_store: JsonlTraceStore | None = None,
        registry_factory: RegistryFactory = create_read_only_registry,
    ) -> None:
        self._store = store
        self._trace_store = trace_store or JsonlTraceStore()
        self._registry_factory = registry_factory
        self._provider = DeterministicFakeProviderAdapter()

    @property
    def registry_factory(self) -> RegistryFactory:
        return self._registry_factory

    @property
    def trace_store(self) -> JsonlTraceStore:
        return self._trace_store

    def run(
        self,
        request: Mapping[str, Any],
        *,
        api_key: str | None = None,
    ) -> dict[str, Any]:
        registry = self._registry_factory(
            store=self._store,
            request=request,
        )
        orchestrator = AgentOrchestrator(
            provider=self._provider,
            registry=registry,
            context_builder=BackendContextBuilder(self._store),
            trace_store=self._trace_store,
        )
        return orchestrator.run(
            request,
            api_key=api_key,
        )


def create_agent_runtime(
    *,
    store: BackendContextStore,
) -> BackendAgentRuntime:
    """Create the deterministic no-network Phase 9 runtime."""

    return BackendAgentRuntime(store=store)
