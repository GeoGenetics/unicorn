"""Application composition for the Unicorn Graph Engine backend."""

from __future__ import annotations

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from unicorn_agent.routes import create_agent_router
from unicorn_agent.runtime import create_agent_runtime
from unicorn_agent.tracing import JsonlTraceStore
from unicorn_backend.config import BackendConfig, load_backend_config
from unicorn_backend.routers.compute import create_compute_router
from unicorn_backend.routers.core import create_core_router
from unicorn_backend.routers.damage import create_damage_router
from unicorn_backend.routers.reports import create_report_router
from unicorn_backend.routers.tree import create_tree_router
from unicorn_backend.store import GraphEngineStore


def create_app(config: BackendConfig | None = None) -> FastAPI:
    """Create one isolated backend application and dependency graph."""

    active_config = config or load_backend_config()
    active_config.ensure_upload_dir()

    store = GraphEngineStore(config=active_config)
    agent_runtime = create_agent_runtime(
        store=store,
        trace_store=JsonlTraceStore(active_config.agent_trace_dir),
    )

    application = FastAPI(
        title="Unicorn Graph Engine Prototype API"
    )
    application.state.backend_config = active_config
    application.state.graphengine_store = store
    application.state.agent_runtime = agent_runtime

    application.include_router(
        create_agent_router(
            store=store,
            registry=agent_runtime.registry_factory,
            orchestrator=agent_runtime,
            trace_store=agent_runtime.trace_store,
        )
    )
    application.include_router(
        create_core_router(
            store=store,
            config=active_config,
        )
    )
    application.include_router(create_tree_router(store=store))
    application.include_router(create_report_router(store=store))
    application.include_router(create_damage_router(store=store))
    application.include_router(create_compute_router(store=store))
    application.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
    )
    return application


app = create_app()
