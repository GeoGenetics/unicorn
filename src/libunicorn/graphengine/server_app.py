from __future__ import annotations

import logging
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from fastapi import Body, FastAPI, File, HTTPException, Query, UploadFile
from fastapi.middleware.cors import CORSMiddleware

from unicorn_agent.routes import create_agent_router
from unicorn_agent.runtime import create_agent_runtime
from unicorn_backend.config import DEFAULT_RUNTIME_DIR, load_backend_config
from unicorn_backend.models import (
    SelectionModel,
    TaxonomyModel,
    TreeModel,
    TreeNodeModel,
)
from unicorn_backend.store import GraphEngineStore
from unicorn_compute.barplot import build_count_matrix_barplot_spec
from unicorn_compute.pcoa import build_count_matrix_pcoa_spec


BACKEND_CONFIG = load_backend_config()
BACKEND_CONFIG.ensure_upload_dir()

# Compatibility aliases remain until server_app.py becomes the thin entrypoint.
HOST = BACKEND_CONFIG.host
PORT = BACKEND_CONFIG.port
RUNTIME_DIR = BACKEND_CONFIG.runtime_dir
UPLOAD_DIR = BACKEND_CONFIG.upload_dir
NODES_FILENAME = BACKEND_CONFIG.nodes_filename
NAMES_FILENAME = BACKEND_CONFIG.names_filename
METADATA_FILENAME = BACKEND_CONFIG.metadata_filename


app = FastAPI(title="Unicorn Graph Engine Prototype API")
LOGGER = logging.getLogger("unicorn.graphengine")


def _normalize_requested_files(files: Optional[List[str]]) -> List[str]:
    if not files:
        return []
    normalized = []
    for value in files:
        for item in value.split(","):
            item = item.strip()
            if item:
                normalized.append(Path(item).name)
    return normalized


STORE = GraphEngineStore(config=BACKEND_CONFIG)
AGENT_RUNTIME = create_agent_runtime(store=STORE)

app.include_router(
    create_agent_router(
        store=STORE,
        registry=AGENT_RUNTIME.registry_factory,
        orchestrator=AGENT_RUNTIME,
        trace_store=AGENT_RUNTIME.trace_store,
    )
)


app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get("/ping")
def ping() -> Dict[str, Any]:
    files = STORE.list_files()
    nodes_path, names_path = STORE.resolve_taxonomy_paths()
    return {
        "ok": True,
        "service": "unicorn-graphengine-prototype",
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "upload_dir": str(UPLOAD_DIR),
        "datasets": len(files),
        "taxonomy": {
            "nodes_file": nodes_path.name if nodes_path else None,
            "names_file": names_path.name if names_path else None,
        },
        "metadata": STORE.metadata_summary_payload(),
        "cache": STORE.cache_status(),
    }


@app.post("/upload")
async def upload(file: UploadFile = File(...)) -> Dict[str, Any]:
    filename = Path(file.filename or "upload.dat").name
    outpath = UPLOAD_DIR / filename
    data = await file.read()
    outpath.write_bytes(data)
    return {
        "ok": True,
        "filename": filename,
        "bytes": len(data),
        "saved_to": str(outpath),
    }


@app.post("/metadata/upload")
async def upload_metadata(file: UploadFile = File(...)) -> Dict[str, Any]:
    outpath = UPLOAD_DIR / METADATA_FILENAME
    data = await file.read()
    outpath.write_bytes(data)
    try:
        metadata = STORE.load_metadata(outpath)
    except Exception:
        try:
            outpath.unlink(missing_ok=True)
        except Exception:
            pass
        raise
    return {
        "ok": True,
        "metadata": STORE.metadata_summary_payload(),
        "saved_to": str(outpath),
    }


@app.get("/metadata/status")
def metadata_status() -> Dict[str, Any]:
    return {
        "ok": True,
        "metadata": STORE.metadata_summary_payload(),
    }


@app.get("/datasets")
def list_datasets() -> Dict[str, Any]:
    datasets = [STORE.dataset_summary_payload(STORE.get_or_load_dataset(path)) for path in STORE.list_files()]
    return {
        "ok": True,
        "datasets": datasets,
        "metadata": STORE.metadata_summary_payload(),
    }


@app.get("/taxonomy/status")
def taxonomy_status(
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
) -> Dict[str, Any]:
    taxonomy = STORE.get_or_load_taxonomy(nodes_name=nodes_file, names_name=names_file)
    return {
        "ok": True,
        "taxonomy": taxonomy.to_status_payload(),
        "cache": STORE.cache_status(),
    }


