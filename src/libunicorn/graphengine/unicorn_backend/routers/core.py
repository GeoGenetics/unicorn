"""Core backend status, upload, dataset, and model routes."""

from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional

from fastapi import APIRouter, File, Query, UploadFile

from unicorn_backend.config import BackendConfig
from unicorn_backend.store import GraphEngineStore


def create_core_router(
    *,
    store: GraphEngineStore,
    config: BackendConfig,
) -> APIRouter:
    router = APIRouter()

    @router.get("/ping")
    def ping_route() -> Dict[str, Any]:
        return ping(store=store, config=config)

    @router.post("/upload")
    async def upload_route(
        file: UploadFile = File(...),
    ) -> Dict[str, Any]:
        return await upload(file, store=store, config=config)

    @router.post("/metadata/upload")
    async def upload_metadata_route(
        file: UploadFile = File(...),
    ) -> Dict[str, Any]:
        return await upload_metadata(file, store=store, config=config)

    @router.get("/metadata/status")
    def metadata_status_route() -> Dict[str, Any]:
        return metadata_status(store=store)

    @router.get("/datasets")
    def list_datasets_route() -> Dict[str, Any]:
        return list_datasets(store=store)

    @router.get("/taxonomy/status")
    def taxonomy_status_route(
        nodes_file: Optional[str] = Query(default=None),
        names_file: Optional[str] = Query(default=None),
    ) -> Dict[str, Any]:
        return taxonomy_status(
            nodes_file=nodes_file,
            names_file=names_file,
            store=store,
        )

    @router.get("/model/status")
    def model_status_route(
        files: Optional[List[str]] = Query(default=None),
        nodes_file: Optional[str] = Query(default=None),
        names_file: Optional[str] = Query(default=None),
    ) -> Dict[str, Any]:
        return model_status(
            files=files,
            nodes_file=nodes_file,
            names_file=names_file,
            store=store,
            config=config,
        )

    @router.get("/render-data")
    def render_data_route(
        files: Optional[List[str]] = Query(default=None),
    ) -> Dict[str, Any]:
        return render_data(files=files, store=store)

    @router.get("/tree-model")
    def tree_model_route(
        files: Optional[List[str]] = Query(default=None),
        nodes_file: Optional[str] = Query(default=None),
        names_file: Optional[str] = Query(default=None),
    ) -> Dict[str, Any]:
        return tree_model(
            files=files,
            nodes_file=nodes_file,
            names_file=names_file,
            store=store,
        )

    return router


def ping(
    *,
    store: GraphEngineStore,
    config: BackendConfig,
) -> Dict[str, Any]:
    files = store.list_files()
    nodes_path, names_path = store.resolve_taxonomy_paths()
    return {
        "ok": True,
        "service": "unicorn-graphengine-prototype",
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "upload_dir": str(config.upload_dir),
        "datasets": len(files),
        "taxonomy": {
            "nodes_file": nodes_path.name if nodes_path else None,
            "names_file": names_path.name if names_path else None,
        },
        "metadata": store.metadata_summary_payload(),
        "cache": store.cache_status(),
    }


async def upload(
    file: UploadFile,
    *,
    store: GraphEngineStore,
    config: BackendConfig,
) -> Dict[str, Any]:
    filename = Path(file.filename or "upload.dat").name
    outpath = config.upload_dir / filename
    data = await file.read()
    outpath.write_bytes(data)
    return {
        "ok": True,
        "filename": filename,
        "bytes": len(data),
        "saved_to": str(outpath),
    }


async def upload_metadata(
    file: UploadFile,
    *,
    store: GraphEngineStore,
    config: BackendConfig,
) -> Dict[str, Any]:
    outpath = config.upload_dir / config.metadata_filename
    data = await file.read()
    outpath.write_bytes(data)
    try:
        store.load_metadata(outpath)
    except Exception:
        try:
            outpath.unlink(missing_ok=True)
        except Exception:
            pass
        raise
    return {
        "ok": True,
        "metadata": store.metadata_summary_payload(),
        "saved_to": str(outpath),
    }


