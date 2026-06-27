from playwright.sync_api import Page, expect


def test_connect_backend(connected_page: Page) -> None:
    expect(connected_page.locator("#connectionState")).to_have_text("Connected")
    expect(connected_page.locator("#remoteDatasetsPanel")).not_to_be_hidden()
    expect(connected_page.locator("#status")).to_contain_text("Tunnel check succeeded")
