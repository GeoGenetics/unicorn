"""Tree-view HTTP routes."""

from __future__ import annotations

import logging
from typing import Any, Dict, List, Optional

from fastapi import APIRouter, Body, HTTPException, Query

from unicorn_backend.routers import error_detail, run_tree_service
from unicorn_backend.store import GraphEngineStore
from unicorn_backend.tree import (
    build_lineage,
    collect_expandable_taxids,
    dataset_breakdown,
    filtered_child_count,
    normalize_taxids,
    require_node_present,
    resolve_node_in_context,
    resolve_selection_and_tree,
    response_context,
    visible_tree_response,
)


LOGGER = logging.getLogger("unicorn.graphengine")


def create_tree_router(*, store: GraphEngineStore) -> APIRouter:
    router = APIRouter()

    @router.get("/root-view")
    def root_view_route(
        files: Optional[List[str]] = Query(default=None),
        nodes_file: Optional[str] = Query(default=None),
        names_file: Optional[str] = Query(default=None),
        min_reads: int = Query(default=0, ge=0),
        expanded: Optional[List[int]] = Query(default=None),
        taxonomy_only: bool = Query(default=False),
    ) -> Dict[str, Any]:
        return root_view(
            files=files,
            nodes_file=nodes_file,
            names_file=names_file,
            min_reads=min_reads,
            expanded=expanded,
            taxonomy_only=taxonomy_only,
            store=store,
        )

    @router.post("/tree-view")
    def tree_view_route(
        payload: Dict[str, Any] = Body(...),
    ) -> Dict[str, Any]:
        return tree_view(payload, store=store)

    @router.get("/expand-node")
    def expand_node_route(
        taxid: int = Query(...),
        files: Optional[List[str]] = Query(default=None),
        nodes_file: Optional[str] = Query(default=None),
        names_file: Optional[str] = Query(default=None),
        min_reads: int = Query(default=0, ge=0),
        expanded: Optional[List[int]] = Query(default=None),
        taxonomy_only: bool = Query(default=False),
    ) -> Dict[str, Any]:
        return expand_node(
            taxid=taxid,
            files=files,
            nodes_file=nodes_file,
            names_file=names_file,
            min_reads=min_reads,
            expanded=expanded,
            taxonomy_only=taxonomy_only,
            store=store,
        )

    @router.get("/node-tooltip")
    def node_tooltip_route(
        taxid: int = Query(...),
        files: Optional[List[str]] = Query(default=None),
        nodes_file: Optional[str] = Query(default=None),
        names_file: Optional[str] = Query(default=None),
        min_reads: int = Query(default=0, ge=0),
        taxonomy_only: bool = Query(default=False),
    ) -> Dict[str, Any]:
        return node_tooltip(
            taxid=taxid,
            files=files,
            nodes_file=nodes_file,
            names_file=names_file,
            min_reads=min_reads,
            taxonomy_only=taxonomy_only,
            store=store,
        )

    @router.post("/uncollapse-to-tips")
    def uncollapse_to_tips_route(
        payload: Dict[str, Any] = Body(...),
    ) -> Dict[str, Any]:
        return uncollapse_to_tips(payload, store=store)

    return router


def root_view(
    *,
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    min_reads: int,
    expanded: Optional[List[int]],
    taxonomy_only: bool = False,
    store: GraphEngineStore,
) -> Dict[str, Any]:
    selection, taxonomy, tree = _resolve_context(
        store,
        files,
        nodes_file,
        names_file,
        taxonomy_only,
    )
    requested_expanded_taxids = set(normalize_taxids(expanded))
    return _visible_response(
        store,
        selection,
        taxonomy,
        tree,
        min_reads,
        requested_expanded_taxids,
    )


def tree_view(
    payload: Dict[str, Any],
    *,
    store: GraphEngineStore,
) -> Dict[str, Any]:
    files_value = payload.get("files")
    if files_value is not None and not isinstance(files_value, list):
        raise HTTPException(
            status_code=400,
            detail=error_detail(
                "files must be an array when supplied.",
                code="invalid_files",
            ),
        )
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
    return root_view(
        files=files_value,
        nodes_file=str(payload.get("nodes_file") or "") or None,
        names_file=str(payload.get("names_file") or "") or None,
        min_reads=min_reads,
        expanded=normalize_taxids(payload.get("expanded_taxids")),
        taxonomy_only=bool(payload.get("taxonomy_only", False)),
        store=store,
    )


