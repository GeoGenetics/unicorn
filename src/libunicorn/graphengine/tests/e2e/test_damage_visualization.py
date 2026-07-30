from pathlib import Path

from playwright.sync_api import Page, expect

from . import conftest as e2e_conftest

from .helpers import (
    STEP_TIMEOUT_MS,
    click_tree_node,
    expand_tree_node,
    log_step,
    wait_for_selected_count,
    wait_for_tree_label,
)
from .test_metadata_integration import (  # noqa: F401
    metadata_free_connected_page,
    metadata_free_page,
    metadata_free_services,
)


def _select_viridiplantae(page: Page) -> None:
    log_step("Waiting for 'cellular organisms' in rendered tree")
    wait_for_tree_label(page, "cellular organisms")

    log_step("Expanding 'cellular organisms'")
    expand_tree_node(page, "cellular organisms", 131567)

    log_step("Waiting for and expanding 'Eukaryota'")
    wait_for_tree_label(page, "Eukaryota")
    expand_tree_node(page, "Eukaryota", 2759)

    log_step("Selecting 'Viridiplantae' for damage visualization")
    wait_for_tree_label(page, "Viridiplantae")
    click_tree_node(page, "Viridiplantae", modifiers=["Meta"])
    wait_for_selected_count(page, "1")
    expect(page.locator("#damageBtn")).to_be_enabled(
        timeout=STEP_TIMEOUT_MS,
    )


def _open_damage_popup(
    page: Page,
    endpoint: str = "/damage/node",
):
    log_step("Opening the backend-owned damage profile")
    with page.context.expect_page(
        timeout=STEP_TIMEOUT_MS,
    ) as popup_info:
        with page.expect_response(
            lambda response: (
                endpoint in response.url
                and response.request.method == "POST"
            ),
            timeout=STEP_TIMEOUT_MS,
        ) as response_info:
            page.locator("#damageBtn").click()
    popup = popup_info.value
    popup.wait_for_load_state("domcontentloaded", timeout=STEP_TIMEOUT_MS)
    return popup, response_info.value


def _plot_dataset_displays(popup: Page) -> dict[str, dict[str, str]]:
    popup.wait_for_function(
        """
        () => {
          const plot = document.getElementById("plot");
          return Array.isArray(plot?.data) && plot.data.length > 0;
        }
        """,
        timeout=STEP_TIMEOUT_MS,
    )
    return popup.locator("#plot").evaluate(
        """
        (plot) => Object.fromEntries(
          plot.data
            .filter((trace) => trace.showlegend)
            .map((trace) => [
              String(trace.legendgroup || ""),
              {
                label: String(trace.name || ""),
                color: String(trace.marker?.color || ""),
              },
            ])
        )
        """
    )


def _resolved_dataset_displays(page: Page) -> dict[str, dict[str, str]]:
    return page.evaluate(
        """
        () => Object.fromEntries(
          UnicornGraphEngine.backend
            .getSelectedResolvedMetadataDisplays()
            .map((entry) => [
              String(entry.filename),
              {
                label: String(entry.label),
                color: String(entry.color),
              },
            ])
        )
        """
    )


def test_valid_wide_damage_upload_renders(
    metadata_free_connected_page: Page,
) -> None:
    fixture = (
        e2e_conftest.DATASET_FIXTURE_DIR
        / "example43.bdamage.txt"
    )

    log_step("Uploading the producer-generated 43-column damage fixture")
    metadata_free_connected_page.locator(
        "#lcaInputs .lca-file-input"
    ).first.set_input_files(str(fixture))
    metadata_free_connected_page.locator("#uploadBtn").click()

    expect(metadata_free_connected_page.locator("#status")).to_contain_text(
        "Uploaded 1 file",
        timeout=STEP_TIMEOUT_MS,
    )
    expect(metadata_free_connected_page.locator("#remoteDatasetsList")).to_contain_text(
        fixture.name,
        timeout=STEP_TIMEOUT_MS,
    )
    expect(metadata_free_connected_page.locator("#remoteDatasetsMeta")).to_contain_text(
        "3 of 3 files selected",
        timeout=STEP_TIMEOUT_MS,
    )

    log_step("Rendering the tree after the valid wide damage upload")
    with metadata_free_connected_page.expect_response(
        lambda response: "/root-view" in response.url,
        timeout=180000,
    ) as render_response_info:
        metadata_free_connected_page.locator("#renderBtn").click()
    render_payload = render_response_info.value.json()
    assert fixture.name in render_payload["request_context"]["dataset_names"]
    metadata_free_connected_page.wait_for_function(
        "() => document.querySelectorAll('#treeSvg .node').length > 1",
        timeout=180000,
    )
    expect(metadata_free_connected_page.locator("#status")).to_contain_text(
        "Rendered",
        timeout=180000,
    )


