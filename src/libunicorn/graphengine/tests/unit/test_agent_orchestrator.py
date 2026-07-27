from __future__ import annotations

from .helpers import (
    build_orchestrator_harness,
    normalized_final_answer,
    normalized_tool_call,
    valid_browser_turn_request,
)


def test_unknown_tool_call_is_rejected_without_execution() -> None:
    responses = [normalized_tool_call(33090, tool_id="unknown.details")]
    orchestrator, adapter, _, executed = build_orchestrator_harness(responses)

    result = orchestrator.run(valid_browser_turn_request())

    assert result["status"] == "failed"
    assert result["error"]["code"] == "unknown_tool"
    assert executed == []
    assert len(adapter.inputs) == 1


def test_invalid_tool_arguments_are_rejected_without_execution() -> None:
    response = normalized_tool_call(33090)
    response["arguments"]["taxid"] = "Viridiplantae"
    orchestrator, adapter, _, executed = build_orchestrator_harness([response])

    result = orchestrator.run(valid_browser_turn_request())

    assert result["status"] == "failed"
    assert result["error"]["code"] == "invalid_tool_arguments"
    assert executed == []
    assert len(adapter.inputs) == 1


def test_repeated_identical_tool_call_is_rejected_before_second_execution() -> None:
    call = normalized_tool_call(33090)
    orchestrator, adapter, _, executed = build_orchestrator_harness([call, call])

    result = orchestrator.run(valid_browser_turn_request())

    assert result["status"] == "failed"
    assert result["error"]["code"] == "repeated_tool_call"
    assert executed == [{"taxid": 33090}]
    assert len(adapter.inputs) == 2
    assert adapter.inputs[0]["iteration"] == 0
    assert adapter.inputs[1]["iteration"] == 1
    assert len(adapter.inputs[1]["tool_results"]) == 1


def test_tool_loop_stops_after_four_executions() -> None:
    responses = [normalized_tool_call(taxid) for taxid in range(1, 6)]
    orchestrator, adapter, _, executed = build_orchestrator_harness(
        responses,
        max_tool_calls=4,
    )

    result = orchestrator.run(valid_browser_turn_request())

    assert result["status"] == "iteration_limit"
    assert result["error"]["code"] == "iteration_limit"
    assert executed == [
        {"taxid": 1},
        {"taxid": 2},
        {"taxid": 3},
        {"taxid": 4},
    ]
    assert [provider_input["iteration"] for provider_input in adapter.inputs] == [
        0,
        1,
        2,
        3,
        4,
    ]


def test_tool_result_is_reinjected_before_final_answer() -> None:
    responses = [
        normalized_tool_call(33090),
        normalized_final_answer("Viridiplantae is grounded by node.details."),
    ]
    orchestrator, adapter, _, executed = build_orchestrator_harness(responses)

    result = orchestrator.run(valid_browser_turn_request())

    assert result["status"] == "completed"
    assert result["answer"] == "Viridiplantae is grounded by node.details."
    assert result["tools_used"] == [
        {
            "tool_id": "node.details",
            "ok": True,
        }
    ]
    assert executed == [{"taxid": 33090}]
    assert adapter.inputs[1]["tool_results"][0]["tool_id"] == "node.details"
    assert adapter.inputs[1]["tool_results"][0]["ok"] is True


def test_assistant_message_is_a_terminal_completed_answer() -> None:
    response = {
        "schema_version": "unicorn_provider_response_v1",
        "type": "assistant_message",
        "content": "The current graph is ready.",
    }
    orchestrator, adapter, _, executed = build_orchestrator_harness([response])

    result = orchestrator.run(valid_browser_turn_request())

    assert result["status"] == "completed"
    assert result["answer"] == "The current graph is ready."
    assert result["tools_used"] == []
    assert executed == []
    assert len(adapter.inputs) == 1


def test_provider_error_is_a_terminal_failed_answer() -> None:
    response = {
        "schema_version": "unicorn_provider_response_v1",
        "type": "error",
        "code": "provider_refused",
        "message": "Provider refused this request.",
    }
    orchestrator, _, _, executed = build_orchestrator_harness([response])

    result = orchestrator.run(valid_browser_turn_request())

    assert result["status"] == "failed"
    assert result["error"] == {
        "code": "provider_refused",
        "message": "Provider refused this request.",
    }
    assert executed == []