def expand_node(
    *,
    taxid: int,
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    min_reads: int,
    expanded: Optional[List[int]],
    taxonomy_only: bool = False,
    store: GraphEngineStore,
) -> Dict[str, Any]:
    selection, taxonomy, tree = _resolve_context(
        store,
        files,
        nodes_file,
        names_file,
        taxonomy_only,
    )
    request_context = response_context(
        selection,
        taxonomy,
        min_reads,
        sorted(set(expanded or [])),
    )
    run_tree_service(
        require_node_present,
        tree,
        taxid,
        request_context,
    )
    requested_expanded_taxids = set(expanded or [])
    requested_expanded_taxids.add(taxid)
    return _visible_response(
        store,
        selection,
        taxonomy,
        tree,
        min_reads,
        requested_expanded_taxids,
    )


def node_tooltip(
    *,
    taxid: int,
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    min_reads: int,
    taxonomy_only: bool = False,
    store: GraphEngineStore,
) -> Dict[str, Any]:
    selection, taxonomy, tree = _resolve_context(
        store,
        files,
        nodes_file,
        names_file,
        taxonomy_only,
    )
    request_context = response_context(
        selection,
        taxonomy,
        min_reads,
        [],
    )
    node = run_tree_service(
        resolve_node_in_context,
        tree,
        taxid,
        min_reads,
        request_context,
    )
    return {
        "ok": True,
        "node": {
            "taxid": node.taxid,
            "name": node.name,
            "rank": node.rank,
            "parent": node.parent,
            "depth": node.depth,
            "direct": node.direct,
            "subtree": node.total,
            "child_count": filtered_child_count(node, min_reads, tree),
            "lineage": build_lineage(node, tree),
            "datasets": dataset_breakdown(selection, node),
        },
        "request_context": request_context,
    }


def uncollapse_to_tips(
    payload: Dict[str, Any],
    *,
    store: GraphEngineStore,
) -> Dict[str, Any]:
    taxids = normalize_taxids(
        payload.get("taxids")
        if isinstance(payload, dict)
        else None
    )
    if not taxids:
        raise HTTPException(
            status_code=400,
            detail=error_detail(
                "At least one taxid is required for an "
                "uncollapse-to-tips request.",
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
    taxonomy_only = bool(payload.get("taxonomy_only", False))
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
    requested_expanded_taxids = set(
        normalize_taxids(payload.get("expanded_taxids"))
    )
    LOGGER.info(
        "uncollapse-to-tips start selected_taxids=%s "
        "requested_expanded_taxids=%s",
        len(taxids),
        len(requested_expanded_taxids),
    )
    selection, taxonomy, tree = _resolve_context(
        store,
        files,
        nodes_file,
        names_file,
        taxonomy_only,
    )
    request_context = response_context(
        selection,
        taxonomy,
        min_reads,
        sorted(requested_expanded_taxids),
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
    for node in selected_nodes:
        collect_expandable_taxids(
            node,
            tree,
            min_reads,
            requested_expanded_taxids,
        )
    response = _visible_response(
        store,
        selection,
        taxonomy,
        tree,
        min_reads,
        requested_expanded_taxids,
    )
    LOGGER.info(
        "uncollapse-to-tips complete selected_taxids=%s "
        "active_expanded_taxids=%s",
        len(taxids),
        len(response.get("expanded_taxids") or []),
    )
    return response


def _resolve_context(
    store: GraphEngineStore,
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    taxonomy_only: bool = False,
):
    return run_tree_service(
        resolve_selection_and_tree,
        store,
        files,
        nodes_file,
        names_file,
        taxonomy_only,
    )


def _visible_response(
    store: GraphEngineStore,
    selection,
    taxonomy,
    tree,
    min_reads: int,
    expanded_taxids: set[int],
) -> Dict[str, Any]:
    return run_tree_service(
        visible_tree_response,
        store,
        selection,
        taxonomy,
        tree,
        min_reads,
        expanded_taxids,
    )
