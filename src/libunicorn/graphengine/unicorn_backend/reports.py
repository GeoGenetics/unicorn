"""Table and report services for Unicorn Graph Engine."""

from __future__ import annotations

from typing import Any, Dict, List, Optional

from unicorn_backend.models import SelectionModel, TreeNodeModel
from unicorn_backend.store import GraphEngineStore
from unicorn_backend.tree import (
    filtered_child_count,
    normalize_taxids,
    resolve_node_in_context,
    resolve_selection_and_tree,
    response_context,
)


class ReportServiceError(Exception):
    """A report-domain failure awaiting HTTP mapping at the route boundary."""

    def __init__(self, status_code: int, detail: Any) -> None:
        super().__init__(
            detail.get("message")
            if isinstance(detail, dict)
            else str(detail)
        )
        self.status_code = status_code
        self.detail = detail


def subtree_table_rows(
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
                    "child_count": filtered_child_count(
                        current,
                        min_reads,
                    ),
                }
            )
        stack.extend(reversed(current.children))

    sort_key = "direct" if sort_by == "direct" else "subtree"
    rows.sort(
        key=lambda row: (
            -int(row[sort_key]),
            -int(row["direct"]),
            str(row["name"]),
        )
    )
    return rows[:limit]


def top_children_rows(
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
            "child_count": filtered_child_count(
                child,
                min_reads,
            ),
        }
        for child in node.children
        if child.total >= threshold
    ]
    rows.sort(
        key=lambda row: (
            -int(row["subtree"]),
            -int(row["direct"]),
            str(row["name"]),
        )
    )
    return rows[:limit]


def selected_count_matrix_report(
    selection: SelectionModel,
    selected_nodes: List[TreeNodeModel],
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
                        "direct": (
                            node.direct_by_source[index]
                            if index < len(node.direct_by_source)
                            else 0
                        ),
                        "subtree": (
                            node.total_by_source[index]
                            if index < len(node.total_by_source)
                            else 0
                        ),
                    }
                    for index, dataset in enumerate(selection.datasets)
                ],
            }
        )

    return {
        "summary": {
            "selected_taxids": [
                int(node.taxid)
                for node in selected_nodes
            ],
            "selected_node_count": len(selected_nodes),
            "dataset_names": [
                dataset.fileinfo.name
                for dataset in selection.datasets
            ],
            "total_direct": sum(
                int(node.direct)
                for node in selected_nodes
            ),
            "total_subtree": sum(
                int(node.total)
                for node in selected_nodes
            ),
        },
        "matrix": {
            "rows": matrix_rows,
            "dataset_names": [
                dataset.fileinfo.name
                for dataset in selection.datasets
            ],
            "row_count": len(matrix_rows),
        },
    }


def table_view_payload(
    store: GraphEngineStore,
    *,
    scope: str,
    taxid: Optional[int],
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    min_reads: int,
    sort: str,
    limit: int,
) -> Dict[str, Any]:
    if scope not in {"root", "node"}:
        raise ReportServiceError(
            400,
            _error_detail(
                "scope must be either 'root' or 'node'.",
                code="invalid_scope",
                scope=scope,
            ),
        )
    if sort not in {"direct", "subtree"}:
        raise ReportServiceError(
            400,
            _error_detail(
                "sort must be either 'direct' or 'subtree'.",
                code="invalid_sort",
                sort=sort,
            ),
        )

    selection, taxonomy, tree = resolve_selection_and_tree(
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
    target = tree.root
    if scope == "node":
        if taxid is None:
            raise ReportServiceError(
                400,
                _error_detail(
                    "taxid is required when scope='node'.",
                    code="missing_taxid",
                    request_context=request_context,
                    scope=scope,
                ),
            )
        target = resolve_node_in_context(
            tree,
            taxid,
            min_reads,
            request_context,
        )

    rows = subtree_table_rows(
        target,
        min_reads=min_reads,
        sort_by=sort,
        limit=limit,
    )
    return {
        "ok": True,
        "scope": scope,
        "target": {
            "taxid": target.taxid,
            "name": target.name,
            "rank": target.rank,
            "direct": target.direct,
            "subtree": target.total,
            "child_count": filtered_child_count(
                target,
                min_reads,
            ),
        },
        "sort": sort,
        "limit": limit,
        "row_count": len(rows),
        "rows": rows,
        "request_context": request_context,
    }


def subtree_report_payload(
    store: GraphEngineStore,
    *,
    taxid: Optional[int],
    taxids: Optional[List[int]],
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    min_reads: int,
) -> Dict[str, Any]:
    selection, taxonomy, tree = resolve_selection_and_tree(
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
    requested_taxids = normalize_taxids(taxids)
    if taxid is not None:
        try:
            single_taxid = int(taxid)
        except (TypeError, ValueError):
            single_taxid = None
        if (
            single_taxid is not None
            and single_taxid not in requested_taxids
        ):
            requested_taxids.append(single_taxid)
    if not requested_taxids:
        raise ReportServiceError(
            400,
            _error_detail(
                "At least one taxid is required for a count matrix report.",
                code="missing_taxids",
                request_context=request_context,
            ),
        )
    selected_nodes = [
        resolve_node_in_context(
            tree,
            selected_taxid,
            min_reads,
            request_context,
        )
        for selected_taxid in requested_taxids
    ]
    report = selected_count_matrix_report(
        selection,
        selected_nodes,
    )

    return {
        "ok": True,
        "report": report,
        "request_context": request_context,
    }


def rank_report_payload(
    store: GraphEngineStore,
    *,
    taxids: Optional[List[int]],
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    min_reads: int,
) -> Dict[str, Any]:
    selection, taxonomy, tree = resolve_selection_and_tree(
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
    requested_taxids = normalize_taxids(taxids)
    if not requested_taxids:
        raise ReportServiceError(
            400,
            _error_detail(
                "At least one taxid is required for a rank report.",
                code="missing_taxids",
                request_context=request_context,
            ),
        )

    selected_nodes = [
        resolve_node_in_context(
            tree,
            taxid,
            min_reads,
            request_context,
        )
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
                    {
                        "dataset": dataset.fileinfo.name,
                        "direct": 0,
                    }
                    for dataset in selection.datasets
                ],
            },
        )
        slot["direct"] += node.direct
        slot["node_count"] += 1
        for index, entry in enumerate(slot["datasets"]):
            entry["direct"] += (
                node.direct_by_source[index]
                if index < len(node.direct_by_source)
                else 0
            )

    rows = sorted(
        by_rank.values(),
        key=lambda row: (
            -int(row["direct"]),
            str(row["rank"]),
        ),
    )

    return {
        "ok": True,
        "report": {
            "summary": {
                "selected_node_count": len(selected_nodes),
                "selected_taxids": requested_taxids,
                "total_direct": sum(
                    node.direct
                    for node in selected_nodes
                ),
                "dataset_names": list(selection.dataset_names),
            },
            "rows": rows,
        },
        "request_context": request_context,
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