def metadata_status(*, store: GraphEngineStore) -> Dict[str, Any]:
    return {
        "ok": True,
        "metadata": store.metadata_summary_payload(),
    }


def list_datasets(*, store: GraphEngineStore) -> Dict[str, Any]:
    datasets = [
        store.dataset_summary_payload(
            store.get_or_load_dataset(path)
        )
        for path in store.list_files()
    ]
    return {
        "ok": True,
        "datasets": datasets,
        "metadata": store.metadata_summary_payload(),
    }


def taxonomy_status(
    *,
    nodes_file: Optional[str],
    names_file: Optional[str],
    store: GraphEngineStore,
) -> Dict[str, Any]:
    taxonomy = store.get_or_load_taxonomy(
        nodes_name=nodes_file,
        names_name=names_file,
    )
    return {
        "ok": True,
        "taxonomy": taxonomy.to_status_payload(),
        "cache": store.cache_status(),
    }


def model_status(
    *,
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    store: GraphEngineStore,
    config: BackendConfig,
) -> Dict[str, Any]:
    available = store.list_files()
    requested = normalize_requested_files(files)
    selected_names = (
        requested
        if requested
        else sorted(path.name for path in available)
    )
    selection = (
        store.build_selection(selected_names)
        if selected_names
        else None
    )
    taxonomy = store.get_or_load_taxonomy(
        nodes_name=nodes_file,
        names_name=names_file,
    )
    tree = (
        store.build_tree_model(selection, taxonomy)
        if selection
        else None
    )
    return {
        "ok": True,
        "upload_dir": str(config.upload_dir),
        "available_datasets": [path.name for path in available],
        "selection": (
            store.selection_status_payload(selection)
            if selection
            else {
                "dataset_count": 0,
                "datasets": [],
                "total_reads": 0,
                "total_taxa": 0,
                "direct_taxa": 0,
            }
        ),
        "metadata": store.metadata_summary_payload(),
        "taxonomy": taxonomy.to_status_payload(),
        "tree": tree.to_status_payload() if tree else None,
        "cache": store.cache_status(),
    }


def render_data(
    *,
    files: Optional[List[str]],
    store: GraphEngineStore,
) -> Dict[str, Any]:
    available = store.list_files()
    requested = normalize_requested_files(files)
    selected_names = (
        requested
        if requested
        else sorted(path.name for path in available)
    )
    if not selected_names:
        return {
            "ok": True,
            "datasets": [],
            "total_reads": 0,
            "total_taxa": 0,
        }

    selection = store.build_selection(selected_names)
    return {
        "ok": True,
        "datasets": [
            dataset.to_render_payload()
            for dataset in selection.datasets
        ],
        "total_reads": selection.total_reads,
        "total_taxa": selection.total_taxa,
        "direct_taxa": len(selection.direct_counts),
    }


def tree_model(
    *,
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    store: GraphEngineStore,
) -> Dict[str, Any]:
    available = store.list_files()
    requested = normalize_requested_files(files)
    selected_names = (
        requested
        if requested
        else sorted(path.name for path in available)
    )
    if not selected_names:
        return {
            "ok": True,
            "datasets": [],
            "taxonomy": None,
            "tree": None,
        }
    selection = store.build_selection(selected_names)
    taxonomy = store.get_or_load_taxonomy(
        nodes_name=nodes_file,
        names_name=names_file,
    )
    tree = store.build_tree_model(selection, taxonomy)
    return {
        "ok": True,
        "datasets": [
            store.dataset_summary_payload(dataset)
            for dataset in selection.datasets
        ],
        "taxonomy": taxonomy.to_status_payload(),
        "tree": tree.root.to_payload(),
        "missing_taxids": tree.missing_taxids,
        "metadata": store.metadata_summary_payload(),
        "cache": store.cache_status(),
    }


def normalize_requested_files(
    files: Optional[List[str]],
) -> List[str]:
    normalized = []
    for value in files or []:
        for item in value.split(","):
            item = item.strip()
            if item:
                normalized.append(Path(item).name)
    return normalized
