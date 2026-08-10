"""Tree-oriented domain services for Unicorn Graph Engine."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from unicorn_backend.models import (
    SelectionModel,
    TaxonomyModel,
    TreeModel,
    TreeNodeModel,
)
from unicorn_backend.store import GraphEngineStore


class TreeServiceError(Exception):
    """A tree-domain failure awaiting HTTP mapping at the route boundary."""

    def __init__(self, status_code: int, detail: Any) -> None:
        super().__init__(
            detail.get("message")
            if isinstance(detail, dict)
            else str(detail)
        )
        self.status_code = status_code
        self.detail = detail


def normalize_taxids(values: Optional[List[int]]) -> List[int]:
    normalized: List[int] = []
    seen: set[int] = set()
    for value in values or []:
        try:
            taxid = int(value)
        except (TypeError, ValueError):
            continue
        if taxid in seen:
            continue
        seen.add(taxid)
        normalized.append(taxid)
    return normalized


def resolve_selection_and_tree(
    store: GraphEngineStore,
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
) -> Tuple[SelectionModel, TaxonomyModel, TreeModel]:
    available = store.list_files()
    requested = _normalize_requested_files(files)
    selected_names = (
        requested
        if requested
        else sorted(path.name for path in available)
    )
    if not selected_names:
        raise TreeServiceError(
            400,
            {"message": "No datasets selected for tree rendering."},
        )
    selection = store.build_selection(selected_names)
    taxonomy = store.get_or_load_taxonomy(
        nodes_name=nodes_file,
        names_name=names_file,
    )
    tree = store.build_tree_model(selection, taxonomy)
    return selection, taxonomy, tree


def response_context(
    selection: SelectionModel,
    taxonomy: TaxonomyModel,
    min_reads: int,
    expanded_taxids: List[int],
) -> Dict[str, Any]:
    return {
        "dataset_names": list(selection.dataset_names),
        "nodes_file": taxonomy.nodes_fileinfo.name,
        "names_file": (
            taxonomy.names_fileinfo.name
            if taxonomy.names_fileinfo
            else None
        ),
        "min_reads": min_reads,
        "expanded_taxids": expanded_taxids,
    }


def node_passes_filter(
    node: TreeNodeModel,
    tree: TreeModel,
    min_reads: int,
) -> bool:
    return node is tree.root or node.total >= max(0, min_reads)


def filtered_child_count(
    node: TreeNodeModel,
    min_reads: int,
) -> int:
    threshold = max(0, min_reads)
    return sum(
        1
        for child in node.children
        if child.total >= threshold
    )


def build_lineage(
    node: TreeNodeModel,
    tree: TreeModel,
) -> List[Dict[str, Any]]:
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


def dataset_breakdown(
    selection: SelectionModel,
    node: TreeNodeModel,
) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for index, dataset in enumerate(selection.datasets):
        rows.append(
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
        )
    return rows


def require_node_present(
    tree: TreeModel,
    taxid: int,
    request_context: Dict[str, Any],
) -> TreeNodeModel:
    node = tree.root.find_taxid(taxid)
    if node is None:
        raise TreeServiceError(
            404,
            _error_detail(
                "Requested node is not present in the active tree.",
                code="node_not_in_active_tree",
                request_context=request_context,
                taxid=taxid,
            ),
        )
    return node


def resolve_node_in_context(
    tree: TreeModel,
    taxid: int,
    min_reads: int,
    request_context: Dict[str, Any],
) -> TreeNodeModel:
    node = require_node_present(tree, taxid, request_context)
    if not node_passes_filter(node, tree, min_reads):
        raise TreeServiceError(
            409,
            _error_detail(
                "Requested node exists in the active tree but is filtered "
                "out by the current min_reads threshold.",
                code="node_filtered_out",
                request_context=request_context,
                taxid=taxid,
                node_subtree_reads=node.total,
                min_reads=min_reads,
            ),
        )
    return node


def collect_expandable_taxids(
    node: TreeNodeModel,
    tree: TreeModel,
    min_reads: int,
    expanded_taxids: set[int],
) -> None:
    eligible_children = [
        child
        for child in node.children
        if node_passes_filter(child, tree, min_reads)
    ]
    if eligible_children:
        expanded_taxids.add(node.taxid)
    for child in eligible_children:
        collect_expandable_taxids(
            child,
            tree,
            min_reads,
            expanded_taxids,
        )


def build_visible_tree_payload(
    tree: TreeModel,
    expanded_taxids: set[int],
    min_reads: int,
) -> Tuple[Dict[str, Any], List[int]]:
    threshold = max(0, min_reads)
    active_expanded_taxids: set[int] = set()

    def build_node_payload(
        node: TreeNodeModel,
        force_expanded: bool = False,
    ) -> Optional[Dict[str, Any]]:
        if node is not tree.root and node.total < threshold:
            return None
        eligible_children = [
            child
            for child in node.children
            if child.total >= threshold
        ]
        expanded = force_expanded or node.taxid in expanded_taxids
        if (
            not force_expanded
            and expanded
            and eligible_children
        ):
            active_expanded_taxids.add(node.taxid)
        visible_children = []
        if expanded:
            for child in eligible_children:
                payload = build_node_payload(
                    child,
                    force_expanded=False,
                )
                if payload is not None:
                    visible_children.append(payload)
        return {
            "taxid": node.taxid,
            "parent": node.parent,
            "rank": node.rank,
            "name": node.name,
            "direct": node.direct,
            "direct_by_source": node.direct_by_source,
            "total": node.total,
            "total_by_source": node.total_by_source,
            "depth": node.depth,
            "child_count": len(eligible_children),
            "has_children": bool(eligible_children),
            "expanded": expanded,
            "children": visible_children,
        }

    payload = build_node_payload(
        tree.root,
        force_expanded=True,
    )
    if payload is None:
        raise TreeServiceError(
            500,
            "Could not build visible tree payload.",
        )
    return payload, sorted(active_expanded_taxids)


def visible_tree_response(
    store: GraphEngineStore,
    selection: SelectionModel,
    taxonomy: TaxonomyModel,
    tree: TreeModel,
    min_reads: int,
    expanded_taxids: set[int],
) -> Dict[str, Any]:
    visible_tree, active_expanded_taxids = build_visible_tree_payload(
        tree,
        expanded_taxids=expanded_taxids,
        min_reads=min_reads,
    )
    return {
        "ok": True,
        "datasets": [
            store.dataset_summary_payload(dataset)
            for dataset in selection.datasets
        ],
        "taxonomy": taxonomy.to_status_payload(),
        "tree": visible_tree,
        "missing_taxids": tree.missing_taxids,
        "expanded_taxids": active_expanded_taxids,
        "requested_expanded_taxids": sorted(expanded_taxids),
        "min_reads": min_reads,
        "total_reads": selection.total_reads,
        "direct_taxa": len(selection.direct_counts),
        "request_context": response_context(
            selection,
            taxonomy,
            min_reads,
            active_expanded_taxids,
        ),
        "metadata": store.metadata_summary_payload(),
        "cache": store.cache_status(),
    }


def _normalize_requested_files(
    files: Optional[List[str]],
) -> List[str]:
    normalized = []
    for value in files or []:
        for item in value.split(","):
            item = item.strip()
            if item:
                normalized.append(Path(item).name)
    return normalized


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
