"""Implement read-only Unicorn tools against explicit backend dependencies."""

from __future__ import annotations

import copy
from collections.abc import Mapping
from typing import Any

from unicorn_agent.context import (
    BackendContextBuilder,
    BackendContextStore,
    threshold_visible_nodes,
)
from unicorn_agent.contracts import validate_browser_turn_request
from unicorn_agent.registry import ToolRegistry, ToolRegistryError


GRAPH_CONTEXT = "graph.context"
DATASET_LIST_SELECTED = "dataset.list_selected"
NODE_LIST_SELECTED = "node.list_selected"
NODE_FIND_VISIBLE = "node.find_visible"
NODE_DETAILS = "node.details"
TABLE_VIEW = "table.view"


_NO_ARGUMENTS = {
    "type": "object",
    "additionalProperties": False,
    "maxProperties": 0,
}

_FIND_VISIBLE_ARGUMENTS = {
    "type": "object",
    "additionalProperties": False,
    "required": ["query"],
    "properties": {
        "query": {
            "type": "string",
            "minLength": 1,
            "pattern": r".*\S.*",
        },
        "limit": {
            "type": "integer",
            "minimum": 1,
            "maximum": 100,
        },
    },
}

_NODE_DETAILS_ARGUMENTS = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "taxid": {
            "type": ["integer", "null"],
            "minimum": 1,
        },
        "taxids": {
            "type": "array",
            "items": {
                "type": "integer",
                "minimum": 1,
            },
            "minItems": 1,
            "maxItems": 100,
            "uniqueItems": True,
        },
    },
    "anyOf": [
        {
            "required": ["taxid"],
            "properties": {
                "taxid": {
                    "type": "integer",
                    "minimum": 1,
                }
            },
        },
        {
            "required": ["taxids"],
        },
    ],
}

_TABLE_VIEW_ARGUMENTS = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "scope": {
            "type": "string",
            "enum": ["root", "node"],
        },
        "taxid": {
            "type": "integer",
            "minimum": 1,
        },
        "sort": {
            "type": "string",
            "enum": ["direct", "subtree"],
        },
        "limit": {
            "type": "integer",
            "minimum": 1,
            "maximum": 1000,
        },
    },
    "allOf": [
        {
            "if": {
                "required": ["scope"],
                "properties": {
                    "scope": {
                        "const": "node",
                    }
                },
            },
            "then": {
                "required": ["taxid"],
            },
        }
    ],
}


class ToolServiceError(ToolRegistryError):
    """Read-only tool failure preserved by registry dispatch."""


