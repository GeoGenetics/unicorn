"""Table and report HTTP routes."""

from __future__ import annotations

from typing import Any, Dict, List, Optional

from fastapi import APIRouter, Body, Query

from unicorn_backend.reports import (
    rank_report_payload,
    subtree_report_payload,
    table_view_payload,
)
from unicorn_backend.routers import run_report_service
from unicorn_backend.store import GraphEngineStore


def create_report_router(*, store: GraphEngineStore) -> APIRouter:
    router = APIRouter()

    @router.get("/table-view")
    def table_view_route(
        scope: str = Query(default="root"),
        taxid: Optional[int] = Query(default=None),
        files: Optional[List[str]] = Query(default=None),
        nodes_file: Optional[str] = Query(default=None),
        names_file: Optional[str] = Query(default=None),
        min_reads: int = Query(default=0, ge=0),
        sort: str = Query(default="direct"),
        limit: int = Query(default=40, ge=1, le=1000),
    ) -> Dict[str, Any]:
        return table_view(
            scope=scope,
            taxid=taxid,
            files=files,
            nodes_file=nodes_file,
            names_file=names_file,
            min_reads=min_reads,
            sort=sort,
            limit=limit,
            store=store,
        )

    @router.get("/subtree-report")
    def subtree_report_route(
        taxid: Optional[int] = Query(default=None),
        taxids: Optional[List[int]] = Query(default=None),
        files: Optional[List[str]] = Query(default=None),
        nodes_file: Optional[str] = Query(default=None),
        names_file: Optional[str] = Query(default=None),
        min_reads: int = Query(default=0, ge=0),
    ) -> Dict[str, Any]:
        return subtree_report(
            taxid=taxid,
            taxids=taxids,
            files=files,
            nodes_file=nodes_file,
            names_file=names_file,
            min_reads=min_reads,
            store=store,
        )

    @router.post("/subtree-report")
    def subtree_report_post_route(
        payload: Dict[str, Any] = Body(...),
    ) -> Dict[str, Any]:
        return subtree_report_post(payload, store=store)

    @router.get("/rank-report")
    def rank_report_route(
        taxids: Optional[List[int]] = Query(default=None),
        files: Optional[List[str]] = Query(default=None),
        nodes_file: Optional[str] = Query(default=None),
        names_file: Optional[str] = Query(default=None),
        min_reads: int = Query(default=0, ge=0),
    ) -> Dict[str, Any]:
        return rank_report(
            taxids=taxids,
            files=files,
            nodes_file=nodes_file,
            names_file=names_file,
            min_reads=min_reads,
            store=store,
        )

    @router.post("/rank-report")
    def rank_report_post_route(
        payload: Dict[str, Any] = Body(...),
    ) -> Dict[str, Any]:
        return rank_report_post(payload, store=store)

    return router


def table_view(
    *,
    scope: str,
    taxid: Optional[int],
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    min_reads: int,
    sort: str,
    limit: int,
    store: GraphEngineStore,
) -> Dict[str, Any]:
    return run_report_service(
        table_view_payload,
        store,
        scope=scope,
        taxid=taxid,
        files=files,
        nodes_file=nodes_file,
        names_file=names_file,
        min_reads=min_reads,
        sort=sort,
        limit=limit,
    )


def subtree_report(
    *,
    taxid: Optional[int],
    taxids: Optional[List[int]],
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    min_reads: int,
    store: GraphEngineStore,
) -> Dict[str, Any]:
    return run_report_service(
        subtree_report_payload,
        store,
        taxid=taxid,
        taxids=taxids,
        files=files,
        nodes_file=nodes_file,
        names_file=names_file,
        min_reads=min_reads,
    )


def subtree_report_post(
    payload: Dict[str, Any],
    *,
    store: GraphEngineStore,
) -> Dict[str, Any]:
    return subtree_report(
        taxid=payload.get("taxid"),
        taxids=payload.get("taxids"),
        files=payload.get("files"),
        nodes_file=payload.get("nodes_file"),
        names_file=payload.get("names_file"),
        min_reads=int(payload.get("min_reads", 0) or 0),
        store=store,
    )


def rank_report(
    *,
    taxids: Optional[List[int]],
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    min_reads: int,
    store: GraphEngineStore,
) -> Dict[str, Any]:
    return run_report_service(
        rank_report_payload,
        store,
        taxids=taxids,
        files=files,
        nodes_file=nodes_file,
        names_file=names_file,
        min_reads=min_reads,
    )


def rank_report_post(
    payload: Dict[str, Any],
    *,
    store: GraphEngineStore,
) -> Dict[str, Any]:
    return rank_report(
        taxids=payload.get("taxids"),
        files=payload.get("files"),
        nodes_file=payload.get("nodes_file"),
        names_file=payload.get("names_file"),
        min_reads=int(payload.get("min_reads", 0) or 0),
        store=store,
    )
