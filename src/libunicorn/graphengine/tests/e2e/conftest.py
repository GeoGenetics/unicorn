from __future__ import annotations

import os
import shutil
import socket
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
TESTING_DIR = REPO_ROOT / "testing"
VENV_PYTHON = GRAPHENGINE_DIR / ".venv" / "bin" / "python"
BACKEND_SCRIPT = GRAPHENGINE_DIR / "server_app.py"
DATASET_FIXTURE_DIR = GRAPHENGINE_DIR / "tests" / "fixtures" / "datasets"
TEST_TAXONOMY_DIR_ENV = "UNICORN_GRAPHENGINE_TEST_TAXONOMY_DIR"
PARSER_ONLY_DATASET_FIXTURES = {
    "example43.bdamage.txt",
}


def seed_dataset_fixtures(upload_dir: Path) -> None:
    upload_dir.mkdir(parents=True, exist_ok=True)
    fixtures = sorted(
        path
        for path in DATASET_FIXTURE_DIR.glob("*.bdamage.txt")
        if path.name not in PARSER_ONLY_DATASET_FIXTURES
    )
    if not fixtures:
        pytest.fail(f"No graphengine dataset fixtures found in {DATASET_FIXTURE_DIR}")
    for source in fixtures:
        shutil.copy2(source, upload_dir / source.name)


def taxonomy_source_files() -> dict[str, Path]:
    configured = os.environ.get(TEST_TAXONOMY_DIR_ENV)
    candidates = []
    if configured:
        candidates.append(Path(configured).expanduser().resolve())
    candidates.extend((DATA_DIR, TESTING_DIR))

    for directory in candidates:
        nodes_file = directory / "nodes.dmp"
        names_file = directory / "names.dmp"
        if nodes_file.is_file() and names_file.is_file():
            return {
                "nodes": nodes_file,
                "names": names_file,
            }

    searched = ", ".join(str(path) for path in candidates)
    pytest.fail(
        "Graphengine E2E taxonomy is unavailable. Set "
        f"{TEST_TAXONOMY_DIR_ENV} to a directory containing nodes.dmp and "
        f"names.dmp. Searched: {searched}"
    )


def seed_taxonomy_fixtures(upload_dir: Path) -> dict[str, Path]:
    sources = taxonomy_source_files()
    upload_dir.mkdir(parents=True, exist_ok=True)
    for source in sources.values():
        destination = upload_dir / source.name
        try:
            destination.hardlink_to(source)
        except OSError:
            destination.symlink_to(source)
    return sources


def is_local_port_in_use(port: int, host: str = "127.0.0.1") -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(0.25)
        return sock.connect_ex((host, port)) == 0


def find_free_port(host: str = "127.0.0.1") -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        return int(sock.getsockname()[1])


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
def services(tmp_path: Path) -> Iterator[dict[str, str]]:
    if not VENV_PYTHON.is_file():
        pytest.fail(f"Missing virtualenv Python: {VENV_PYTHON}")
    if not BACKEND_SCRIPT.is_file():
        pytest.fail(f"Missing backend script: {BACKEND_SCRIPT}")

    backend_port = str(find_free_port())
    frontend_port = str(find_free_port())
    backend_url = f"http://127.0.0.1:{backend_port}/ping"
    frontend_url = f"http://127.0.0.1:{frontend_port}/index.html"
    runtime_dir = tmp_path / "runtime"
    upload_dir = runtime_dir / "uploads"
    trace_dir = runtime_dir / "logs" / "agent_trace"
    log_dir = tmp_path / "service_logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    seed_dataset_fixtures(upload_dir)
    seed_taxonomy_fixtures(upload_dir)

    backend_log_path = log_dir / "backend.log"
    frontend_log_path = log_dir / "frontend.log"
    backend_log = backend_log_path.open("wb")
    frontend_log = frontend_log_path.open("wb")

    backend_process = subprocess.Popen(
        [str(VENV_PYTHON), str(BACKEND_SCRIPT)],
        cwd=GRAPHENGINE_DIR,
        stdout=backend_log,
        stderr=subprocess.STDOUT,
        env={
            **os.environ,
            "PYTHONUNBUFFERED": "1",
            "UNICORN_GRAPHENGINE_PORT": backend_port,
            "UNICORN_GRAPHENGINE_RUNTIME_DIR": str(runtime_dir),
            "UNICORN_GRAPHENGINE_UPLOAD_DIR": str(upload_dir),
            "UNICORN_GRAPHENGINE_AGENT_TRACE_DIR": str(trace_dir),
        },
    )
    frontend_process = subprocess.Popen(
        ["python3", "-m", "http.server", frontend_port],
        cwd=GRAPHENGINE_DIR,
        stdout=frontend_log,
        stderr=subprocess.STDOUT,
        env={**os.environ, "PYTHONUNBUFFERED": "1"},
    )

    try:
        wait_for_url(backend_url)
        wait_for_url(frontend_url)
        yield {
            "backend_url": backend_url,
            "frontend_url": frontend_url,
            "backend_base_url": f"http://127.0.0.1:{backend_port}",
            "runtime_dir": str(runtime_dir),
            "upload_dir": str(upload_dir),
            "trace_dir": str(trace_dir),
            "backend_log": str(backend_log_path),
            "frontend_log": str(frontend_log_path),
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
    backend_base_url = services["backend_base_url"]
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
def local_taxonomy_files() -> dict[str, Path]:
    return taxonomy_source_files()


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
