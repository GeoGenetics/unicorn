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

    raw_event = next(
        event
        for event in events
        if event["event"] == "provider_response_received"
    )
    assert raw_event["data"]["raw_response"] == normalized_tool_call(33090)

    second_input = [
        event
        for event in events
        if event["event"] == "provider_input_built"
    ][1]
    assert second_input["data"]["provider_input"]["tool_results"][0]["ok"] is True


def test_trace_store_redacts_credentials_and_returns_isolated_copies() -> None:
    tracing = __import__(
        "unicorn_agent.tracing",
        fromlist=["InMemoryTraceStore"],
    )
    secret = "fixture-provider-secret"
    trace_store = tracing.InMemoryTraceStore()
    trace_id = trace_store.start_trace(
        "turn_redaction_001",
        secrets=(secret,),
    )
    source = {
        "Authorization": f"Bearer {secret}",
        "nested": {
            "api_key": secret,
            "text": f"response accidentally echoed {secret}",
        },
    }

    trace_store.record(
        trace_id,
        iteration=0,
        event="provider_response_received",
        data=source,
    )
    first_read = trace_store.events_for(trace_id)

    assert_contains_no_secret(first_read, secret)
    assert "Authorization" not in first_read[0]["data"]
    assert "api_key" not in first_read[0]["data"]["nested"]
    assert first_read[0]["data"]["nested"]["text"].endswith("[REDACTED]")

    first_read[0]["data"]["nested"]["text"] = "mutated"
    second_read = trace_store.events_for(trace_id)
    assert second_read[0]["data"]["nested"]["text"].endswith("[REDACTED]")
