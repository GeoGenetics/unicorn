from __future__ import annotations

import os
import subprocess
import time
import json
from pathlib import Path
from typing import Iterator
from urllib.error import URLError
from urllib.request import urlopen

import pytest
from playwright.sync_api import Browser, Page, Playwright, expect, sync_playwright


GRAPHENGINE_DIR = Path(__file__).resolve().parents[2]
REPO_ROOT = Path(__file__).resolve().parents[5]
DATA_DIR = REPO_ROOT / "data"
VENV_PYTHON = GRAPHENGINE_DIR / ".venv" / "bin" / "python"
BACKEND_SCRIPT = GRAPHENGINE_DIR / "server_app.py"
BACKEND_URL = "http://127.0.0.1:8000/ping"
FRONTEND_URL = "http://127.0.0.1:8081/index.html"
TEST_LOG_DIR = GRAPHENGINE_DIR / "tests" / "logs"
BACKEND_LOG = TEST_LOG_DIR / "e2e_backend.log"
FRONTEND_LOG = TEST_LOG_DIR / "e2e_frontend.log"


def wait_for_url(url: str, timeout_seconds: float = 20.0) -> None:
    deadline = time.time() + timeout_seconds
    while time.time() < deadline:
        try:
            with urlopen(url, timeout=1.5) as response:
                if 200 <= response.status < 500:
                    return
        except URLError:
            time.sleep(0.25)
            continue
    raise RuntimeError(f"Timed out waiting for URL: {url}")


def fetch_json(url: str) -> dict:
    with urlopen(url, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


def terminate_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


@pytest.fixture()
def services() -> Iterator[dict[str, str]]:
    if not VENV_PYTHON.is_file():
        pytest.fail(f"Missing virtualenv Python: {VENV_PYTHON}")
    if not BACKEND_SCRIPT.is_file():
        pytest.fail(f"Missing backend script: {BACKEND_SCRIPT}")

    TEST_LOG_DIR.mkdir(parents=True, exist_ok=True)

    backend_log = BACKEND_LOG.open("wb")
    frontend_log = FRONTEND_LOG.open("wb")

    backend_process = subprocess.Popen(
        [str(VENV_PYTHON), str(BACKEND_SCRIPT)],
        cwd=GRAPHENGINE_DIR,
        stdout=backend_log,
        stderr=subprocess.STDOUT,
        env={**os.environ, "PYTHONUNBUFFERED": "1"},
    )
    frontend_process = subprocess.Popen(
        ["python3", "-m", "http.server", "8081"],
        cwd=GRAPHENGINE_DIR,
        stdout=frontend_log,
        stderr=subprocess.STDOUT,
        env={**os.environ, "PYTHONUNBUFFERED": "1"},
    )

    try:
        wait_for_url(BACKEND_URL)
        wait_for_url(FRONTEND_URL)
        yield {
            "backend_url": BACKEND_URL,
            "frontend_url": FRONTEND_URL,
            "backend_base_url": "http://127.0.0.1:8000",
        }
    finally:
        terminate_process(frontend_process)
        terminate_process(backend_process)
        frontend_log.close()
        backend_log.close()


@pytest.fixture(scope="session")
def playwright_instance() -> Iterator[Playwright]:
    with sync_playwright() as playwright:
        yield playwright


@pytest.fixture()
def browser(playwright_instance: Playwright) -> Iterator[Browser]:
    try:
        browser = playwright_instance.chromium.launch()
    except Exception as error:  # pragma: no cover - diagnostic path
        pytest.fail(
            "Could not launch Playwright Chromium. "
            "Install it with '.venv/bin/python -m playwright install chromium'. "
            f"Original error: {error}"
        )
    try:
        yield browser
    finally:
        browser.close()


@pytest.fixture()
def page(browser: Browser, services: dict[str, str]) -> Iterator[Page]:
    context = browser.new_context()
    page = context.new_page()
    page.goto(services["frontend_url"], wait_until="domcontentloaded")
    try:
        yield page
    finally:
        context.close()


@pytest.fixture()
def connected_page(page: Page) -> Iterator[Page]:
    expect(page.locator("h1")).to_have_text("Unicorn LCA Graph Engine")
    page.locator("#remoteUser").fill("localuser")
    page.locator("#remoteHost").fill("localhost")
    page.locator("#connectBtn").click()
    expect(page.locator("#connectionState")).to_have_text("Connected")
    expect(page.locator("#remoteDatasetsPanel")).not_to_be_hidden()
    expect(page.locator("#status")).to_contain_text("Tunnel check succeeded")
    yield page


@pytest.fixture(scope="session")
def local_taxonomy_files() -> dict[str, Path | None]:
    nodes_file = DATA_DIR / "nodes.dmp"
    names_file = DATA_DIR / "names.dmp"
    return {
        "nodes": nodes_file if nodes_file.is_file() else None,
        "names": names_file if names_file.is_file() else None,
    }


@pytest.fixture()
def backend_ping(services: dict[str, str]) -> dict:
    return fetch_json(services["backend_url"])


def ensure_taxonomy_ready(
    page: Page,
    backend_ping: dict,
    local_taxonomy_files: dict[str, Path | None],
) -> None:
    taxonomy = backend_ping.get("taxonomy") if isinstance(backend_ping, dict) else {}
    backend_nodes = taxonomy.get("nodes_file") if isinstance(taxonomy, dict) else None
    backend_names = taxonomy.get("names_file") if isinstance(taxonomy, dict) else None

    nodes_path = local_taxonomy_files.get("nodes")
    names_path = local_taxonomy_files.get("names")

    if backend_nodes and backend_names:
        return

    if not nodes_path or not names_path:
        missing = []
        if not backend_nodes and not nodes_path:
            missing.append("nodes.dmp")
        if not backend_names and not names_path:
            missing.append("names.dmp")
        pytest.fail(
            "Backend taxonomy is incomplete and local fallback taxonomy files are unavailable: "
            + ", ".join(missing)
        )

    if not backend_nodes:
        page.locator("#nodesFile").set_input_files(str(nodes_path))
    if not backend_names:
        page.locator("#namesFile").set_input_files(str(names_path))


@pytest.fixture()
def rendered_page(
    connected_page: Page,
    backend_ping: dict,
    local_taxonomy_files: dict[str, Path | None],
) -> Iterator[Page]:
    ensure_taxonomy_ready(connected_page, backend_ping, local_taxonomy_files)
    expect(connected_page.locator("#remoteDatasetsMeta")).not_to_contain_text("0 files selected")
    connected_page.locator("#renderBtn").click()
    connected_page.wait_for_function(
        "() => document.querySelectorAll('#treeSvg .node').length > 1",
        timeout=180000,
    )
    expect(connected_page.locator("#visibleCount")).not_to_have_text("0")
    expect(connected_page.locator("#status")).to_contain_text("Rendered", timeout=180000)
    yield connected_page
