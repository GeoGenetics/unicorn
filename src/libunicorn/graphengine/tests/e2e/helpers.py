from playwright.sync_api import Page, expect


STEP_TIMEOUT_MS = 60000


def log_step(message: str) -> None:
    print(f"[graphengine-e2e] {message}", flush=True)


def node_locator(page: Page, label: str):
    return page.locator("#treeSvg .node").filter(has_text=label).first


def node_hit_locator(page: Page, label: str):
    return node_locator(page, label).locator(".node-hit")


def click_tree_node(page: Page, label: str, *, modifiers: list[str] | None = None) -> None:
    locator = node_hit_locator(page, label)
    expect(locator).to_be_visible(timeout=STEP_TIMEOUT_MS)
    locator.click(force=True, modifiers=modifiers or [])


def wait_for_tree_label(page: Page, label: str) -> None:
    expect(node_locator(page, label)).to_be_visible(timeout=STEP_TIMEOUT_MS)


def wait_for_selected_count(page: Page, expected_text: str) -> None:
    expect(page.locator("#selectedCount")).to_have_text(expected_text, timeout=STEP_TIMEOUT_MS)


def wait_for_selected_count_gt_one(page: Page) -> None:
    page.wait_for_function(
        """
        () => {
          const el = document.getElementById('selectedCount');
          if (!el) return false;
          const value = Number((el.textContent || '0').replace(/,/g, ''));
          return Number.isFinite(value) && value > 1;
        }
        """,
        timeout=STEP_TIMEOUT_MS,
    )


def wait_for_species_option(page: Page) -> None:
    expect(page.locator("#selectToRankValue option[value='species']")).to_be_attached(timeout=STEP_TIMEOUT_MS)


def expand_tree_node(page: Page, label: str, taxid: int) -> None:
    with page.expect_response(lambda response: "/expand-node" in response.url and f"taxid={taxid}" in response.url, timeout=STEP_TIMEOUT_MS):
        click_tree_node(page, label)


def select_species_below_viridiplantae(page: Page) -> None:
    log_step("Waiting for 'cellular organisms' in rendered tree")
    wait_for_tree_label(page, "cellular organisms")

    log_step("Expanding 'cellular organisms'")
    expand_tree_node(page, "cellular organisms", 131567)

    log_step("Waiting for 'Eukaryota' after expanding 'cellular organisms'")
    wait_for_tree_label(page, "Eukaryota")

    log_step("Expanding 'Eukaryota'")
    expand_tree_node(page, "Eukaryota", 2759)

    log_step("Waiting for 'Viridiplantae' after expanding 'Eukaryota'")
    wait_for_tree_label(page, "Viridiplantae")

    log_step("Selecting 'Viridiplantae'")
    click_tree_node(page, "Viridiplantae", modifiers=["Meta"])
    wait_for_selected_count(page, "1")

    log_step("Running 'Uncollapse Tips' on selected node")
    page.locator("#uncollapseTipsBtn").click()

    log_step("Waiting for rank dropdown to include 'species'")
    wait_for_species_option(page)

    log_step("Selecting rank 'species'")
    page.locator("#selectToRankValue").select_option("species")
    expect(page.locator("#selectToRankBtn")).to_be_enabled(timeout=STEP_TIMEOUT_MS)

    log_step("Triggering 'Select To Rank'")
    page.locator("#selectToRankBtn").click()

    log_step("Waiting for selected count to become greater than one")
    wait_for_selected_count_gt_one(page)

    log_step("Checking final status text for rank selection")
    expect(page.locator("#status")).to_contain_text("at rank species", timeout=STEP_TIMEOUT_MS)
