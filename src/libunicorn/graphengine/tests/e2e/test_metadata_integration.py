from pathlib import Path

from playwright.sync_api import Page, expect

from .helpers import STEP_TIMEOUT_MS, log_step


def test_metadata_upload_and_selected_dataset_rendering(
    connected_page: Page,
    tmp_path: Path,
) -> None:
    metadata_file = tmp_path / "metadata.txt"
    metadata_file.write_text(
        "\n".join([
            "dataset\tgroup\thost\tsite",
            "Lib_sim1_collapsed.bdamage.txt\tmetadata-e2e-group-a\twheat\te2e-greenhouse",
            "Lib_sim1_collapsed.alnfilt.bdamage.txt\tmetadata-e2e-group-b\twheat\te2e-greenhouse",
            "unmatched_dataset.bdamage.txt\tmetadata-e2e-unmatched\tghost\tnowhere",
        ]),
        encoding="utf-8",
    )

    log_step("Uploading a metadata table through the UI")
    connected_page.locator("#metadataFile").set_input_files(str(metadata_file))
    connected_page.locator("#uploadBtn").click()

    log_step("Waiting for backend metadata summary to reflect the uploaded file")
    expect(connected_page.locator("#status")).to_contain_text("Uploaded 1 file", timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataSummaryMeta")).to_contain_text("2 matched", timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataSummaryMeta")).to_contain_text("3 fields", timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataSummaryMeta")).to_contain_text("1 unmatched", timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataSummaryFields")).to_contain_text("metadata.txt", timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataSummaryFields")).to_contain_text("group, host, site", timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataFieldSelect")).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataFieldColorList")).to_contain_text("group", timeout=STEP_TIMEOUT_MS)

    log_step("Checking that selected datasets render uploaded metadata values")
    expect(connected_page.locator("#metadataSelectedMeta")).to_contain_text("2 of", timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataSelectedList")).to_contain_text("Lib_sim1_collapsed.bdamage.txt", timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataSelectedList")).to_contain_text("Lib_sim1_collapsed.alnfilt.bdamage.txt", timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataSelectedList")).to_contain_text("metadata-e2e-group-a", timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataSelectedList")).to_contain_text("metadata-e2e-group-b", timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataSelectedList")).to_contain_text("e2e-greenhouse", timeout=STEP_TIMEOUT_MS)

    log_step("Changing dataset selection to verify metadata rendering refreshes live")
    connected_page.locator("#remoteClearAllBtn").click()
    expect(connected_page.locator("#metadataSelectedMeta")).to_contain_text("No selected datasets yet", timeout=STEP_TIMEOUT_MS)

    target_dataset = connected_page.locator(".remote-dataset-item").filter(
        has=connected_page.locator(".remote-dataset-name", has_text="Lib_sim1_collapsed.bdamage.txt")
    ).first
    target_dataset.locator('input[type="checkbox"]').check(force=True)

    expect(connected_page.locator("#metadataSelectedMeta")).to_contain_text("1 of 1 selected dataset matched metadata", timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataSelectedList")).to_contain_text("Lib_sim1_collapsed.bdamage.txt", timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataSelectedList")).to_contain_text("metadata-e2e-group-a", timeout=STEP_TIMEOUT_MS)
    expect(connected_page.locator("#metadataSelectedList")).not_to_contain_text("metadata-e2e-group-b", timeout=STEP_TIMEOUT_MS)
