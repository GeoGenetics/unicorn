from __future__ import annotations

import json

from unicorn_agent.runtime import BackendAgentRuntime
from unicorn_agent.tracing import JsonlTraceStore

from .helpers import valid_browser_turn_request
from .test_agent_context import FixtureStore


def test_backend_runtime_completes_deterministic_fake_tool_loop(tmp_path) -> None:
    trace_store = JsonlTraceStore(root_dir=tmp_path)
    runtime = BackendAgentRuntime(
        store=FixtureStore(),
        trace_store=trace_store,
    )
    request = valid_browser_turn_request()

    result = runtime.run(request)

    assert result["status"] == "completed"
    assert result["answer"] == (
        "Deterministic fake provider inspected graph.context: "
        "1 selected dataset(s), 2 visible node(s), and 0 selected node(s)."
    )
    assert result["tools_used"] == [
        {
            "tool_id": "graph.context",
            "ok": True,
        }
    ]

    trace_path = trace_store.trace_path(result["trace_id"])
    events = [
        json.loads(line)
        for line in trace_path.read_text(encoding="utf-8").splitlines()
    ]
    assert [event["iteration"] for event in events if event["event"] == "iteration_started"] == [
        0,
        1,
    ]
    assert any(
        event["event"] == "tool_execution_completed"
        and event["data"]["tool_result"]["tool_id"] == "graph.context"
        and event["data"]["tool_result"]["ok"] is True
        for event in events
    )
    assert any(
        event["event"] == "tool_result_reinjected"
        and event["iteration"] == 1
        for event in events
    )
    assert events[-1]["event"] == "turn_completed"
