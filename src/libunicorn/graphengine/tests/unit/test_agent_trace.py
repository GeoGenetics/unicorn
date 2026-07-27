from __future__ import annotations

from .helpers import (
    assert_contains_no_secret,
    build_orchestrator_harness,
    contract_validator,
    normalized_final_answer,
    normalized_tool_call,
    valid_browser_turn_request,
)


def assert_ordered_subsequence(actual: list[str], expected: list[str]) -> None:
    cursor = 0
    for event in actual:
        if cursor < len(expected) and event == expected[cursor]:
            cursor += 1
    assert cursor == len(expected), f"Missing ordered events: {expected[cursor:]}"


def test_trace_reconstructs_complete_two_iteration_turn_without_secrets() -> None:
    responses = [
        normalized_tool_call(33090),
        normalized_final_answer("Viridiplantae details."),
    ]
    orchestrator, adapter, trace_store, _ = build_orchestrator_harness(responses)
    secret = "fixture-provider-secret"

    result = orchestrator.run(
        valid_browser_turn_request(),
        api_key=secret,
    )
    events = trace_store.events_for(result["trace_id"])

    assert result["status"] == "completed"
    assert adapter.api_keys == [secret, secret]
    assert events
    assert [event["sequence"] for event in events] == list(
        range(1, len(events) + 1)
    )
    assert all(event["turn_id"] == result["turn_id"] for event in events)
    assert all(event["trace_id"] == result["trace_id"] for event in events)
    assert all(event["elapsed_ms"] >= 0 for event in events)

    for event in events:
        contract_validator("trace_event.schema.json").validate(event)
        assert_contains_no_secret(event, secret)

    assert_ordered_subsequence(
        [event["event"] for event in events],
        [
            "browser_turn_received",
            "graph_context_built",
            "provider_input_built",
            "provider_request_mapped",
            "provider_request_sent",
            "provider_response_received",
            "provider_response_normalized",
            "tool_execution_started",
            "tool_execution_completed",
            "provider_input_built",
            "provider_request_mapped",
            "provider_request_sent",
            "provider_response_received",
            "provider_response_normalized",
            "browser_turn_completed",
        ],
    )

