import os
import shutil
import socket
import subprocess
from pathlib import Path
from typing import Iterator

import pytest
from playwright.sync_api import Browser, Page, expect

from . import conftest as e2e_conftest
from .helpers import STEP_TIMEOUT_MS, log_step


def _find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


@pytest.fixture()
def metadata_free_services(tmp_path: Path) -> Iterator[dict[str, str]]:
    backend_port = str(_find_free_port())
    frontend_port = str(_find_free_port())
    backend_url = f"http://127.0.0.1:{backend_port}/ping"
    frontend_url = f"http://127.0.0.1:{frontend_port}/index.html"
    upload_dir = tmp_path / "uploads"
    upload_dir.mkdir(parents=True, exist_ok=True)

    for source in (e2e_conftest.GRAPHENGINE_DIR / "uploads").iterdir():
        if source.name == "metadata.txt":
            continue
        if source.is_file():
            shutil.copy2(source, upload_dir / source.name)

    e2e_conftest.TEST_LOG_DIR.mkdir(parents=True, exist_ok=True)
    backend_log = e2e_conftest.BACKEND_LOG.open("wb")
    frontend_log = e2e_conftest.FRONTEND_LOG.open("wb")

    backend_process = subprocess.Popen(
        [str(e2e_conftest.VENV_PYTHON), str(e2e_conftest.BACKEND_SCRIPT)],
        cwd=e2e_conftest.GRAPHENGINE_DIR,
        stdout=backend_log,
        stderr=subprocess.STDOUT,
        env={
            **os.environ,
            "PYTHONUNBUFFERED": "1",
            "UNICORN_GRAPHENGINE_PORT": backend_port,
            "UNICORN_GRAPHENGINE_UPLOAD_DIR": str(upload_dir),
        },
    )
    frontend_process = subprocess.Popen(
        ["python3", "-m", "http.server", frontend_port],
        cwd=e2e_conftest.GRAPHENGINE_DIR,
        stdout=frontend_log,
        stderr=subprocess.STDOUT,
        env={**os.environ, "PYTHONUNBUFFERED": "1"},
    )

    try:
        e2e_conftest.wait_for_url(backend_url)
        e2e_conftest.wait_for_url(frontend_url)
        yield {
            "backend_url": backend_url,
            "frontend_url": frontend_url,
            "backend_base_url": f"http://127.0.0.1:{backend_port}",
        }
    finally:
        e2e_conftest.terminate_process(frontend_process)
        e2e_conftest.terminate_process(backend_process)
        frontend_log.close()
        backend_log.close()


@pytest.fixture()
def metadata_free_page(browser: Browser, metadata_free_services: dict[str, str]) -> Iterator[Page]:
    context = browser.new_context()
    backend_base_url = metadata_free_services["backend_base_url"]
    context.add_init_script(
        script=f"""
        (() => {{
          const backendBaseUrl = {backend_base_url!r};
          const originalFetch = window.fetch.bind(window);
          const rewriteUrl = (value) => {{
            const source = String(value);
            return source
              .replace("http://localhost:8000", backendBaseUrl)
              .replace("http://127.0.0.1:8000", backendBaseUrl);
          }};
          window.fetch = (input, init) => {{
            if (typeof input === "string") {{
              return originalFetch(rewriteUrl(input), init);
            }}
            if (input instanceof Request) {{
              return originalFetch(new Request(rewriteUrl(input.url), input), init);
            }}
            return originalFetch(input, init);
          }};
        }})();
        """,
    )
    page = context.new_page()
    page.goto(metadata_free_services["frontend_url"], wait_until="domcontentloaded")
    try:
        yield page
    finally:
        context.close()


@pytest.fixture()
def metadata_free_connected_page(metadata_free_page: Page) -> Iterator[Page]:
    expect(metadata_free_page.locator("h1")).to_have_text("Unicorn LCA Graph Engine")
    metadata_free_page.locator("#remoteUser").fill("localuser")
    metadata_free_page.locator("#remoteHost").fill("localhost")
    metadata_free_page.locator("#connectBtn").click()
    expect(metadata_free_page.locator("#connectionState")).to_have_text("Connected")
    expect(metadata_free_page.locator("#remoteDatasetsPanel")).not_to_be_hidden()
    expect(metadata_free_page.locator("#status")).to_contain_text("Tunnel check succeeded")
    yield metadata_free_page


