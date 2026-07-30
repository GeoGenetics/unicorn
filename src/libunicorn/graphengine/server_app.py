"""Compatibility entrypoint for historical Graph Engine startup commands.

Application construction and backend implementation live in
``unicorn_backend``. This module remains only so ``uvicorn server_app:app`` and
``python server_app.py`` continue to work.
"""

from __future__ import annotations

from unicorn_backend.app import app


def main() -> None:
    """Run the factory-created application with its resolved configuration."""

    import uvicorn

    config = app.state.backend_config
    uvicorn.run(app, host=config.host, port=config.port)


if __name__ == "__main__":
    main()
