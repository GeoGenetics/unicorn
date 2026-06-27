from playwright.sync_api import Page, expect


def test_render_tree(rendered_page: Page) -> None:
    expect(rendered_page.locator("#visibleCount")).not_to_have_text("0")
    rendered_page.wait_for_function(
        "() => document.querySelectorAll('#treeSvg .node').length > 1",
        timeout=180000,
    )
