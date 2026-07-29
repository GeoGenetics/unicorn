"""Implement read-only Unicorn tools against explicit backend dependencies."""

from __future__ import annotations

import copy
from collections.abc import Mapping
from difflib import SequenceMatcher
from typing import Any

from unicorn_agent.context import (
    BackendContextBuilder,
    BackendContextStore,
    threshold_visible_nodes,
)
from unicorn_agent.contracts import validate_browser_turn_request
from unicorn_agent.registry import ToolRegistry, ToolRegistryError


GRAPH_CONTEXT = "graph.context"
DATASETS_SELECTED = "datasets.selected"
METADATA_COMPARE_SELECTED = "metadata.compare_selected"
METADATA_SUMMARY = "metadata.summary"
NODES_SELECTED = "nodes.selected"
NODES_FIND_VISIBLE = "nodes.find_visible"
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

_METADATA_COMPARE_ARGUMENTS = {
    "type": "object",
    "additionalProperties": False,
    "required": ["field"],
    "properties": {
        "field": {
            "type": "string",
            "minLength": 1,
            "pattern": r".*\S.*",
        },
        "taxids": {
            "type": "array",
            "items": {
                "type": "integer",
                "minimum": 1,
            },
            "minItems": 1,
            "maxItems": 50,
            "uniqueItems": True,
        },
        "limit": {
            "type": "integer",
            "minimum": 1,
            "maximum": 50,
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

    def metadata_summary(
        self,
        arguments: dict[str, Any],
    ) -> dict[str, Any]:
        del arguments
        summary = self._store.metadata_summary_payload()
        selected_datasets = set(self._scope["datasets"])
        if summary is None:
            return {
                "loaded": False,
                "filename": None,
                "fields": [],
                "field_count": 0,
                "rows_total": 0,
                "matched_rows": 0,
                "unmatched_rows": 0,
                "selected_dataset_count": len(selected_datasets),
                "selected_datasets_with_metadata": 0,
            }

        fields = [
            str(field)
            for field in summary.get("fields", [])
            if isinstance(field, str) and field
        ]
        datasets_with_metadata = {
            str(filename)
            for filename in summary.get("datasets_with_metadata", [])
            if isinstance(filename, str) and filename
        }
        return {
            "loaded": True,
            "filename": summary.get("filename"),
            "fields": fields,
            "field_count": len(fields),
            "rows_total": int(summary.get("rows_total", 0)),
            "matched_rows": int(summary.get("matched_rows", 0)),
            "unmatched_rows": int(summary.get("unmatched_rows", 0)),
            "selected_dataset_count": len(selected_datasets),
            "selected_datasets_with_metadata": len(
                selected_datasets & datasets_with_metadata
            ),
        }

    def compare_selected_by_metadata(
        self,
        arguments: dict[str, Any],
    ) -> dict[str, Any]:
        selection, _, tree = self._get_models()
        summary = self._store.metadata_summary_payload()
        if summary is None:
            raise ToolServiceError(
                code="metadata_not_loaded",
                message="No backend metadata table is loaded.",
                tool_id=METADATA_COMPARE_SELECTED,
            )

        field = arguments["field"].strip()
        fields = {
            str(value)
            for value in summary.get("fields", [])
            if isinstance(value, str) and value
        }
        if field not in fields:
            raise ToolServiceError(
                code="metadata_field_not_found",
                message=f"Metadata field {field!r} is not available.",
                tool_id=METADATA_COMPARE_SELECTED,
            )

        limit = int(arguments.get("limit", 25))
        scope_taxids = list(self._scope["selected_taxids"])
        selected_taxids = list(arguments.get("taxids", scope_taxids))
        outside_selection = sorted(set(selected_taxids) - set(scope_taxids))
        if outside_selection:
            raise ToolServiceError(
                code="metadata_taxid_not_selected",
                message=(
                    "Metadata comparison taxids must belong to the current "
                    "node selection."
                ),
                tool_id=METADATA_COMPARE_SELECTED,
            )
        selected_nodes = [
            self._resolve_visible_node(
                tree,
                taxid,
                tool_id=METADATA_COMPARE_SELECTED,
            )
            for taxid in selected_taxids[:limit]
        ]

        grouped_indices: dict[str, list[int]] = {}
        grouped_datasets: dict[str, list[str]] = {}
        for index, dataset in enumerate(selection.datasets):
            filename = str(dataset.fileinfo.name)
            metadata = self._store.metadata_for_dataset(filename)
            raw_value = metadata.get(field) if metadata is not None else None
            value = str(raw_value).strip() if raw_value is not None else ""
            value = value or "__missing__"
            grouped_indices.setdefault(value, []).append(index)
            grouped_datasets.setdefault(value, []).append(filename)

        count_mode = self._scope["count_mode"]
        groups = []
        node_values: dict[int, list[dict[str, Any]]] = {
            int(node.taxid): []
            for node in selected_nodes
        }
        ordered_values = sorted(
            grouped_indices,
            key=lambda value: (value == "__missing__", value.casefold()),
        )
        for value in ordered_values:
            indices = grouped_indices[value]
            node_counts = []
            for node in selected_nodes:
                source_counts = (
                    node.direct_by_source
                    if count_mode == "direct"
                    else node.total_by_source
                )
                counts = [
                    int(source_counts[index])
                    if index < len(source_counts)
                    else 0
                    for index in indices
                ]
                count_sum = sum(counts)
                group_count = {
                    "value": value,
                    "sum": count_sum,
                    "mean": round(count_sum / len(counts), 3),
                    "minimum": min(counts),
                    "maximum": max(counts),
                }
                node_values[int(node.taxid)].append(group_count)
                node_counts.append(group_count)
            groups.append(
                {
                    "value": value,
                    "dataset_count": len(indices),
                    "datasets": grouped_datasets[value],
                    "sum_across_nodes": sum(
                        item["sum"]
                        for item in node_counts
                    ),
                    "mean_across_nodes": round(
                        sum(item["mean"] for item in node_counts)
                        / len(node_counts),
                        3,
                    ) if node_counts else 0.0,
                }
            )

        node_comparisons = []
        for node in selected_nodes:
            values = node_values[int(node.taxid)]
            nonmissing = [
                value
                for value in values
                if value["value"] != "__missing__"
            ]
            highest = (
                max(nonmissing, key=lambda value: value["mean"])["value"]
                if nonmissing
                else None
            )
            node_comparisons.append(
                {
                    "taxid": int(node.taxid),
                    "name": str(node.name),
                    "group_counts": values,
                    "highest_mean_group": highest,
                }
            )

        pairwise_comparisons = []
        comparable_values = [
            value
            for value in ordered_values
            if value != "__missing__"
        ][:10]
        for left_index, left_value in enumerate(comparable_values):
            for right_value in comparable_values[left_index + 1:]:
                left_higher = 0
                right_higher = 0
                tied = 0
                left_total = 0
                right_total = 0
                for comparison in node_comparisons:
                    by_value = {
                        item["value"]: item
                        for item in comparison["group_counts"]
                    }
                    left_count = by_value[left_value]["mean"]
                    right_count = by_value[right_value]["mean"]
                    left_total += by_value[left_value]["sum"]
                    right_total += by_value[right_value]["sum"]
                    if left_count > right_count:
                        left_higher += 1
                    elif right_count > left_count:
                        right_higher += 1
                    else:
                        tied += 1
                pairwise_comparisons.append(
                    {
                        "left_value": left_value,
                        "right_value": right_value,
                        "left_sum": left_total,
                        "right_sum": right_total,
                        "left_higher_node_count": left_higher,
                        "right_higher_node_count": right_higher,
                        "tied_node_count": tied,
                    }
                )

        return {
            "field": field,
            "count_mode": count_mode,
            "comparison_basis": "descriptive_raw_counts",
            "caveat": (
                "Group summaries compare raw counts without library-size "
                "normalization; they do not estimate causal effects or "
                "statistical significance."
            ),
            "selected_node_count": len(scope_taxids),
            "nodes_returned": len(selected_nodes),
            "truncated": len(selected_nodes) < len(selected_taxids),
            "group_count": len(groups),
            "groups": groups,
            "node_comparisons": node_comparisons,
            "pairwise_comparisons": pairwise_comparisons,
            "pairwise_truncated": len(comparable_values) < len(
                [
                    value
                    for value in ordered_values
                    if value != "__missing__"
                ]
            ),
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
                    tool_id=NODES_SELECTED,
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
            match_score = 0.0
            if query == taxid_text:
                match_type, priority, match_score = "taxid", 0, 1.0
            elif query_folded == name_folded:
                match_type, priority, match_score = "exact_name", 1, 1.0
            elif name_folded.startswith(query_folded):
                match_type, priority, match_score = "name_prefix", 2, 1.0
            elif query_folded in name_folded:
                match_type, priority, match_score = "name_contains", 3, 1.0
            elif len(query_folded) >= 4:
                similarity = SequenceMatcher(
                    None,
                    query_folded,
                    name_folded,
                ).ratio()
                if similarity >= 0.86:
                    match_type = "close_name"
                    priority = 4
                    match_score = similarity
            if match_type is None:
                continue
            match = _node_counts(
                node,
                min_reads=self._scope["min_reads"],
            )
            match["match_type"] = match_type
            match["_priority"] = priority
            match["_score"] = match_score
            matches.append(match)

        matches.sort(
            key=lambda item: (
                item["_priority"],
                -item["_score"],
                str(item["name"]).casefold(),
                int(item["taxid"]),
            )
        )
        matches = matches[:limit]
        for match in matches:
            del match["_priority"]
            score = match.pop("_score")
            if match["match_type"] == "close_name":
                match["match_score"] = round(score, 3)
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
        when_to_use=(
            "Use for current dataset count, visible-tree scope, selection "
            "count, count mode, and minimum-read threshold."
        ),
        output_summary=(
            "Compact session, dataset, tree, filter, active metadata display "
            "field, and report-state summaries."
        ),
        arguments_schema=_NO_ARGUMENTS,
        handler=service.graph_context,
        mutation=False,
    )
    registry.register(
        tool_id=DATASETS_SELECTED,
        description="List active datasets and their backend count summaries.",
        when_to_use=(
            "Use when dataset filenames or per-dataset total summaries are "
            "needed."
        ),
        output_summary=(
            "Dataset count, total reads, direct taxa, and one summary per "
            "selected dataset."
        ),
        arguments_schema=_NO_ARGUMENTS,
        handler=service.list_selected_datasets,
        mutation=False,
    )
    registry.register(
        tool_id=METADATA_COMPARE_SELECTED,
        description=(
            "Compare selected-node counts across values of one metadata field."
        ),
        when_to_use=(
            "Use when the user asks which values a metadata field contains or "
            "how that field relates to the currently selected tree nodes. Pass "
            "grounded taxids to restrict comparison to named selected nodes. "
            "Results are descriptive raw-count comparisons, not causal effects."
        ),
        output_summary=(
            "Metadata-value groups, node-centric counts, pairwise group "
            "comparisons, and an explicit non-causal interpretation caveat."
        ),
        arguments_schema=_METADATA_COMPARE_ARGUMENTS,
        handler=service.compare_selected_by_metadata,
        mutation=False,
    )
    registry.register(
        tool_id=METADATA_SUMMARY,
        description="Return the compact backend metadata-table summary.",
        when_to_use=(
            "Use when the user asks which metadata variables or fields are "
            "available, whether metadata is loaded, or how many rows and "
            "datasets match metadata. Do not use graph.context for field names."
        ),
        output_summary=(
            "Metadata load state, field names, row counts, backend match counts, "
            "and selected-dataset metadata coverage without raw metadata rows."
        ),
        arguments_schema=_NO_ARGUMENTS,
        handler=service.metadata_summary,
        mutation=False,
    )
    registry.register(
        tool_id=NODES_SELECTED,
        description="List nodes selected in the active backend graph scope.",
        when_to_use=(
            "Use when the user refers to the current node selection without "
            "providing taxids. Call it once to obtain grounded taxids; if the "
            "user requests details, pass those taxids to node.details in one "
            "call."
        ),
        output_summary=(
            "Selected taxids with names, ranks, direct and subtree counts, and "
            "visible child counts."
        ),
        arguments_schema=_NO_ARGUMENTS,
        handler=service.list_selected_nodes,
        mutation=False,
    )
    registry.register(
        tool_id=NODES_FIND_VISIBLE,
        description="Find threshold-visible nodes by taxid or name.",
        when_to_use=(
            "Use before node.details when a prompt identifies a node by name "
            "or an unverified taxid. Call it once for a query and reuse the "
            "grounded taxid from its result."
        ),
        output_summary=(
            "Ordered visible-node matches with grounded taxids, counts, and "
            "match types."
        ),
        arguments_schema=_FIND_VISIBLE_ARGUMENTS,
        handler=service.find_visible_nodes,
        mutation=False,
    )
    registry.register(
        tool_id=NODE_DETAILS,
        description="Return backend-authoritative details for one or more taxids.",
        when_to_use=(
            "Use after taxids are known or resolved through nodes.find_visible. "
            "After a successful result, answer the user from that result rather "
            "than restarting lookup."
        ),
        output_summary=(
            "Node identity, lineage, direct and subtree counts, child count, "
            "and per-dataset counts for one or more taxids."
        ),
        arguments_schema=_NODE_DETAILS_ARGUMENTS,
        handler=service.node_details,
        mutation=False,
    )
    registry.register(
        tool_id=TABLE_VIEW,
        description="Return a ranked table for the root or one visible node.",
        when_to_use=(
            "Use for ranked direct or subtree count rows within the root or a "
            "known visible-node scope."
        ),
        output_summary=(
            "Target-node summary and ordered table rows capped by the requested "
            "limit."
        ),
        arguments_schema=_TABLE_VIEW_ARGUMENTS,
        handler=service.table_view,
        mutation=False,
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
