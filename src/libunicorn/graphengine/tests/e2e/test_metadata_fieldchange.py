from pathlib import Path

from playwright.sync_api import Page, expect

from .helpers import STEP_TIMEOUT_MS, log_step
from .test_metadata_integration import (  # noqa: F401
    metadata_free_connected_page,
    metadata_free_page,
    metadata_free_services,
)


def test_metadata_legend_switches_with_active_field(
    metadata_free_connected_page: Page,
    tmp_path: Path,
) -> None:
    metadata_file = tmp_path / "metadata.txt"
    metadata_file.write_text(
        "\n".join([
            "dataset\tgroup\thost\tsite",
            "Lib_sim1_collapsed.bdamage.txt\tlegend-group-alpha\tlegend-host-wheat\te2e-greenhouse-a",
            "Lib_sim1_collapsed.alnfilt.bdamage.txt\tlegend-group-beta\tlegend-host-barley\te2e-greenhouse-b",
        ]),
        encoding="utf-8",
    )

    log_step("Uploading metadata for active-field legend switching")
    metadata_free_connected_page.locator("#metadataFile").set_input_files(str(metadata_file))
    metadata_free_connected_page.locator("#uploadBtn").click()
    expect(metadata_free_connected_page.locator("#metadataSummaryMeta")).to_contain_text("2 matched", timeout=STEP_TIMEOUT_MS)

    log_step("Rendering the backend tree so the main source legend becomes available")
    metadata_free_connected_page.locator("#renderBtn").click()
    metadata_free_connected_page.wait_for_function(
        "() => document.querySelectorAll('#treeSvg .node').length > 1",
        timeout=180000,
    )

    source_legend = metadata_free_connected_page.locator("#sourceLegend")

    log_step("Confirming the legend defaults to the first metadata field")
    expect(source_legend).to_contain_text("Coloring by group", timeout=STEP_TIMEOUT_MS)
    expect(source_legend).to_contain_text("legend-group-alpha", timeout=STEP_TIMEOUT_MS)
    expect(source_legend).to_contain_text("legend-group-beta", timeout=STEP_TIMEOUT_MS)

    log_step("Switching the active metadata color field from group to host")
    metadata_free_connected_page.locator("#metadataColorFieldSelect").select_option("host")

    expect(source_legend).to_contain_text("Coloring by host", timeout=STEP_TIMEOUT_MS)
    expect(source_legend).to_contain_text("legend-host-wheat", timeout=STEP_TIMEOUT_MS)
    expect(source_legend).to_contain_text("legend-host-barley", timeout=STEP_TIMEOUT_MS)
    expect(source_legend).not_to_contain_text("Coloring by group", timeout=STEP_TIMEOUT_MS)
    expect(source_legend).not_to_contain_text("legend-group-alpha", timeout=STEP_TIMEOUT_MS)
