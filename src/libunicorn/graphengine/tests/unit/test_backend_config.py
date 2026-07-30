from __future__ import annotations

from pathlib import Path

import pytest

import server_app
from unicorn_backend.config import (
    DEFAULT_NAMES_FILENAME,
    DEFAULT_NODES_FILENAME,
    DEFAULT_PORT,
    METADATA_FILENAME,
    load_backend_config,
)


def test_default_config_is_pure_and_uses_repository_runtime(tmp_path: Path) -> None:
    config = load_backend_config(environ={}, repository_root=tmp_path)
    expected_runtime = (tmp_path / "var" / "graphengine").resolve()

    assert config.host == "127.0.0.1"
    assert config.port == DEFAULT_PORT
    assert config.runtime_dir == expected_runtime
    assert config.upload_dir == expected_runtime / "uploads"
    assert config.agent_trace_dir == expected_runtime / "logs" / "agent_trace"
    assert config.nodes_filename == DEFAULT_NODES_FILENAME
    assert config.names_filename == DEFAULT_NAMES_FILENAME
    assert config.metadata_filename == METADATA_FILENAME
    assert not expected_runtime.exists()


def test_ensure_upload_dir_is_an_explicit_side_effect(tmp_path: Path) -> None:
    config = load_backend_config(environ={}, repository_root=tmp_path)

    config.ensure_upload_dir()

    assert config.upload_dir.is_dir()
    assert not config.agent_trace_dir.exists()


def test_runtime_override_derives_upload_and_trace_paths(tmp_path: Path) -> None:
    runtime_dir = tmp_path / "custom-runtime"
    config = load_backend_config(
        environ={
            "UNICORN_GRAPHENGINE_HOST": "0.0.0.0",
            "UNICORN_GRAPHENGINE_PORT": "8123",
            "UNICORN_GRAPHENGINE_RUNTIME_DIR": str(runtime_dir),
            "UNICORN_GRAPHENGINE_NODES_FILE": "taxonomy.nodes",
            "UNICORN_GRAPHENGINE_NAMES_FILE": "taxonomy.names",
        },
        repository_root=tmp_path / "unused-repository",
    )

    assert config.host == "0.0.0.0"
    assert config.port == 8123
    assert config.runtime_dir == runtime_dir.resolve()
    assert config.upload_dir == runtime_dir.resolve() / "uploads"
    assert config.agent_trace_dir == runtime_dir.resolve() / "logs" / "agent_trace"
    assert config.nodes_filename == "taxonomy.nodes"
    assert config.names_filename == "taxonomy.names"


def test_upload_and_trace_overrides_take_precedence(tmp_path: Path) -> None:
    runtime_dir = tmp_path / "runtime"
    upload_dir = tmp_path / "external-uploads"
    trace_dir = tmp_path / "external-traces"
    config = load_backend_config(
        environ={
            "UNICORN_GRAPHENGINE_RUNTIME_DIR": str(runtime_dir),
            "UNICORN_GRAPHENGINE_UPLOAD_DIR": str(upload_dir),
            "UNICORN_GRAPHENGINE_AGENT_TRACE_DIR": str(trace_dir),
        }
    )

    assert config.runtime_dir == runtime_dir.resolve()
    assert config.upload_dir == upload_dir.resolve()
    assert config.agent_trace_dir == trace_dir.resolve()


def test_relative_paths_resolve_from_current_directory(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    monkeypatch.chdir(tmp_path)

    config = load_backend_config(
        environ={
            "UNICORN_GRAPHENGINE_RUNTIME_DIR": "runtime",
            "UNICORN_GRAPHENGINE_UPLOAD_DIR": "uploads",
            "UNICORN_GRAPHENGINE_AGENT_TRACE_DIR": "traces",
        }
    )

    assert config.runtime_dir == (tmp_path / "runtime").resolve()
    assert config.upload_dir == (tmp_path / "uploads").resolve()
    assert config.agent_trace_dir == (tmp_path / "traces").resolve()


def test_invalid_port_fails_during_configuration_loading() -> None:
    with pytest.raises(ValueError):
        load_backend_config(
            environ={"UNICORN_GRAPHENGINE_PORT": "not-a-port"}
        )


def test_server_app_exposes_configuration_compatibility_aliases() -> None:
    config = server_app.BACKEND_CONFIG

    assert server_app.HOST == config.host
    assert server_app.PORT == config.port
    assert server_app.RUNTIME_DIR == config.runtime_dir
    assert server_app.UPLOAD_DIR == config.upload_dir
    assert server_app.NODES_FILENAME == config.nodes_filename
    assert server_app.NAMES_FILENAME == config.names_filename
    assert server_app.METADATA_FILENAME == config.metadata_filename
    assert server_app.UPLOAD_DIR.is_dir()
