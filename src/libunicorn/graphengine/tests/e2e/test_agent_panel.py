import json

from playwright.sync_api import Page, Route, expect

from .helpers import STEP_TIMEOUT_MS, log_step


def test_agent_panel_submits_one_backend_turn_and_renders_result(
    rendered_page: Page,
) -> None:
    captured_requests: list[dict] = []
    captured_headers: list[dict[str, str]] = []

    def fulfill_agent_turn(route: Route) -> None:
        payload = route.request.post_data_json
        captured_requests.append(payload)
        captured_headers.append(route.request.headers)
        route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps(
                {
                    "schema_version": "unicorn_agent_result_v1",
                    "turn_id": payload["turn_id"],
                    "status": "completed",
                    "answer": "The current min_reads threshold is 1000.",
                    "tools_used": [
                        {
                            "tool_id": "graph.context",
                            "ok": True,
                        }
                    ],
                    "error": None,
                    "trace_id": "trace_browser_fixture",
                }
            ),
        )

    rendered_page.route("**/agent/turn", fulfill_agent_turn)

    log_step("Checking the backend-owned Agent controls")
    expect(rendered_page.locator("#agentProviderSelect")).to_be_enabled(
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#agentModelSelect")).to_be_enabled(
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#agentApiKey")).to_be_enabled(
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#agentBaseUrl")).to_be_enabled(
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#agentPrompt")).to_be_enabled(
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#agentProviderState")).not_to_contain_text(
        "loading",
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#agentMeta")).to_contain_text(
        "Backend Agent ready",
        timeout=STEP_TIMEOUT_MS,
    )

    log_step("Configuring the local OpenAI-compatible provider")
    rendered_page.locator("#agentProviderSelect").select_option(
        "local_openai_compat"
    )
    rendered_page.locator("#agentModelSelect").fill("fixture-local-model")
    rendered_page.locator("#agentBaseUrl").fill("http://localhost:8000")
    rendered_page.locator("#agentApiKey").fill("fixture-browser-secret")
    rendered_page.locator("#agentPrompt").fill(
        "What min_reads threshold am I using?"
    )
    expect(rendered_page.locator("#agentSendBtn")).to_be_enabled(
        timeout=STEP_TIMEOUT_MS,
    )

    log_step("Submitting exactly one backend Agent turn")
    rendered_page.locator("#agentSendBtn").click()
    expect(rendered_page.locator("#agentTranscript")).to_contain_text(
        "The current min_reads threshold is 1000.",
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#agentTranscript")).to_contain_text(
        "graph.context",
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#agentTranscript")).to_contain_text(
        "trace_browser_fixture",
        timeout=STEP_TIMEOUT_MS,
    )

    assert len(captured_requests) == 1
    request = captured_requests[0]
    assert request["schema_version"] == "unicorn_agent_turn_v1"
    assert request["prompt"] == "What min_reads threshold am I using?"
    assert request["provider"] == {
        "name": "local_openai_compat",
        "model": "fixture-local-model",
        "base_url": "http://localhost:8000",
    }
    assert request["graph_scope"]["datasets"]
    assert request["graph_scope"]["nodes_file"]
    assert request["graph_scope"]["names_file"]
    assert request["graph_scope"]["count_mode"] == "subtree"
    assert request["conversation"] == []
    assert (
        captured_headers[0]["x-unicorn-provider-api-key"]
        == "fixture-browser-secret"
    )
    assert "fixture-browser-secret" not in json.dumps(request)

    log_step("Checking turn identity and trace controls")
    expect(rendered_page.locator("#agentMeta")).to_contain_text(
        request["turn_id"],
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#agentTraceOpenBtn")).to_be_enabled()
    expect(rendered_page.locator("#agentTraceDownloadBtn")).to_be_enabled()
    expect(rendered_page.locator("#agentClearBtn")).to_be_enabled()

    log_step("Clearing transcript starts a fresh browser Agent session")
    rendered_page.locator("#agentClearBtn").click()
    expect(rendered_page.locator("#agentTranscript .agent-empty")).to_contain_text(
        "Ask a question about the current graph",
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#agentTraceOpenBtn")).to_be_disabled()
    expect(rendered_page.locator("#agentTraceDownloadBtn")).to_be_disabled()
