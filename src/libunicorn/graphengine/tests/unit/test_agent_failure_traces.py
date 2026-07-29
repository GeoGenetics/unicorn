from __future__ import annotations

import copy
import json
from collections.abc import Mapping
from typing import Any
from urllib.error import URLError

from unicorn_agent.orchestrator import AgentOrchestrator
from unicorn_agent.providers.base import ProviderTransportResponse
from unicorn_agent.providers.openai_compat import (
    LocalOpenAICompatibleAdapter,
)
from unicorn_agent.registry import ToolRegistry
from unicorn_agent.tracing import JsonlTraceStore

from .helpers import (
    AGENT_FIXTURE_DIR,
    assert_contains_no_secret,
    authoritative_graph_context,
    build_orchestrator_harness,
    contract_validator,
    load_json,
    normalized_tool_call,
    valid_browser_turn_request,
)


class FixtureLocalAdapter(LocalOpenAICompatibleAdapter):
    """Run the real local parser against deterministic provider envelopes."""

    def __init__(self, responses: list[dict[str, Any]]) -> None:
        super().__init__(base_url="http://localhost:8542")
        self._responses = copy.deepcopy(responses)

    def send_request(
        self,
        native_request: Mapping[str, Any],
        *,
        api_key: str | None = None,
    ) -> ProviderTransportResponse:
        del native_request, api_key
        if not self._responses:
            raise AssertionError("Fixture provider received too many requests.")
        return ProviderTransportResponse(
            status_code=200,
            body=self._responses.pop(0),
        )


def _registry(
    executed: list[dict[str, Any]],
) -> ToolRegistry:
    registry = ToolRegistry()

    def node_details(arguments: dict[str, Any]) -> dict[str, Any]:
        executed.append(copy.deepcopy(arguments))
        return {
            "taxid": arguments["taxid"],
            "name": "Viridiplantae",
        }

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
    return registry


def _orchestrator(
    provider: Any,
    trace_store: JsonlTraceStore,
    executed: list[dict[str, Any]],
) -> AgentOrchestrator:
    return AgentOrchestrator(
        provider=provider,
        registry=_registry(executed),
        context_builder=lambda request: authoritative_graph_context(),
        trace_store=trace_store,
    )


def _persisted_events(
    trace_store: JsonlTraceStore,
    result: dict[str, Any],
) -> list[dict[str, Any]]:
    path = trace_store.trace_path(result["trace_id"])
    events = [
        json.loads(line)
        for line in path.read_text(encoding="utf-8").splitlines()
    ]

    assert events == trace_store.events_for(result["trace_id"])
    assert [event["sequence"] for event in events] == list(
        range(1, len(events) + 1)
    )
    for event in events:
        contract_validator("trace_event.schema.json").validate(event)
    return events


def _events_named(
    events: list[dict[str, Any]],
    name: str,
) -> list[dict[str, Any]]:
    return [event for event in events if event["event"] == name]


def test_unknown_tool_rejection_persists_complete_trace(tmp_path) -> None:
    trace_store = JsonlTraceStore(root_dir=tmp_path)
    orchestrator, adapter, _, executed = build_orchestrator_harness(
        [normalized_tool_call(33090, tool_id="unknown.details")],
        trace_store=trace_store,
    )

    result = orchestrator.run(valid_browser_turn_request())
    events = _persisted_events(trace_store, result)

    assert result["status"] == "failed"
    assert result["error"]["code"] == "unknown_tool"
    assert executed == []
    assert len(adapter.inputs) == 1
    assert _events_named(events, "tool_execution_started") == []
    assert _events_named(events, "tool_execution_completed") == []
    assert _events_named(events, "provider_response_normalized")[0]["data"][
        "normalized_response"
    ]["tool_id"] == "unknown.details"
    assert _events_named(events, "iteration_completed")[-1]["data"] == {
        "status": "failed",
        "response_type": "tool_call",
        "error_code": "unknown_tool",
    }
    assert events[-1]["event"] == "turn_failed"
    assert events[-1]["data"]["result"] == result