def test_render_tree_without_metadata_loaded(metadata_free_connected_page: Page) -> None:
    log_step("Checking metadata panel state before any metadata file is uploaded")
    expect(metadata_free_connected_page.locator("#metadataSummaryMeta")).to_contain_text(
        "No backend metadata loaded",
        timeout=STEP_TIMEOUT_MS,
    )

    log_step("Rendering the tree with datasets and taxonomy but no metadata table")
    metadata_free_connected_page.locator("#renderBtn").click()
    metadata_free_connected_page.wait_for_function(
        "() => document.querySelectorAll('#treeSvg .node').length > 1",
        timeout=180000,
    )

    expect(metadata_free_connected_page.locator("#visibleCount")).not_to_have_text("0")
    expect(metadata_free_connected_page.locator("#metadataSummaryMeta")).to_contain_text(
        "No backend metadata loaded",
        timeout=STEP_TIMEOUT_MS,
    )


def test_metadata_upload_missing_dataset_column_fails_cleanly(
    metadata_free_connected_page: Page,
    tmp_path: Path,
) -> None:
    invalid_metadata_file = tmp_path / "metadata-invalid.txt"
    invalid_metadata_file.write_text(
        "\n".join([
            "group\thost\tsite",
            "metadata-e2e-group-a\twheat\te2e-greenhouse",
        ]),
        encoding="utf-8",
    )

    log_step("Uploading an invalid metadata table without the required dataset column")
    metadata_free_connected_page.locator("#metadataFile").set_input_files(str(invalid_metadata_file))
    metadata_free_connected_page.locator("#uploadBtn").click()

    expect(metadata_free_connected_page.locator("#status")).to_contain_text(
        "Could not upload files to the backend: Metadata file must contain a 'dataset' header column.",
        timeout=STEP_TIMEOUT_MS,
    )
    expect(metadata_free_connected_page.locator("#metadataSummaryMeta")).to_contain_text(
        "No backend metadata loaded",
        timeout=STEP_TIMEOUT_MS,
    )

    log_step("Confirming the session still renders normally after the invalid metadata upload")
    metadata_free_connected_page.locator("#renderBtn").click()
    metadata_free_connected_page.wait_for_function(
        "() => document.querySelectorAll('#treeSvg .node').length > 1",
        timeout=180000,
    )
    expect(metadata_free_connected_page.locator("#visibleCount")).not_to_have_text("0")


def test_metadata_upload_and_selected_dataset_rendering(
    metadata_free_connected_page: Page,
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
    metadata_free_connected_page.locator("#metadataFile").set_input_files(str(metadata_file))
    metadata_free_connected_page.locator("#uploadBtn").click()

    log_step("Waiting for backend metadata summary to reflect the uploaded file")
    expect(metadata_free_connected_page.locator("#status")).to_contain_text("Uploaded 1 file", timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataSummaryMeta")).to_contain_text("2 matched", timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataSummaryMeta")).to_contain_text("3 fields", timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataSummaryMeta")).to_contain_text("1 unmatched", timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataSummaryFields")).to_contain_text("metadata.txt", timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataSummaryFields")).to_contain_text("group, host, site", timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataFieldSelect")).to_be_visible(timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataFieldColorList")).to_contain_text("group", timeout=STEP_TIMEOUT_MS)

    log_step("Checking that selected datasets render uploaded metadata values")
    expect(metadata_free_connected_page.locator("#metadataSelectedMeta")).to_contain_text("2 of", timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataSelectedList")).to_contain_text("Lib_sim1_collapsed.bdamage.txt", timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataSelectedList")).to_contain_text("Lib_sim1_collapsed.alnfilt.bdamage.txt", timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataSelectedList")).to_contain_text("metadata-e2e-group-a", timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataSelectedList")).to_contain_text("metadata-e2e-group-b", timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataSelectedList")).to_contain_text("e2e-greenhouse", timeout=STEP_TIMEOUT_MS)

    log_step("Changing dataset selection to verify metadata rendering refreshes live")
    metadata_free_connected_page.locator("#remoteClearAllBtn").click()
    expect(metadata_free_connected_page.locator("#metadataSelectedMeta")).to_contain_text("No selected datasets yet", timeout=STEP_TIMEOUT_MS)

    target_dataset = metadata_free_connected_page.locator(".remote-dataset-item").filter(
        has=metadata_free_connected_page.locator(".remote-dataset-name", has_text="Lib_sim1_collapsed.bdamage.txt")
    ).first
    target_dataset.locator('input[type="checkbox"]').check(force=True)

    expect(metadata_free_connected_page.locator("#metadataSelectedMeta")).to_contain_text("1 of 1 selected dataset matched metadata", timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataSelectedList")).to_contain_text("Lib_sim1_collapsed.bdamage.txt", timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataSelectedList")).to_contain_text("metadata-e2e-group-a", timeout=STEP_TIMEOUT_MS)
    expect(metadata_free_connected_page.locator("#metadataSelectedList")).not_to_contain_text("metadata-e2e-group-b", timeout=STEP_TIMEOUT_MS)