def test_selected_node_opens_damage_visualization(
    rendered_page: Page,
    services: dict[str, str],
) -> None:
    _select_viridiplantae(rendered_page)

    log_step("Removing one taxid profile to exercise mixed valid/missing state")
    dataset_path = (
        Path(services["upload_dir"])
        / "Lib_sim1_collapsed.alnfilt.bdamage.txt"
    )
    lines = dataset_path.read_text(encoding="utf-8").splitlines()
    dataset_path.write_text(
        "\n".join(
            line
            for line in lines
            if line.startswith("#") or line.split("\t", 1)[0] != "33090"
        ) + "\n",
        encoding="utf-8",
    )

    popup, response = _open_damage_popup(rendered_page)
    assert response.status == 200
    response_payload = response.json()
    assert response_payload["taxid"] == 33090
    assert response_payload["count_scope"] == "direct"
    assert response_payload["damage_scope"] == "subtree"
    assert sorted(
        dataset["profile_status"]
        for dataset in response_payload["datasets"]
    ) == ["missing", "valid"]

    expect(popup).to_have_title("Unicorn Damage Profile")
    expect(popup.locator("#damageSummary")).to_contain_text(
        "Damage evidence is cumulative",
        timeout=STEP_TIMEOUT_MS,
    )
    expect(popup.locator("#damageSummary")).to_contain_text(
        "Direct reads",
        timeout=STEP_TIMEOUT_MS,
    )
    expect(popup.locator(".dataset-card")).to_have_count(2)
    expect(popup.locator('[data-profile-status="valid"]')).to_have_count(1)
    expect(popup.locator('[data-profile-status="missing"]')).to_have_count(1)
    expect(popup.locator("#exportDamageBtn")).to_be_visible(
        timeout=STEP_TIMEOUT_MS,
    )

    popup.wait_for_function(
        "() => document.getElementById('plot')?.data?.length === 4",
        timeout=STEP_TIMEOUT_MS,
    )
    trace_summary = popup.locator("#plot").evaluate(
        """
        (plot) => plot.data.map((trace) => ({
          mode: trace.mode,
          symbol: trace.marker?.symbol || null,
          dash: trace.line?.dash || null,
        }))
        """
    )
    assert sorted(
        trace["symbol"]
        for trace in trace_summary
        if trace["mode"] == "markers"
    ) == ["circle", "diamond-open"]
    assert sorted(
        trace["dash"]
        for trace in trace_summary
        if trace["mode"] == "lines"
    ) == ["dash", "solid"]

    expect(rendered_page.locator("#status")).to_contain_text(
        "Opened damage profile for Viridiplantae",
        timeout=STEP_TIMEOUT_MS,
    )
    popup.close()