class ReadOnlyToolService:
    """One turn's read-only tools over one verified backend graph scope."""

    def __init__(
        self,
        *,
        store: BackendContextStore,
        request: Mapping[str, Any],
    ) -> None:
        validate_browser_turn_request(request)
        self._store = store
        self._request = copy.deepcopy(dict(request))
        self._context_builder = BackendContextBuilder(store)
        self._context: dict[str, Any] | None = None
        self._selection: Any = None
        self._taxonomy: Any = None
        self._tree: Any = None

    def graph_context(self, arguments: dict[str, Any]) -> dict[str, Any]:
        del arguments
        return copy.deepcopy(self._get_context())

    def list_selected_datasets(
        self,
        arguments: dict[str, Any],
    ) -> dict[str, Any]:
        del arguments
        selection, _, _ = self._get_models()
        datasets = [
            {
                "filename": dataset.fileinfo.name,
                "total_reads": int(dataset.total_reads),
                "total_taxa": int(dataset.total_taxa),
            }
            for dataset in selection.datasets
        ]
        return {
            "count": len(datasets),
            "total_reads": int(selection.total_reads),
            "direct_taxa": len(selection.direct_counts),
            "datasets": datasets,
        }

    def list_selected_nodes(
        self,
        arguments: dict[str, Any],
    ) -> dict[str, Any]:
        del arguments
        _, _, tree = self._get_models()
        selected = [
            _node_counts(
                self._resolve_visible_node(
                    tree,
                    taxid,
                    tool_id=NODE_LIST_SELECTED,
                ),
                min_reads=self._scope["min_reads"],
            )
            for taxid in self._scope["selected_taxids"]
        ]
        return {
            "count": len(selected),
            "nodes": selected,
        }

    def find_visible_nodes(
        self,
        arguments: dict[str, Any],
    ) -> dict[str, Any]:
        _, _, tree = self._get_models()
        query = arguments["query"].strip()
        query_folded = query.casefold()
        limit = int(arguments.get("limit", 20))
        matches: list[dict[str, Any]] = []

        for node in self._visible_nodes(tree):
            taxid_text = str(node.taxid)
            name_folded = str(node.name).casefold()
            match_type = None
            priority = 99
            if query == taxid_text:
                match_type, priority = "taxid", 0
            elif query_folded == name_folded:
                match_type, priority = "exact_name", 1
            elif name_folded.startswith(query_folded):
                match_type, priority = "name_prefix", 2
            elif query_folded in name_folded:
                match_type, priority = "name_contains", 3
            if match_type is None:
                continue
            match = _node_counts(
                node,
                min_reads=self._scope["min_reads"],
            )
            match["match_type"] = match_type
            match["_priority"] = priority
            matches.append(match)

        matches.sort(
            key=lambda item: (
                item["_priority"],
                str(item["name"]).casefold(),
                int(item["taxid"]),
            )
        )
        matches = matches[:limit]
        for match in matches:
            del match["_priority"]
        return {
            "query": query,
            "count": len(matches),
            "matches": matches,
        }

    def node_details(self, arguments: dict[str, Any]) -> dict[str, Any]:
        selection, _, tree = self._get_models()
        requested: list[int] = []
        taxid = arguments.get("taxid")
        if isinstance(taxid, int):
            requested.append(taxid)
        for item in arguments.get("taxids", []):
            if item not in requested:
                requested.append(item)

        nodes = [
            _node_details(
                selection,
                tree,
                self._resolve_visible_node(
                    tree,
                    item,
                    tool_id=NODE_DETAILS,
                ),
                min_reads=self._scope["min_reads"],
            )
            for item in requested
        ]
        return {
            "count": len(nodes),
            "node": nodes[0] if len(nodes) == 1 else None,
            "nodes": nodes,
        }

    def table_view(self, arguments: dict[str, Any]) -> dict[str, Any]:
        _, _, tree = self._get_models()
        scope = arguments.get("scope", "root")
        sort_by = arguments.get("sort", self._scope["count_mode"])
        limit = int(arguments.get("limit", 40))
        target = tree.root
        if scope == "node":
            target = self._resolve_visible_node(
                tree,
                arguments["taxid"],
                tool_id=TABLE_VIEW,
            )

        rows = _table_rows(
            target,
            min_reads=self._scope["min_reads"],
            sort_by=sort_by,
            limit=limit,
        )
        return {
            "scope": scope,
            "target": _node_counts(
                target,
                min_reads=self._scope["min_reads"],
            ),
            "sort": sort_by,
            "limit": limit,
            "row_count": len(rows),
            "rows": rows,
        }

    @property
    def _scope(self) -> Mapping[str, Any]:
        return self._request["graph_scope"]

    def _get_context(self) -> dict[str, Any]:
        if self._context is None:
            self._context = self._context_builder.build(self._request)
        return self._context

    def _get_models(self) -> tuple[Any, Any, Any]:
        self._get_context()
        if self._tree is None:
            scope = self._scope
            self._selection = self._store.build_selection(list(scope["datasets"]))
            self._taxonomy = self._store.get_or_load_taxonomy(
                nodes_name=scope["nodes_file"],
                names_name=scope["names_file"],
            )
            self._tree = self._store.build_tree_model(
                self._selection,
                self._taxonomy,
            )
        return self._selection, self._taxonomy, self._tree

    def _visible_nodes(self, tree: Any) -> list[Any]:
        return threshold_visible_nodes(
            tree.root,
            min_reads=self._scope["min_reads"],
        )

    def _resolve_visible_node(
        self,
        tree: Any,
        taxid: int,
        *,
        tool_id: str,
    ) -> Any:
        for node in self._visible_nodes(tree):
            if int(node.taxid) == taxid:
                return node
        raise ToolServiceError(
            code="node_not_visible",
            message=f"Taxid {taxid} is not visible in the active backend scope.",
            tool_id=tool_id,
        )