@app.get("/model/status")
def model_status(
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
) -> Dict[str, Any]:
    available = STORE.list_files()
    requested = _normalize_requested_files(files)
    selected_names = requested if requested else sorted(path.name for path in available)
    selection = STORE.build_selection(selected_names) if selected_names else None
    taxonomy = STORE.get_or_load_taxonomy(nodes_name=nodes_file, names_name=names_file)
    tree = STORE.build_tree_model(selection, taxonomy) if selection else None
    return {
        "ok": True,
        "upload_dir": str(UPLOAD_DIR),
        "available_datasets": [path.name for path in available],
        "selection": STORE.selection_status_payload(selection) if selection else {
            "dataset_count": 0,
            "datasets": [],
            "total_reads": 0,
            "total_taxa": 0,
            "direct_taxa": 0,
        },
        "metadata": STORE.metadata_summary_payload(),
        "taxonomy": taxonomy.to_status_payload(),
        "tree": tree.to_status_payload() if tree else None,
        "cache": STORE.cache_status(),
    }


@app.get("/render-data")
def render_data(files: Optional[List[str]] = Query(default=None)) -> Dict[str, Any]:
    available = STORE.list_files()
    requested = _normalize_requested_files(files)
    selected_names = requested if requested else sorted(path.name for path in available)
    if not selected_names:
        return {
            "ok": True,
            "datasets": [],
            "total_reads": 0,
            "total_taxa": 0,
        }

    selection = STORE.build_selection(selected_names)
    return {
        "ok": True,
        "datasets": [dataset.to_render_payload() for dataset in selection.datasets],
        "total_reads": selection.total_reads,
        "total_taxa": selection.total_taxa,
        "direct_taxa": len(selection.direct_counts),
    }


@app.get("/tree-model")
def tree_model(
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
) -> Dict[str, Any]:
    available = STORE.list_files()
    requested = _normalize_requested_files(files)
    selected_names = requested if requested else sorted(path.name for path in available)
    if not selected_names:
        return {
            "ok": True,
            "datasets": [],
            "taxonomy": None,
            "tree": None,
        }
    selection = STORE.build_selection(selected_names)
    taxonomy = STORE.get_or_load_taxonomy(nodes_name=nodes_file, names_name=names_file)
    tree = STORE.build_tree_model(selection, taxonomy)
    return {
        "ok": True,
        "datasets": [STORE.dataset_summary_payload(dataset) for dataset in selection.datasets],
        "taxonomy": taxonomy.to_status_payload(),
        "tree": tree.root.to_payload(),
        "missing_taxids": tree.missing_taxids,
        "metadata": STORE.metadata_summary_payload(),
        "cache": STORE.cache_status(),
    }


def _resolve_selection_and_tree(
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
) -> Tuple[SelectionModel, TaxonomyModel, TreeModel]:
    available = STORE.list_files()
    requested = _normalize_requested_files(files)
    selected_names = requested if requested else sorted(path.name for path in available)
    if not selected_names:
        raise HTTPException(
            status_code=400,
            detail={"message": "No datasets selected for tree rendering."},
        )
    selection = STORE.build_selection(selected_names)
    taxonomy = STORE.get_or_load_taxonomy(nodes_name=nodes_file, names_name=names_file)
    tree = STORE.build_tree_model(selection, taxonomy)
    return selection, taxonomy, tree


def _response_context(
    selection: SelectionModel,
    taxonomy: TaxonomyModel,
    min_reads: int,
    expanded_taxids: List[int],
) -> Dict[str, Any]:
    return {
        "dataset_names": list(selection.dataset_names),
        "nodes_file": taxonomy.nodes_fileinfo.name,
        "names_file": taxonomy.names_fileinfo.name if taxonomy.names_fileinfo else None,
        "min_reads": min_reads,
        "expanded_taxids": expanded_taxids,
    }


def _error_detail(
    message: str,
    *,
    code: str,
    request_context: Optional[Dict[str, Any]] = None,
    **extra: Any,
) -> Dict[str, Any]:
    detail: Dict[str, Any] = {
        "message": message,
        "code": code,
    }
    if request_context is not None:
        detail["request_context"] = request_context
    detail.update(extra)
    return detail


def _node_passes_filter(node: TreeNodeModel, tree: TreeModel, min_reads: int) -> bool:
    return node is tree.root or node.total >= max(0, min_reads)