def test_repeated_tool_call_persists_one_execution_and_controlled_failure(
    tmp_path,
) -> None:
    trace_store = JsonlTraceStore(root_dir=tmp_path)
    call = normalized_tool_call(33090)
    orchestrator, adapter, _, executed = build_orchestrator_harness(
        [call, call],
        trace_store=trace_store,
    )

    result = orchestrator.run(valid_browser_turn_request())
    events = _persisted_events(trace_store, result)

    assert result["status"] == "failed"
    assert result["error"]["code"] == "repeated_tool_call"
    assert result["tools_used"] == [{"tool_id": "node.details", "ok": True}]
    assert executed == [{"taxid": 33090}]
    assert len(adapter.inputs) == 2
    assert len(_events_named(events, "tool_execution_started")) == 1
    assert len(_events_named(events, "tool_execution_completed")) == 1
    assert len(_events_named(events, "tool_result_reinjected")) == 1
    assert _events_named(events, "iteration_completed")[-1]["data"] == {
        "status": "failed",
        "response_type": "tool_call",
        "error_code": "repeated_tool_call",
    }
    assert events[-1]["event"] == "turn_failed"
    assert events[-1]["data"]["result"] == result


def test_think_output_retains_raw_reasoning_and_parses_sanitized_json(
    tmp_path,
) -> None:
    fixture = load_json(
        AGENT_FIXTURE_DIR
        / "local_openai_compat"
        / "think_and_tool_call.json"
    )
    final_response = {
        "id": "chatcmpl_fixture_local_final",
        "object": "chat.completion",
        "created": 0,
        "model": "fixture-local-model",
        "choices": [
            {
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": (
                        '{"type":"final_answer","content":'
                        '"Viridiplantae details are grounded."}'
                    ),
                },
                "finish_reason": "stop",
            }
        ],
    }
    provider = FixtureLocalAdapter(
        [
            fixture["raw_provider_response"],
            final_response,
        ]
    )
    trace_store = JsonlTraceStore(root_dir=tmp_path)
    executed: list[dict[str, Any]] = []
    orchestrator = _orchestrator(provider, trace_store, executed)

    result = orchestrator.run(valid_browser_turn_request())
    events = _persisted_events(trace_store, result)

    assert result["status"] == "completed"
    assert executed == [{"taxid": 33090}]

    raw_response_event = _events_named(
        events,
        "provider_http_completed",
    )[0]
    raw_response_text = raw_response_event["data"]["raw_response"]["choices"][
        0
    ]["message"]["content"]
    assert "<think>" in raw_response_text
    assert "</think>" in raw_response_text

    extracted_event = _events_named(
        events,
        "provider_output_extracted",
    )[0]
    assert "<think>" in extracted_event["data"]["raw_output"]
    assert "</think>" in extracted_event["data"]["raw_output"]
    assert "<think>" not in extracted_event["data"]["sanitized_input"]
    assert "</think>" not in extracted_event["data"]["sanitized_input"]
    assert (
        extracted_event["data"]["sanitized_input"]
        == fixture["expected_extracted_text"]
    )

    normalized_event = _events_named(
        events,
        "provider_response_normalized",
    )[0]
    assert (
        normalized_event["data"]["normalized_response"]
        == fixture["expected_normalized_response"]
    )
    assert events[-1]["event"] == "turn_completed"


def test_network_failure_returns_structured_error_and_complete_trace(
    tmp_path,
    monkeypatch,
) -> None:
    secret = "fixture-network-secret"
    base_url = "http://127.0.0.1:65534"
    provider = LocalOpenAICompatibleAdapter(base_url=base_url)
    trace_store = JsonlTraceStore(root_dir=tmp_path)
    executed: list[dict[str, Any]] = []
    orchestrator = _orchestrator(provider, trace_store, executed)
    request = valid_browser_turn_request()
    request["provider"]["base_url"] = base_url

    def refused_connection(request, *, timeout):
        del request, timeout
        raise URLError(ConnectionRefusedError(111, "Connection refused"))

    monkeypatch.setattr(
        "unicorn_agent.providers.base.urlopen",
        refused_connection,
    )

    result = orchestrator.run(request, api_key=secret)
    events = _persisted_events(trace_store, result)

    assert result["status"] == "failed"
    assert result["error"]["code"] == "local_openai_compat_network_error"
    assert "Connection refused" in result["error"]["message"]
    assert executed == []
    assert _events_named(events, "provider_http_completed") == []
    assert _events_named(events, "provider_output_extracted") == []

    failure_event = _events_named(events, "provider_http_failed")[0]
    assert failure_event["data"]["provider"] == "local_openai_compat"
    assert failure_event["data"]["duration_ms"] >= 0
    assert failure_event["data"]["error"]["code"] == (
        "local_openai_compat_network_error"
    )
    assert "Connection refused" in failure_event["data"]["error"]["message"]
    assert events[-1]["event"] == "turn_failed"
    assert events[-1]["data"]["result"] == result
    assert_contains_no_secret(events, secret)