def test_multiple_selected_nodes_open_table_and_export(
    rendered_page: Page,
) -> None:
    _select_viridiplantae(rendered_page)

    log_step("Adding Eukaryota to the damage selection")
    click_tree_node(
        rendered_page,
        "Eukaryota",
        modifiers=["Meta"],
    )
    wait_for_selected_count(rendered_page, "2")
    expect(rendered_page.locator("#damageBtn")).to_be_enabled(
        timeout=STEP_TIMEOUT_MS,
    )

    popup, response = _open_damage_popup(
        rendered_page,
        endpoint="/damage/selected",
    )
    assert response.status == 200
    response_payload = response.json()
    assert {node["taxid"] for node in response_payload["nodes"]} == {
        2759,
        33090,
    }
    assert response_payload["count_scope"] == "direct"
    assert response_payload["damage_scope"] == "subtree"

    expect(popup).to_have_title("Unicorn Selected Damage")
    expect(popup.locator("#damageSelectedTable tbody tr")).to_have_count(4)
    expect(popup.locator("body")).to_contain_text(
        "2 selected nodes across 2 datasets",
        timeout=STEP_TIMEOUT_MS,
    )
    expect(popup.locator("#exportDamageBtn")).to_be_visible(
        timeout=STEP_TIMEOUT_MS,
    )

    log_step("Exporting normalized long-form damage TSV")
    with popup.expect_download(timeout=STEP_TIMEOUT_MS) as download_info:
        popup.locator("#exportDamageBtn").click()
    download = download_info.value
    assert download.suggested_filename == (
        "unicorn_damage_selected_2_nodes.tsv"
    )
    export_text = download.path().read_text(encoding="utf-8")
    header, *rows = export_text.splitlines()
    assert header.startswith(
        "taxid\tname\tdataset\tdataset_label\tmetadata_field"
    )
    assert "\tcount_scope\tdamage_scope\t" in header
    assert len(rows) == 20
    assert any(row.startswith("2759\tEukaryota\t") for row in rows)
    assert any(row.startswith("33090\tViridiplantae\t") for row in rows)

    expect(rendered_page.locator("#status")).to_contain_text(
        "Opened damage table for 2 selected nodes",
        timeout=STEP_TIMEOUT_MS,
    )
    popup.close()


def test_damage_reuses_active_metadata_labels_and_colors(
    metadata_free_connected_page: Page,
    tmp_path: Path,
) -> None:
    metadata_file = tmp_path / "metadata.txt"
    metadata_file.write_text(
        "\n".join([
            "dataset\tgroup\thost",
            "Lib_sim1_collapsed.bdamage.txt\tunfiltered\twheat",
            "Lib_sim1_collapsed.alnfilt.bdamage.txt\tfiltered\tbarley",
        ]),
        encoding="utf-8",
    )

    log_step("Uploading metadata for damage color reuse")
    metadata_free_connected_page.locator("#metadataFile").set_input_files(
        str(metadata_file)
    )
    metadata_free_connected_page.locator("#uploadBtn").click()
    expect(
        metadata_free_connected_page.locator("#metadataSummaryMeta")
    ).to_contain_text("2 matched", timeout=STEP_TIMEOUT_MS)

    log_step("Rendering and selecting Viridiplantae")
    metadata_free_connected_page.locator("#renderBtn").click()
    metadata_free_connected_page.wait_for_function(
        "() => document.querySelectorAll('#treeSvg .node').length > 1",
        timeout=180000,
    )
    _select_viridiplantae(metadata_free_connected_page)

    log_step("Comparing group-mode resolver displays with damage traces")
    metadata_free_connected_page.locator(
        "#metadataColorFieldSelect"
    ).select_option("group")
    expected_group = _resolved_dataset_displays(
        metadata_free_connected_page
    )
    group_popup, group_response = _open_damage_popup(
        metadata_free_connected_page
    )
    assert group_response.status == 200
    assert _plot_dataset_displays(group_popup) == expected_group
    assert all(
        "[group:" in display["label"]
        for display in expected_group.values()
    )
    group_popup.close()

    log_step("Switching to host and comparing the same shared resolver again")
    metadata_free_connected_page.locator(
        "#metadataColorFieldSelect"
    ).select_option("host")
    expected_host = _resolved_dataset_displays(
        metadata_free_connected_page
    )
    host_popup, host_response = _open_damage_popup(
        metadata_free_connected_page
    )
    assert host_response.status == 200
    assert _plot_dataset_displays(host_popup) == expected_host
    assert all(
        "[host:" in display["label"]
        for display in expected_host.values()
    )
    host_popup.close()