def _filtered_child_count(node: TreeNodeModel, min_reads: int) -> int:
    threshold = max(0, min_reads)
    return sum(1 for child in node.children if child.total >= threshold)


def _build_lineage(node: TreeNodeModel, tree: TreeModel) -> List[Dict[str, Any]]:
    lineage: List[Dict[str, Any]] = []
    current: Optional[TreeNodeModel] = node
    while current is not None:
        lineage.append(
            {
                "taxid": current.taxid,
                "name": current.name,
                "rank": current.rank,
            }
        )
        if current is tree.root or current.parent is None:
            break
        current = tree.root.find_taxid(current.parent)
    lineage.reverse()
    return lineage


def _dataset_breakdown(selection: SelectionModel, node: TreeNodeModel) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for index, dataset in enumerate(selection.datasets):
        rows.append(
            {
                "dataset": dataset.fileinfo.name,
                "direct": node.direct_by_source[index] if index < len(node.direct_by_source) else 0,
                "subtree": node.total_by_source[index] if index < len(node.total_by_source) else 0,
            }
        )
    return rows


def _resolve_node_in_context(
    tree: TreeModel,
    taxid: int,
    min_reads: int,
    request_context: Dict[str, Any],
) -> TreeNodeModel:
    node = tree.root.find_taxid(taxid)
    if node is None:
        raise HTTPException(
            status_code=404,
            detail=_error_detail(
                "Requested node is not present in the active tree.",
                code="node_not_in_active_tree",
                request_context=request_context,
                taxid=taxid,
            ),
        )
    if not _node_passes_filter(node, tree, min_reads):
        raise HTTPException(
            status_code=409,
            detail=_error_detail(
                "Requested node exists in the active tree but is filtered out by the current min_reads threshold.",
                code="node_filtered_out",
                request_context=request_context,
                taxid=taxid,
                node_subtree_reads=node.total,
                min_reads=min_reads,
            ),
        )
    return node


def _table_rows_for_subtree(
    node: TreeNodeModel,
    min_reads: int,
    sort_by: str,
    limit: int,
) -> List[Dict[str, Any]]:
    threshold = max(0, min_reads)
    rows: List[Dict[str, Any]] = []
    stack = [node]
    while stack:
        current = stack.pop()
        if current is not node and current.total < threshold:
            continue
        if current.direct > 0:
            rows.append(
                {
                    "taxid": current.taxid,
                    "name": current.name,
                    "rank": current.rank,
                    "depth": current.depth,
                    "direct": current.direct,
                    "subtree": current.total,
                    "child_count": _filtered_child_count(current, min_reads),
                }
            )
        stack.extend(reversed(current.children))

    sort_key = "direct" if sort_by == "direct" else "subtree"
    rows.sort(key=lambda row: (-int(row[sort_key]), -int(row["direct"]), str(row["name"])))
    return rows[:limit]


def _top_children_rows(
    node: TreeNodeModel,
    min_reads: int,
    limit: int,
) -> List[Dict[str, Any]]:
    threshold = max(0, min_reads)
    rows = [
        {
            "taxid": child.taxid,
            "name": child.name,
            "rank": child.rank,
            "depth": child.depth,
            "direct": child.direct,
            "subtree": child.total,
            "child_count": _filtered_child_count(child, min_reads),
        }
        for child in node.children
        if child.total >= threshold
    ]
    rows.sort(key=lambda row: (-int(row["subtree"]), -int(row["direct"]), str(row["name"])))
    return rows[:limit]


def _normalize_taxids(values: Optional[List[int]]) -> List[int]:
    if not values:
        return []
    out: List[int] = []
    seen: set[int] = set()
    for value in values:
        try:
            taxid = int(value)
        except (TypeError, ValueError):
            continue
        if taxid in seen:
            continue
        seen.add(taxid)
        out.append(taxid)
    return out


def _selected_count_matrix_report(
    selection: "SelectionModel",
    selected_nodes: List["TreeNodeModel"],
) -> Dict[str, Any]:
    matrix_rows = []
    for node in selected_nodes:
        matrix_rows.append(
            {
                "taxid": node.taxid,
                "name": node.name,
                "rank": node.rank,
                "direct": node.direct,
                "subtree": node.total,
                "datasets": [
                    {
                        "dataset": dataset.fileinfo.name,
                        "direct": node.direct_by_source[index] if index < len(node.direct_by_source) else 0,
                        "subtree": node.total_by_source[index] if index < len(node.total_by_source) else 0,
                    }
                    for index, dataset in enumerate(selection.datasets)
                ],
            }
        )

    return {
        "summary": {
            "selected_taxids": [int(node.taxid) for node in selected_nodes],
            "selected_node_count": len(selected_nodes),
            "dataset_names": [dataset.fileinfo.name for dataset in selection.datasets],
            "total_direct": sum(int(node.direct) for node in selected_nodes),
            "total_subtree": sum(int(node.total) for node in selected_nodes),
        },
        "matrix": {
            "rows": matrix_rows,
            "dataset_names": [dataset.fileinfo.name for dataset in selection.datasets],
            "row_count": len(matrix_rows),
        },
    }


