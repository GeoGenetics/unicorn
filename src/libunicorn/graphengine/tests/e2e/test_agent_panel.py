from playwright.sync_api import Page, expect

from .helpers import STEP_TIMEOUT_MS, log_step


def test_agent_panel_mock_transcript_and_logs(rendered_page: Page) -> None:
    log_step("Checking agent panel controls in rendered backend session")
    expect(rendered_page.locator("#agentSectionBody")).not_to_be_hidden(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#agentProviderSelect")).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#agentModelSelect")).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#agentPrompt")).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#agentSendBtn")).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#agentClearBtn")).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#agentTranscript")).to_be_visible(timeout=STEP_TIMEOUT_MS)

    log_step("Verifying agent runtime is still in mock mode")
    expect(rendered_page.locator("#agentMeta")).to_contain_text("mock mode", timeout=STEP_TIMEOUT_MS)

    prompt = "What datasets are selected?"
    log_step("Submitting deterministic mock prompt to the agent panel")
    rendered_page.locator("#agentPrompt").fill(prompt)
    rendered_page.locator("#agentSendBtn").click()

    log_step("Waiting for user and assistant transcript entries")
    user_entry = rendered_page.locator("#agentTranscript .agent-entry.role-user").last
    assistant_entry = rendered_page.locator("#agentTranscript .agent-entry.role-assistant").last
    expect(user_entry).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(user_entry).to_contain_text(prompt, timeout=STEP_TIMEOUT_MS)
    expect(assistant_entry).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(assistant_entry.locator(".agent-entry-message")).not_to_have_text("", timeout=STEP_TIMEOUT_MS)
    expect(assistant_entry.locator(".agent-tool-badge").first).to_be_visible(timeout=STEP_TIMEOUT_MS)

    log_step("Opening client log section and checking agent activity entries")
    if rendered_page.locator("#logSectionBody").is_hidden():
        rendered_page.locator("#toggleLogSection").click()
    expect(rendered_page.locator("#logSectionBody")).not_to_be_hidden(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#clientLog")).to_contain_text("agent", timeout=STEP_TIMEOUT_MS)
    rendered_page.wait_for_function(
        """
        () => {
          const text = document.getElementById('clientLog')?.textContent || '';
          return text.includes('Captured agent context')
            || text.includes('Provider responded with')
            || text.includes('Executing Unicorn tool');
        }
        """,
        timeout=STEP_TIMEOUT_MS,
    )

    log_step("Clearing transcript back to empty state")
    rendered_page.locator("#agentClearBtn").click()
    expect(rendered_page.locator("#agentTranscript .agent-empty")).to_contain_text(
        "Render a backend-backed tree, then agent replies will appear here.",
        timeout=STEP_TIMEOUT_MS,
    )
