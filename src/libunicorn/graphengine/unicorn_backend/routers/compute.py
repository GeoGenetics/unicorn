"""Barplot and PCoA compute routes."""

from __future__ import annotations

from typing import Any, Dict

from fastapi import APIRouter, Body, HTTPException

from unicorn_backend.reports import selected_count_matrix_report
from unicorn_backend.routers import error_detail, run_tree_service
from unicorn_backend.store import GraphEngineStore
from unicorn_backend.tree import (
    normalize_taxids,
    resolve_node_in_context,
    resolve_selection_and_tree,
    response_context,
)
from unicorn_compute.barplot import build_count_matrix_barplot_spec
from unicorn_compute.pcoa import build_count_matrix_pcoa_spec


def create_compute_router(*, store: GraphEngineStore) -> APIRouter:
    router = APIRouter()

    @router.post("/compute/barplot")
    def compute_barplot_route(
        payload: Dict[str, Any] = Body(...),
    ) -> Dict[str, Any]:
        return compute_barplot(payload, store=store)

    @router.post("/compute/pcoa")
    def compute_pcoa_route(
        payload: Dict[str, Any] = Body(...),
    ) -> Dict[str, Any]:
        return compute_pcoa(payload, store=store)

    return router


def compute_barplot(
    payload: Dict[str, Any],
    *,
    store: GraphEngineStore,
) -> Dict[str, Any]:
    report, request_context = _compute_report(
        payload,
        store=store,
        operation_name="barplot",
    )
    try:
        spec = build_count_matrix_barplot_spec(
            report,
            count_mode=payload.get("count_mode"),
            dataset_colors=payload.get("dataset_colors"),
            dataset_display=payload.get("dataset_display"),
        )
    except ValueError as error:
        raise HTTPException(
            status_code=400,
            detail=error_detail(
                str(error),
                code="invalid_barplot_request",
                request_context=request_context,
            ),
        )
    return {
        "ok": True,
        "spec": spec,
        "request_context": request_context,
    }


def compute_pcoa(
    payload: Dict[str, Any],
    *,
    store: GraphEngineStore,
) -> Dict[str, Any]:
    report, request_context = _compute_report(
        payload,
        store=store,
        operation_name="PCoA",
    )
    try:
        spec = build_count_matrix_pcoa_spec(
            report,
            count_mode=payload.get("count_mode"),
            distance_metric=payload.get("distance_metric"),
            dataset_colors=payload.get("dataset_colors"),
            dataset_display=payload.get("dataset_display"),
        )
    except NotImplementedError as error:
        raise HTTPException(
            status_code=501,
            detail=error_detail(
                str(error),
                code="unimplemented_pcoa_request",
                request_context=request_context,
            ),
        )
    except ValueError as error:
        raise HTTPException(
            status_code=400,
            detail=error_detail(
                str(error),
                code="invalid_pcoa_request",
                request_context=request_context,
            ),
        )
    return {
        "ok": True,
        "spec": spec,
        "request_context": request_context,
    }


def _compute_report(
    payload: Dict[str, Any],
    *,
    store: GraphEngineStore,
    operation_name: str,
) -> tuple[Dict[str, Any], Dict[str, Any]]:
    taxids = normalize_taxids(
        payload.get("taxids")
        if isinstance(payload, dict)
        else None
    )
    if not taxids:
        article = "a" if operation_name == "barplot" else "a"
        raise HTTPException(
            status_code=400,
            detail=error_detail(
                f"At least one taxid is required for {article} "
                f"{operation_name} compute request.",
                code="missing_taxids",
            ),
        )
    files = (
        payload.get("files")
        if isinstance(payload.get("files"), list)
        else None
    )
    nodes_file = str(payload.get("nodes_file") or "") or None
    names_file = str(payload.get("names_file") or "") or None
    min_reads_value = payload.get("min_reads", 0)
    try:
        min_reads = max(0, int(min_reads_value))
    except (TypeError, ValueError):
        raise HTTPException(
            status_code=400,
            detail=error_detail(
                "min_reads must be an integer.",
                code="invalid_min_reads",
                min_reads=min_reads_value,
            ),
        )
    selection, taxonomy, tree = run_tree_service(
        resolve_selection_and_tree,
        store,
        files,
        nodes_file,
        names_file,
    )
    request_context = response_context(
        selection,
        taxonomy,
        min_reads,
        [],
    )
    selected_nodes = [
        run_tree_service(
            resolve_node_in_context,
            tree,
            taxid,
            min_reads,
            request_context,
        )
        for taxid in taxids
    ]
    return (
        selected_count_matrix_report(selection, selected_nodes),
        request_context,
    )