def _collect_expandable_taxids_in_context(
    node: TreeNodeModel,
    tree: TreeModel,
    min_reads: int,
    expanded_taxids: set[int],
) -> None:
    eligible_children = [
        child for child in node.children
        if _node_passes_filter(child, tree, min_reads)
    ]
    if eligible_children:
        expanded_taxids.add(node.taxid)
    for child in eligible_children:
        _collect_expandable_taxids_in_context(child, tree, min_reads, expanded_taxids)


def _visible_tree_response(
    selection: SelectionModel,
    taxonomy: TaxonomyModel,
    tree: TreeModel,
    min_reads: int,
    expanded_taxids: set[int],
) -> Dict[str, Any]:
    visible_tree, active_expanded_taxids = STORE.build_visible_tree_payload(
        tree,
        expanded_taxids=expanded_taxids,
        min_reads=min_reads,
    )
    return {
        "ok": True,
        "datasets": [STORE.dataset_summary_payload(dataset) for dataset in selection.datasets],
        "taxonomy": taxonomy.to_status_payload(),
        "tree": visible_tree,
        "missing_taxids": tree.missing_taxids,
        "expanded_taxids": active_expanded_taxids,
        "min_reads": min_reads,
        "total_reads": selection.total_reads,
        "direct_taxa": len(selection.direct_counts),
        "request_context": _response_context(selection, taxonomy, min_reads, active_expanded_taxids),
        "metadata": STORE.metadata_summary_payload(),
        "cache": STORE.cache_status(),
    }


@app.get("/root-view")
def root_view(
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
    min_reads: int = Query(default=0, ge=0),
    expanded: Optional[List[int]] = Query(default=None),
) -> Dict[str, Any]:
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    requested_expanded_taxids = set(_normalize_taxids(expanded))
    return _visible_tree_response(selection, taxonomy, tree, min_reads, requested_expanded_taxids)


@app.get("/expand-node")
def expand_node(
    taxid: int = Query(...),
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
    min_reads: int = Query(default=0, ge=0),
    expanded: Optional[List[int]] = Query(default=None),
) -> Dict[str, Any]:
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(selection, taxonomy, min_reads, sorted(set(expanded or [])))
    if not tree.root.has_taxid(taxid):
        raise HTTPException(
            status_code=404,
            detail=_error_detail(
                "Requested node is not present in the active tree.",
                code="node_not_in_active_tree",
                request_context=request_context,
                taxid=taxid,
            ),
        )
    requested_expanded_taxids = set(expanded or [])
    requested_expanded_taxids.add(taxid)
    return _visible_tree_response(selection, taxonomy, tree, min_reads, requested_expanded_taxids)


@app.get("/node-tooltip")
def node_tooltip(
    taxid: int = Query(...),
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
    min_reads: int = Query(default=0, ge=0),
) -> Dict[str, Any]:
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(selection, taxonomy, min_reads, [])
    node = _resolve_node_in_context(tree, taxid, min_reads, request_context)
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
            "child_count": _filtered_child_count(node, min_reads),
            "lineage": _build_lineage(node, tree),
            "datasets": _dataset_breakdown(selection, node),
        },
        "request_context": request_context,
    }


