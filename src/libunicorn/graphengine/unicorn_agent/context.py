"""Build compact Agent context from backend-authoritative graph state."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol

from unicorn_agent.contracts import (
    validate_browser_turn_request,
    validate_graph_context,
)


class BackendContextStore(Protocol):
    """Narrow store surface required to rebuild one graph scope."""

    def list_files(self) -> Sequence[Any]: ...

    def build_selection(self, names: list[str]) -> Any: ...

    def get_or_load_taxonomy(
        self,
        nodes_name: str | None = None,
        names_name: str | None = None,
    ) -> Any: ...

    def build_tree_model(self, selection: Any, taxonomy: Any) -> Any: ...


@dataclass(frozen=True)
class ContextErrorDetail:
    code: str
    message: str
    details: Mapping[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "code": self.code,
            "message": self.message,
            "details": dict(self.details),
        }


class ContextBuildError(ValueError):
    """Structured failure while resolving browser scope against the backend."""

    def __init__(
        self,
        *,
        code: str,
        message: str,
        **details: Any,
    ) -> None:
        self.detail = ContextErrorDetail(
            code=code,
            message=message,
            details=details,
        )
        self.code = code
        super().__init__(message)

    def to_dict(self) -> dict[str, Any]:
        return self.detail.to_dict()


class BackendContextBuilder:
    """Resolve browser identifiers and return the frozen compact context."""

    def __init__(self, store: BackendContextStore) -> None:
        self._store = store

    def __call__(self, request: Mapping[str, Any]) -> dict[str, Any]:
        return self.build(request)

    def build(self, request: Mapping[str, Any]) -> dict[str, Any]:
        validate_browser_turn_request(request)
        scope = request["graph_scope"]

        dataset_names = [
            _require_filename(value, role="dataset")
            for value in scope["datasets"]
        ]
        nodes_name = _require_filename(scope["nodes_file"], role="nodes_file")
        names_name = _require_filename(scope["names_file"], role="names_file")

        self._verify_datasets(dataset_names)
        taxonomy = self._load_and_verify_taxonomy(nodes_name, names_name)

        if not dataset_names:
            if scope["selected_taxids"] or scope["focused_taxid"] is not None:
                raise ContextBuildError(
                    code="taxid_without_datasets",
                    message="Selected or focused taxids require active datasets.",
                )
            context = _empty_graph_context(scope)
            validate_graph_context(context)
            return context

        try:
            selection = self._store.build_selection(dataset_names)
            tree = self._store.build_tree_model(selection, taxonomy)
        except ContextBuildError:
            raise
        except Exception as error:
            raise ContextBuildError(
                code="graph_scope_build_failed",
                message="Backend could not build the requested graph scope.",
            ) from error

        visible_nodes = threshold_visible_nodes(
            tree.root,
            min_reads=scope["min_reads"],
        )
        visible_by_taxid = {
            int(node.taxid): node
            for node in visible_nodes
            if int(node.taxid) >= 1
        }

        selected_taxids = list(scope["selected_taxids"])
        self._verify_taxids(
            selected_taxids,
            visible_by_taxid,
            role="selected",
        )

        focused_taxid = scope["focused_taxid"]
        if focused_taxid is not None:
            self._verify_taxids(
                [focused_taxid],
                visible_by_taxid,
                role="focused",
            )

        context = {
            "session": {
                "backend_connected": True,
            },
            "datasets": {
                "selected_count": len(selection.datasets),
            },
            "tree": {
                "visible_node_count": len(visible_nodes),
                "selected_node_count": len(selected_taxids),
                "visible_root": _node_summary(tree.root),
                "focused_node": (
                    _node_summary(visible_by_taxid[focused_taxid])
                    if focused_taxid is not None
                    else None
                ),
            },
            "filters": {
                "min_reads": scope["min_reads"],
                "count_mode": scope["count_mode"],
            },
            "metadata": {
                "active_field": None,
            },
            "report_state": {
                "active": False,
                "mode": None,
            },
        }
        validate_graph_context(context)
        return context

    def _verify_datasets(self, dataset_names: list[str]) -> None:
        available = {
            _backend_filename(item)
            for item in self._store.list_files()
        }
        missing = sorted(name for name in dataset_names if name not in available)
        if missing:
            raise ContextBuildError(
                code="dataset_not_found",
                message="Requested datasets are not available on the backend.",
                missing=missing,
            )

    def _load_and_verify_taxonomy(
        self,
        nodes_name: str,
        names_name: str,
    ) -> Any:
        try:
            taxonomy = self._store.get_or_load_taxonomy(
                nodes_name=nodes_name,
                names_name=names_name,
            )
        except Exception as error:
            raise ContextBuildError(
                code="taxonomy_not_available",
                message="Requested taxonomy files are not available on the backend.",
                nodes_file=nodes_name,
                names_file=names_name,
            ) from error

        actual_nodes = _fileinfo_name(
            getattr(taxonomy, "nodes_fileinfo", None),
        )
        actual_names = _fileinfo_name(
            getattr(taxonomy, "names_fileinfo", None),
        )
        if actual_nodes != nodes_name or actual_names != names_name:
            raise ContextBuildError(
                code="taxonomy_mismatch",
                message="Backend taxonomy does not match the requested filenames.",
                requested_nodes_file=nodes_name,
                requested_names_file=names_name,
                resolved_nodes_file=actual_nodes,
                resolved_names_file=actual_names,
            )
        return taxonomy

    @staticmethod
    def _verify_taxids(
        taxids: Sequence[int],
        visible_by_taxid: Mapping[int, Any],
        *,
        role: str,
    ) -> None:
        missing = sorted(taxid for taxid in taxids if taxid not in visible_by_taxid)
        if missing:
            raise ContextBuildError(
                code=f"{role}_taxid_not_visible",
                message=f"Requested {role} taxids are not visible in the backend scope.",
                taxids=missing,
            )


def build_backend_context(
    request: Mapping[str, Any],
    *,
    store: BackendContextStore,
) -> dict[str, Any]:
    return BackendContextBuilder(store).build(request)


def _require_filename(value: str, *, role: str) -> str:
    if Path(value).name != value:
        raise ContextBuildError(
            code="invalid_backend_filename",
            message=f"{role} must be a backend filename, not a path.",
            role=role,
        )
    return value


def _backend_filename(item: Any) -> str:
    name = getattr(item, "name", None)
    if not isinstance(name, str) or not name:
        raise ContextBuildError(
            code="invalid_backend_store",
            message="Backend returned a dataset entry without a filename.",
        )
    return name


def _fileinfo_name(fileinfo: Any) -> str | None:
    name = getattr(fileinfo, "name", None)
    return name if isinstance(name, str) and name else None


def threshold_visible_nodes(root: Any, *, min_reads: int) -> list[Any]:
    visible: list[Any] = []
    stack = [root]
    while stack:
        node = stack.pop()
        if node is not root and int(node.total) < min_reads:
            continue
        visible.append(node)
        stack.extend(reversed(list(node.children)))
    return visible


def _node_summary(node: Any) -> dict[str, Any] | None:
    taxid = int(node.taxid)
    if taxid < 1:
        return None
    return {
        "taxid": taxid,
        "name": str(node.name) or str(taxid),
        "rank": str(node.rank) or "no rank",
    }


def _empty_graph_context(scope: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "session": {
            "backend_connected": True,
        },
        "datasets": {
            "selected_count": 0,
        },
        "tree": {
            "visible_node_count": 0,
            "selected_node_count": 0,
            "visible_root": None,
            "focused_node": None,
        },
        "filters": {
            "min_reads": scope["min_reads"],
            "count_mode": scope["count_mode"],
        },
        "metadata": {
            "active_field": None,
        },
        "report_state": {
            "active": False,
            "mode": None,
        },
    }
