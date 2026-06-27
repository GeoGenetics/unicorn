from playwright.sync_api import Page, expect

from .helpers import STEP_TIMEOUT_MS, log_step, select_species_below_viridiplantae


def test_species_selection_count_matrix_barplot_and_pcoa(rendered_page: Page) -> None:
    select_species_below_viridiplantae(rendered_page)

    log_step("Switching top-bar count view to cumulative")
    rendered_page.locator("#countViewMode").select_option("subtree")

    log_step("Opening count matrix from species selection")
    rendered_page.locator("#subtreeReportBtn").click()
    expect(rendered_page.locator("#tablePanel")).not_to_be_hidden(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#tablePanelTitle")).to_contain_text("Count matrix", timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#subtreeReport")).not_to_be_hidden(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#subtreeReport")).to_contain_text("Selected-node cumulative count matrix", timeout=STEP_TIMEOUT_MS)

    log_step("Waiting for Barplot and PCoA controls to become visible")
    expect(rendered_page.locator("#barplotBtn")).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#pcoaBtn")).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(rendered_page.locator("#pcoaMetric")).to_be_visible(timeout=STEP_TIMEOUT_MS)

    log_step("Opening count matrix Barplot popup")
    with rendered_page.context.expect_page(timeout=STEP_TIMEOUT_MS) as barplot_popup:
        rendered_page.locator("#barplotBtn").click()
    barplot_page = barplot_popup.value
    barplot_page.wait_for_load_state("domcontentloaded", timeout=STEP_TIMEOUT_MS)
    expect(barplot_page).to_have_title("Unicorn Count Matrix Barplot")
    expect(rendered_page.locator("#status")).to_contain_text("Opened count matrix barplot", timeout=STEP_TIMEOUT_MS)
    barplot_page.close()

    log_step("Selecting Bray-Curtis metric for PCoA")
    rendered_page.locator("#pcoaMetric").select_option("bray_curtis")

    log_step("Opening count matrix PCoA popup")
    with rendered_page.context.expect_page(timeout=STEP_TIMEOUT_MS) as pcoa_popup:
        rendered_page.locator("#pcoaBtn").click()
    pcoa_page = pcoa_popup.value
    pcoa_page.wait_for_load_state("domcontentloaded", timeout=STEP_TIMEOUT_MS)
    expect(pcoa_page).to_have_title("Unicorn Count Matrix PCoA")
    expect(rendered_page.locator("#status")).to_contain_text("Opened count matrix PCoA", timeout=STEP_TIMEOUT_MS)
    pcoa_page.close()