def create_read_only_registry(
    *,
    store: BackendContextStore,
    request: Mapping[str, Any],
) -> ToolRegistry:
    service = ReadOnlyToolService(store=store, request=request)
    registry = ToolRegistry()
    registry.register(
        tool_id=GRAPH_CONTEXT,
        description="Return the compact backend-authoritative graph context.",
        arguments_schema=_NO_ARGUMENTS,
        handler=service.graph_context,
    )
    registry.register(
        tool_id=DATASET_LIST_SELECTED,
        description="List active datasets and their backend count summaries.",
        arguments_schema=_NO_ARGUMENTS,
        handler=service.list_selected_datasets,
    )
    registry.register(
        tool_id=NODE_LIST_SELECTED,
        description="List nodes selected in the active backend graph scope.",
        arguments_schema=_NO_ARGUMENTS,
        handler=service.list_selected_nodes,
    )
    registry.register(
        tool_id=NODE_FIND_VISIBLE,
        description="Find threshold-visible nodes by taxid or name.",
        arguments_schema=_FIND_VISIBLE_ARGUMENTS,
        handler=service.find_visible_nodes,
    )
    registry.register(
        tool_id=NODE_DETAILS,
        description="Return backend-authoritative details for one or more taxids.",
        arguments_schema=_NODE_DETAILS_ARGUMENTS,
        handler=service.node_details,
    )
    registry.register(
        tool_id=TABLE_VIEW,
        description="Return a ranked table for the root or one visible node.",
        arguments_schema=_TABLE_VIEW_ARGUMENTS,
        handler=service.table_view,
    )
    return registry


def _node_counts(node: Any, *, min_reads: int) -> dict[str, Any]:
    return {
        "taxid": int(node.taxid),
        "name": str(node.name),
        "rank": str(node.rank),
        "direct": int(node.direct),
        "subtree": int(node.total),
        "child_count": sum(
            1
            for child in node.children
            if int(child.total) >= min_reads
        ),
    }


def _node_details(
    selection: Any,
    tree: Any,
    node: Any,
    *,
    min_reads: int,
) -> dict[str, Any]:
    details = _node_counts(node, min_reads=min_reads)
    details.update(
        {
            "parent": node.parent,
            "depth": int(node.depth),
            "lineage": _lineage(tree, node),
            "datasets": [
                {
                    "dataset": dataset.fileinfo.name,
                    "direct": (
                        int(node.direct_by_source[index])
                        if index < len(node.direct_by_source)
                        else 0
                    ),
                    "subtree": (
                        int(node.total_by_source[index])
                        if index < len(node.total_by_source)
                        else 0
                    ),
                }
                for index, dataset in enumerate(selection.datasets)
            ],
        }
    )
    return details


def _lineage(tree: Any, node: Any) -> list[dict[str, Any]]:
    lineage: list[dict[str, Any]] = []
    current = node
    while current is not None:
        lineage.append(
            {
                "taxid": int(current.taxid),
                "name": str(current.name),
                "rank": str(current.rank),
            }
        )
        if current is tree.root or current.parent is None:
            break
        current = _find_node(tree.root, int(current.parent))
    lineage.reverse()
    return lineage


def _find_node(root: Any, taxid: int) -> Any | None:
    stack = [root]
    while stack:
        node = stack.pop()
        if int(node.taxid) == taxid:
            return node
        stack.extend(reversed(list(node.children)))
    return None


def _filtered_child_count(node: Any, min_reads: int) -> int:
    return sum(
        1
        for child in node.children
        if int(child.total) >= min_reads
    )


def _table_rows(
    node: Any,
    *,
    min_reads: int,
    sort_by: str,
    limit: int,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    stack = [node]
    while stack:
        current = stack.pop()
        if current is not node and int(current.total) < min_reads:
            continue
        if int(current.direct) > 0:
            rows.append(
                {
                    "taxid": int(current.taxid),
                    "name": str(current.name),
                    "rank": str(current.rank),
                    "depth": int(current.depth),
                    "direct": int(current.direct),
                    "subtree": int(current.total),
                    "child_count": _filtered_child_count(current, min_reads),
                }
            )
        stack.extend(reversed(list(current.children)))

    rows.sort(
        key=lambda row: (
            -int(row[sort_by]),
            -int(row["direct"]),
            str(row["name"]),
        )
    )
    return rows[:limit]
