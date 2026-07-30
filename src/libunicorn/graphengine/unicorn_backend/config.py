"""Runtime configuration for the Unicorn Graph Engine backend."""

from __future__ import annotations

import os
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8000
DEFAULT_NODES_FILENAME = "nodes.dmp"
DEFAULT_NAMES_FILENAME = "names.dmp"
METADATA_FILENAME = "metadata.txt"
DEFAULT_REPOSITORY_ROOT = Path(__file__).resolve().parents[4]
DEFAULT_RUNTIME_DIR = DEFAULT_REPOSITORY_ROOT / "var" / "graphengine"


@dataclass(frozen=True)
class BackendConfig:
    """Resolved backend configuration without application side effects."""

    host: str
    port: int
    runtime_dir: Path
    upload_dir: Path
    agent_trace_dir: Path
    nodes_filename: str
    names_filename: str
    metadata_filename: str

    def ensure_upload_dir(self) -> None:
        """Create the upload directory at the application startup boundary."""

        self.upload_dir.mkdir(parents=True, exist_ok=True)


def _resolve_path(value: str | Path) -> Path:
    return Path(value).expanduser().resolve()


def load_backend_config(
    environ: Mapping[str, str] | None = None,
    *,
    repository_root: str | Path | None = None,
) -> BackendConfig:
    """Load backend configuration while preserving environment precedence."""

    active_environ = os.environ if environ is None else environ
    resolved_repository_root = (
        DEFAULT_REPOSITORY_ROOT
        if repository_root is None
        else _resolve_path(repository_root)
    )
    default_runtime_dir = resolved_repository_root / "var" / "graphengine"
    runtime_dir = _resolve_path(
        active_environ.get("UNICORN_GRAPHENGINE_RUNTIME_DIR", default_runtime_dir)
    )
    upload_dir = _resolve_path(
        active_environ.get(
            "UNICORN_GRAPHENGINE_UPLOAD_DIR",
            runtime_dir / "uploads",
        )
    )
    agent_trace_dir = _resolve_path(
        active_environ.get(
            "UNICORN_GRAPHENGINE_AGENT_TRACE_DIR",
            runtime_dir / "logs" / "agent_trace",
        )
    )

    return BackendConfig(
        host=active_environ.get("UNICORN_GRAPHENGINE_HOST", DEFAULT_HOST),
        port=int(active_environ.get("UNICORN_GRAPHENGINE_PORT", str(DEFAULT_PORT))),
        runtime_dir=runtime_dir,
        upload_dir=upload_dir,
        agent_trace_dir=agent_trace_dir,
        nodes_filename=active_environ.get(
            "UNICORN_GRAPHENGINE_NODES_FILE",
            DEFAULT_NODES_FILENAME,
        ),
        names_filename=active_environ.get(
            "UNICORN_GRAPHENGINE_NAMES_FILE",
            DEFAULT_NAMES_FILENAME,
        ),
        metadata_filename=METADATA_FILENAME,
    )
