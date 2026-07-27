from playwright.sync_api import Page, expect

from .helpers import STEP_TIMEOUT_MS, log_step


def test_agent_panel_reports_rebuilding_state(rendered_page: Page) -> None:
    log_step("Checking preserved Agent panel during framework rebuild")
    expect(rendered_page.locator("#agentSectionBody")).not_to_be_hidden(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#agentProviderSelect")).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#agentModelSelect")).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#agentPrompt")).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#agentSendBtn")).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#agentClearBtn")).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#agentTranscript")).to_be_visible(timeout=STEP_TIMEOUT_MS)

    log_step("Verifying no old Agent runtime remains active")
    expect(rendered_page.locator("#agentMeta")).to_contain_text(
        "Agent framework rebuilding",
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#agentTranscript .agent-empty")).to_contain_text(
        "new traceable Agent runtime is not available yet",
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#agentProviderState")).to_contain_text(
        "previous Agent runtime has been removed",
        timeout=STEP_TIMEOUT_MS,
    )
    expect(rendered_page.locator("#agentProviderSelect")).to_be_disabled()
    expect(rendered_page.locator("#agentModelSelect")).to_be_disabled()
    expect(rendered_page.locator("#agentApiKey")).to_be_disabled()
    expect(rendered_page.locator("#agentBaseUrl")).to_be_disabled()
    expect(rendered_page.locator("#agentPrompt")).to_be_disabled()
    expect(rendered_page.locator("#agentSendBtn")).to_be_disabled()
    expect(rendered_page.locator("#agentClearBtn")).to_be_disabled()
