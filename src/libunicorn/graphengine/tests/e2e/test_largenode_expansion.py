from playwright.sync_api import Page, expect

from .helpers import (
    STEP_TIMEOUT_MS,
    click_tree_node,
    expand_tree_node,
    log_step,
    wait_for_selected_count,
    wait_for_species_option,
    wait_for_tree_label,
)


def test_uncollapse_tips_on_large_near_root_node(rendered_page: Page) -> None:
    log_step("Waiting for 'cellular organisms' in rendered tree")
    wait_for_tree_label(rendered_page, "cellular organisms")

    log_step("Expanding 'cellular organisms'")
    expand_tree_node(rendered_page, "cellular organisms", 131567)

    log_step("Waiting for 'Eukaryota' after expanding 'cellular organisms'")
    wait_for_tree_label(rendered_page, "Eukaryota")

    log_step("Selecting near-root node 'Eukaryota'")
    click_tree_node(rendered_page, "Eukaryota", modifiers=["Meta"])
    wait_for_selected_count(rendered_page, "1")

    visible_before = rendered_page.locator("#visibleCount").inner_text(timeout=STEP_TIMEOUT_MS)

    log_step("Running 'Uncollapse Tips' on selected near-root node")
    rendered_page.locator("#uncollapseTipsBtn").click()

    log_step("Waiting for rank dropdown to include 'species' after large-node expansion")
    wait_for_species_option(rendered_page)

    log_step("Checking that the UI did not surface the uncollapse failure status")
    expect(rendered_page.locator("#status")).not_to_contain_text(
        "Could not uncollapse selected nodes to tips on the backend",
        timeout=STEP_TIMEOUT_MS,
    )

    log_step("Waiting for visible node count to grow after large-node expansion")
    rendered_page.wait_for_function(
        """
        (previousText) => {
          const el = document.getElementById('visibleCount');
          if (!el) return false;
          const before = Number(String(previousText || '0').replace(/,/g, ''));
          const after = Number((el.textContent || '0').replace(/,/g, ''));
          return Number.isFinite(before) && Number.isFinite(after) && after > before;
        }
        """,
        arg=visible_before,
        timeout=STEP_TIMEOUT_MS,
    )
