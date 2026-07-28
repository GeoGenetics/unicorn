from __future__ import annotations

import json

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
            "turn_received",
            "turn_validated",
            "context_built",
            "iteration_started",
            "provider_input_created",
            "provider_request_mapped",
            "provider_http_started",
            "provider_http_completed",
            "provider_output_extracted",
            "provider_response_normalized",
            "tool_call_validated",
            "tool_execution_started",
            "tool_execution_completed",
            "iteration_completed",
            "iteration_started",
            "tool_result_reinjected",
            "provider_input_created",
            "provider_request_mapped",
            "provider_http_started",
            "provider_http_completed",
            "provider_output_extracted",
            "provider_response_normalized",
            "iteration_completed",
            "turn_completed",
        ],
    )

    raw_event = next(
        event
        for event in events
        if event["event"] == "provider_http_completed"
    )
    assert raw_event["data"]["raw_response"] == normalized_tool_call(33090)
    assert raw_event["data"]["status_code"] == 200
    assert raw_event["data"]["response_bytes"] > 0
    assert raw_event["data"]["duration_ms"] >= 0

    extracted_event = next(
        event
        for event in events
        if event["event"] == "provider_output_extracted"
    )
    assert extracted_event["data"]["raw_output"]
    assert extracted_event["data"]["sanitized_input"]

    second_input = [
        event
        for event in events
        if event["event"] == "provider_input_created"
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
        event="provider_http_completed",
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


def test_jsonl_trace_store_persists_complete_secret_free_events(tmp_path) -> None:
    tracing = __import__(
        "unicorn_agent.tracing",
        fromlist=["JsonlTraceStore"],
    )
    secret = "fixture-provider-secret"
    trace_store = tracing.JsonlTraceStore(root_dir=tmp_path)
    trace_id = trace_store.start_trace(
        "turn_jsonl_001",
        secrets=(secret,),
    )
    trace_store.record(
        trace_id,
        iteration=0,
        event="turn_received",
        data={
            "prompt": "Trace this turn.",
            "authorization": f"Bearer {secret}",
        },
    )
    trace_store.record(
        trace_id,
        iteration=0,
        event="turn_failed",
        data={
            "message": f"Provider echoed {secret}",
        },
    )
    trace_store.finish_trace(trace_id)

    path = trace_store.trace_path(trace_id)
    persisted = [
        json.loads(line)
        for line in path.read_text(encoding="utf-8").splitlines()
    ]

    assert path.parent.parent == tmp_path.resolve()
    assert path.name == "turn_jsonl_001.jsonl"
    assert persisted == trace_store.events_for(trace_id)
    assert [event["sequence"] for event in persisted] == [1, 2]
    assert_contains_no_secret(persisted, secret)


def test_complete_two_iteration_turn_is_persisted_as_jsonl(tmp_path) -> None:
    tracing = __import__(
        "unicorn_agent.tracing",
        fromlist=["JsonlTraceStore"],
    )
    trace_store = tracing.JsonlTraceStore(root_dir=tmp_path)
    orchestrator, _, _, _ = build_orchestrator_harness(
        [
            normalized_tool_call(33090),
            normalized_final_answer("Persistent Viridiplantae details."),
        ],
        trace_store=trace_store,
    )

    result = orchestrator.run(valid_browser_turn_request())
    path = trace_store.trace_path(result["trace_id"])
    persisted = [
        json.loads(line)
        for line in path.read_text(encoding="utf-8").splitlines()
    ]

    assert result["status"] == "completed"
    assert persisted == trace_store.events_for(result["trace_id"])
    assert persisted[-1]["event"] == "turn_completed"
    assert any(event["event"] == "tool_result_reinjected" for event in persisted)
    assert any(
        event["event"] == "provider_http_completed"
        and event["data"]["status_code"] == 200
        for event in persisted
    )