@app.get("/table-view")
def table_view(
    scope: str = Query(default="root"),
    taxid: Optional[int] = Query(default=None),
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
    min_reads: int = Query(default=0, ge=0),
    sort: str = Query(default="direct"),
    limit: int = Query(default=40, ge=1, le=1000),
) -> Dict[str, Any]:
    if scope not in {"root", "node"}:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "scope must be either 'root' or 'node'.",
                code="invalid_scope",
                scope=scope,
            ),
        )
    if sort not in {"direct", "subtree"}:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "sort must be either 'direct' or 'subtree'.",
                code="invalid_sort",
                sort=sort,
            ),
        )

    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(selection, taxonomy, min_reads, [])
    target = tree.root
    if scope == "node":
        if taxid is None:
            raise HTTPException(
                status_code=400,
                detail=_error_detail(
                    "taxid is required when scope='node'.",
                    code="missing_taxid",
                    request_context=request_context,
                    scope=scope,
                ),
            )
        target = _resolve_node_in_context(tree, taxid, min_reads, request_context)

    rows = _table_rows_for_subtree(target, min_reads=min_reads, sort_by=sort, limit=limit)
    return {
        "ok": True,
        "scope": scope,
        "target": {
            "taxid": target.taxid,
            "name": target.name,
            "rank": target.rank,
            "direct": target.direct,
            "subtree": target.total,
            "child_count": _filtered_child_count(target, min_reads),
        },
        "sort": sort,
        "limit": limit,
        "row_count": len(rows),
        "rows": rows,
        "request_context": request_context,
    }


@app.get("/subtree-report")
def subtree_report(
    taxid: Optional[int] = Query(default=None),
    taxids: Optional[List[int]] = Query(default=None),
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
    min_reads: int = Query(default=0, ge=0),
) -> Dict[str, Any]:
    return _subtree_report_payload(taxid, taxids, files, nodes_file, names_file, min_reads)


@app.post("/subtree-report")
def subtree_report_post(payload: Dict[str, Any] = Body(...)) -> Dict[str, Any]:
    return _subtree_report_payload(
        payload.get("taxid"),
        payload.get("taxids"),
        payload.get("files"),
        payload.get("nodes_file"),
        payload.get("names_file"),
        int(payload.get("min_reads", 0) or 0),
    )


@app.get("/rank-report")
def rank_report(
    taxids: Optional[List[int]] = Query(default=None),
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
    min_reads: int = Query(default=0, ge=0),
) -> Dict[str, Any]:
    return _rank_report_payload(taxids, files, nodes_file, names_file, min_reads)


@app.post("/rank-report")
def rank_report_post(payload: Dict[str, Any] = Body(...)) -> Dict[str, Any]:
    return _rank_report_payload(
        payload.get("taxids"),
        payload.get("files"),
        payload.get("nodes_file"),
        payload.get("names_file"),
        int(payload.get("min_reads", 0) or 0),
    )


def _subtree_report_payload(
    taxid: Optional[int],
    taxids: Optional[List[int]],
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    min_reads: int,
) -> Dict[str, Any]:
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(selection, taxonomy, min_reads, [])
    requested_taxids = _normalize_taxids(taxids)
    if taxid is not None:
        try:
            single_taxid = int(taxid)
        except (TypeError, ValueError):
            single_taxid = None
        if single_taxid is not None and single_taxid not in requested_taxids:
            requested_taxids.append(single_taxid)
    if not requested_taxids:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "At least one taxid is required for a count matrix report.",
                code="missing_taxids",
                request_context=request_context,
            ),
        )
    selected_nodes = [
        _resolve_node_in_context(tree, selected_taxid, min_reads, request_context)
        for selected_taxid in requested_taxids
    ]
    report = _selected_count_matrix_report(selection, selected_nodes)

    return {
        "ok": True,
        "report": report,
        "request_context": request_context,
    }


def _rank_report_payload(
    taxids: Optional[List[int]],
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    min_reads: int,
) -> Dict[str, Any]:
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(selection, taxonomy, min_reads, [])
    requested_taxids = _normalize_taxids(taxids)
    if not requested_taxids:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "At least one taxid is required for a rank report.",
                code="missing_taxids",
                request_context=request_context,
            ),
        )

    selected_nodes = [
        _resolve_node_in_context(tree, taxid, min_reads, request_context)
        for taxid in requested_taxids
    ]

    by_rank: Dict[str, Dict[str, Any]] = {}
    for node in selected_nodes:
        rank = node.rank or "no rank"
        slot = by_rank.setdefault(
            rank,
            {
                "rank": rank,
                "direct": 0,
                "node_count": 0,
                "datasets": [
                    {"dataset": dataset.fileinfo.name, "direct": 0}
                    for dataset in selection.datasets
                ],
            },
        )
        slot["direct"] += node.direct
        slot["node_count"] += 1
        for index, entry in enumerate(slot["datasets"]):
            entry["direct"] += node.direct_by_source[index] if index < len(node.direct_by_source) else 0

    rows = sorted(
        by_rank.values(),
        key=lambda row: (-int(row["direct"]), str(row["rank"])),
    )

    return {
        "ok": True,
        "report": {
            "summary": {
                "selected_node_count": len(selected_nodes),
                "selected_taxids": requested_taxids,
                "total_direct": sum(node.direct for node in selected_nodes),
                "dataset_names": list(selection.dataset_names),
            },
            "rows": rows,
        },
        "request_context": request_context,
    }


