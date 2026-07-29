from __future__ import annotations

import copy
import importlib
import json
from pathlib import Path
from types import ModuleType
from typing import Any, Iterable

import pytest
from jsonschema import Draft202012Validator, FormatChecker
from referencing import Registry, Resource

from unicorn_agent.providers.base import (
    ProviderOutputInspection,
    ProviderTransportResponse,
)


GRAPHENGINE_DIR = Path(__file__).resolve().parents[2]
CONTRACT_DIR = GRAPHENGINE_DIR / "unicorn_agent" / "schemas"
AGENT_FIXTURE_DIR = GRAPHENGINE_DIR / "tests" / "fixtures" / "agent"

def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def contract_schemas() -> dict[str, dict[str, Any]]:
    return {
        path.name: load_json(path)
        for path in sorted(CONTRACT_DIR.glob("*.schema.json"))
    }


def contract_registry(
    schemas: dict[str, dict[str, Any]] | None = None,
) -> Registry:
    active_schemas = schemas or contract_schemas()
    registry = Registry()
    for schema in active_schemas.values():
        registry = registry.with_resource(
            schema["$id"],
            Resource.from_contents(schema),
        )
    return registry


def contract_validator(schema_name: str) -> Draft202012Validator:
    schemas = contract_schemas()
    return Draft202012Validator(
        schemas[schema_name],
        registry=contract_registry(schemas),
        format_checker=FormatChecker(),
    )


def provider_fixtures() -> list[tuple[str, Path, dict[str, Any]]]:
    fixtures: list[tuple[str, Path, dict[str, Any]]] = []
    for path in sorted(AGENT_FIXTURE_DIR.glob("*/*.json")):
        fixture = load_json(path)
        provider = str(fixture["provenance"]["provider"])
        fixtures.append((provider, path, fixture))
    return fixtures


def require_agent_module(module_name: str) -> ModuleType:
    try:
        return importlib.import_module(module_name)
    except ModuleNotFoundError as error:
        if error.name == module_name or str(error.name or "").startswith("unicorn_agent"):
            pytest.fail(
                "The Phase 4 test boundary is present, but the rewritten "
                f"runtime module {module_name!r} has not been implemented yet."
            )
        raise


def valid_browser_turn_request() -> dict[str, Any]:
    schema = load_json(CONTRACT_DIR / "browser_turn_request.schema.json")
    return copy.deepcopy(schema["examples"][0])


def valid_provider_input() -> dict[str, Any]:
    schema = load_json(CONTRACT_DIR / "provider_adapter_input.schema.json")
    return copy.deepcopy(schema["examples"][0])


def authoritative_graph_context() -> dict[str, Any]:
    return copy.deepcopy(valid_provider_input()["graph_context"])


def normalized_tool_call(taxid: int, tool_id: str = "node.details") -> dict[str, Any]:
    return {
        "schema_version": "unicorn_provider_response_v1",
        "type": "tool_call",
        "tool_id": tool_id,
        "arguments": {
            "taxid": taxid,
        },
    }


def normalized_final_answer(content: str = "Grounded answer.") -> dict[str, Any]:
    return {
        "schema_version": "unicorn_provider_response_v1",
        "type": "final_answer",
        "content": content,
    }


def assert_contains_no_secret(value: Any, secret: str) -> None:
    secret_lower = secret.lower()

    def walk(item: Any) -> Iterable[str]:
        if isinstance(item, dict):
            for key, child in item.items():
                yield str(key)
                yield from walk(child)
        elif isinstance(item, list):
            for child in item:
                yield from walk(child)
        elif isinstance(item, str):
            yield item

    for text in walk(value):
        lowered = text.lower()
        assert secret_lower not in lowered
        assert "x-unicorn-provider-api-key" not in lowered
        assert "authorization" not in lowered


class SequenceProviderAdapter:
    """Deterministic adapter implementing the frozen provider adapter surface."""

    def __init__(self, responses: list[dict[str, Any]]) -> None:
        self._responses = copy.deepcopy(responses)
        self.inputs: list[dict[str, Any]] = []
        self.native_requests: list[dict[str, Any]] = []
        self.api_keys: list[str | None] = []

    def map_request(self, provider_input: dict[str, Any]) -> dict[str, Any]:
        self.inputs.append(copy.deepcopy(provider_input))
        native_request = {
            "fixture_iteration": provider_input["iteration"],
            "model": provider_input["model"],
        }
        self.native_requests.append(copy.deepcopy(native_request))
        return native_request

    def send_request(
        self,
        native_request: dict[str, Any],
        *,
        api_key: str | None = None,
    ) -> ProviderTransportResponse:
        self.api_keys.append(api_key)
        if not self._responses:
            raise AssertionError("Fake provider received more requests than expected.")
        return ProviderTransportResponse(
            status_code=200,
            body=self._responses.pop(0),
        )

    def inspect_response(
        self,
        raw_response: dict[str, Any],
    ) -> ProviderOutputInspection:
        text = json.dumps(raw_response, separators=(",", ":"), ensure_ascii=True)
        return ProviderOutputInspection(
            raw_text=text,
            sanitized_text=text,
        )

    def parse_response(self, sanitized_text: str) -> dict[str, Any]:
        return json.loads(sanitized_text)


def build_orchestrator_harness(
    responses: list[dict[str, Any]],
    *,
    max_tool_calls: int = 4,
    trace_store: Any = None,
) -> tuple[Any, SequenceProviderAdapter, Any, list[dict[str, Any]]]:
    orchestrator_module = require_agent_module("unicorn_agent.orchestrator")
    registry_module = require_agent_module("unicorn_agent.registry")
    tracing_module = require_agent_module("unicorn_agent.tracing")

    executed_arguments: list[dict[str, Any]] = []

    def node_details(arguments: dict[str, Any]) -> dict[str, Any]:
        executed_arguments.append(copy.deepcopy(arguments))
        return {
            "taxid": arguments["taxid"],
            "name": f"Fixture node {arguments['taxid']}",
        }

    registry = registry_module.ToolRegistry()
    registry.register(
        tool_id="node.details",
        description="Return details for one taxid.",
        when_to_use="Use after resolving a taxid.",
        output_summary="One fixture node summary.",
        arguments_schema={
            "type": "object",
            "additionalProperties": False,
            "required": ["taxid"],
            "properties": {
                "taxid": {
                    "type": "integer",
                    "minimum": 1,
                }
            },
        },
        handler=node_details,
        mutation=False,
    )

    adapter = SequenceProviderAdapter(responses)
    active_trace_store = trace_store or tracing_module.InMemoryTraceStore()
    orchestrator = orchestrator_module.AgentOrchestrator(
        provider=adapter,
        registry=registry,
        context_builder=lambda request: authoritative_graph_context(),
        trace_store=active_trace_store,
        max_tool_calls=max_tool_calls,
    )
    return orchestrator, adapter, active_trace_store, executed_arguments