@app.post("/compute/barplot")
def compute_barplot(payload: Dict[str, Any] = Body(...)) -> Dict[str, Any]:
    taxids = _normalize_taxids(payload.get("taxids") if isinstance(payload, dict) else None)
    if not taxids:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "At least one taxid is required for a barplot compute request.",
                code="missing_taxids",
            ),
        )
    files = payload.get("files") if isinstance(payload.get("files"), list) else None
    nodes_file = str(payload.get("nodes_file") or "") or None
    names_file = str(payload.get("names_file") or "") or None
    min_reads_value = payload.get("min_reads", 0)
    try:
        min_reads = max(0, int(min_reads_value))
    except (TypeError, ValueError):
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "min_reads must be an integer.",
                code="invalid_min_reads",
                min_reads=min_reads_value,
            ),
        )
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(selection, taxonomy, min_reads, [])
    selected_nodes = [
        _resolve_node_in_context(tree, taxid, min_reads, request_context)
        for taxid in taxids
    ]
    report = _selected_count_matrix_report(selection, selected_nodes)
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
            detail=_error_detail(
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


@app.post("/uncollapse-to-tips")
def uncollapse_to_tips(payload: Dict[str, Any] = Body(...)) -> Dict[str, Any]:
    taxids = _normalize_taxids(payload.get("taxids") if isinstance(payload, dict) else None)
    if not taxids:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "At least one taxid is required for an uncollapse-to-tips request.",
                code="missing_taxids",
            ),
        )
    files = payload.get("files") if isinstance(payload.get("files"), list) else None
    nodes_file = str(payload.get("nodes_file") or "") or None
    names_file = str(payload.get("names_file") or "") or None
    min_reads_value = payload.get("min_reads", 0)
    try:
        min_reads = max(0, int(min_reads_value))
    except (TypeError, ValueError):
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "min_reads must be an integer.",
                code="invalid_min_reads",
                min_reads=min_reads_value,
            ),
        )
    requested_expanded_taxids = set(
        _normalize_taxids(payload.get("expanded_taxids") if isinstance(payload, dict) else None)
    )
    LOGGER.info(
        "uncollapse-to-tips start selected_taxids=%s requested_expanded_taxids=%s",
        len(taxids),
        len(requested_expanded_taxids),
    )
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(
        selection,
        taxonomy,
        min_reads,
        sorted(requested_expanded_taxids),
    )
    selected_nodes = [
        _resolve_node_in_context(tree, taxid, min_reads, request_context)
        for taxid in taxids
    ]
    for node in selected_nodes:
        _collect_expandable_taxids_in_context(node, tree, min_reads, requested_expanded_taxids)
    response = _visible_tree_response(selection, taxonomy, tree, min_reads, requested_expanded_taxids)
    LOGGER.info(
        "uncollapse-to-tips complete selected_taxids=%s active_expanded_taxids=%s",
        len(taxids),
        len(response.get("expanded_taxids") or []),
    )
    return response


@app.post("/compute/pcoa")
def compute_pcoa(payload: Dict[str, Any] = Body(...)) -> Dict[str, Any]:
    taxids = _normalize_taxids(payload.get("taxids") if isinstance(payload, dict) else None)
    if not taxids:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "At least one taxid is required for a PCoA compute request.",
                code="missing_taxids",
            ),
        )
    files = payload.get("files") if isinstance(payload.get("files"), list) else None
    nodes_file = str(payload.get("nodes_file") or "") or None
    names_file = str(payload.get("names_file") or "") or None
    min_reads_value = payload.get("min_reads", 0)
    try:
        min_reads = max(0, int(min_reads_value))
    except (TypeError, ValueError):
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "min_reads must be an integer.",
                code="invalid_min_reads",
                min_reads=min_reads_value,
            ),
        )
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(selection, taxonomy, min_reads, [])
    selected_nodes = [
        _resolve_node_in_context(tree, taxid, min_reads, request_context)
        for taxid in taxids
    ]
    report = _selected_count_matrix_report(selection, selected_nodes)
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
            detail=_error_detail(
                str(error),
                code="unimplemented_pcoa_request",
                request_context=request_context,
            ),
        )
    except ValueError as error:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
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


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host=HOST, port=PORT)
