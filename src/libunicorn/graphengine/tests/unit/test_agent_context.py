from __future__ import annotations

import copy
from dataclasses import dataclass, field
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

from .helpers import require_agent_module, valid_browser_turn_request


@dataclass
class FixtureNode:
    taxid: int
    name: str
    rank: str
    total: int
    children: list["FixtureNode"] = field(default_factory=list)


class FixtureStore:
    def __init__(self) -> None:
        hidden = FixtureNode(33154, "Opisthokonta", "clade", 999)
        selected = FixtureNode(33090, "Viridiplantae", "kingdom", 5000)
        self.tree = SimpleNamespace(
            root=FixtureNode(
                1,
                "root",
                "no rank",
                6000,
                [selected, hidden],
            )
        )
        self.dataset_names = ["sample_a.bdamage.txt"]
        self.selection = SimpleNamespace(
            datasets=[SimpleNamespace(name="sample_a.bdamage.txt")]
        )
        self.taxonomy = SimpleNamespace(
            nodes_fileinfo=SimpleNamespace(name="nodes.dmp"),
            names_fileinfo=SimpleNamespace(name="names.dmp"),
        )
        self.selection_requests: list[list[str]] = []
        self.taxonomy_requests: list[tuple[str | None, str | None]] = []

    def list_files(self) -> list[Path]:
        return [Path(name) for name in self.dataset_names]

    def build_selection(self, names: list[str]) -> Any:
        self.selection_requests.append(list(names))
        return self.selection

    def get_or_load_taxonomy(
        self,
        nodes_name: str | None = None,
        names_name: str | None = None,
    ) -> Any:
        self.taxonomy_requests.append((nodes_name, names_name))
        return self.taxonomy

    def build_tree_model(self, selection: Any, taxonomy: Any) -> Any:
        assert selection is self.selection
        assert taxonomy is self.taxonomy
        return self.tree


def scoped_request() -> dict[str, Any]:
    request = valid_browser_turn_request()
    request["graph_scope"]["selected_taxids"] = [33090]
    request["graph_scope"]["focused_taxid"] = 33090
    return request


def test_context_is_rebuilt_from_backend_authoritative_state() -> None:
    context_module = require_agent_module("unicorn_agent.context")
    store = FixtureStore()

    context = context_module.build_backend_context(
        scoped_request(),
        store=store,
    )

    assert store.selection_requests == [["sample_a.bdamage.txt"]]
    assert store.taxonomy_requests == [("nodes.dmp", "names.dmp")]
    assert context == {
        "session": {
            "backend_connected": True,
        },
        "datasets": {
            "selected_count": 1,
        },
        "tree": {
            "visible_node_count": 2,
            "selected_node_count": 1,
            "visible_root": {
                "taxid": 1,
                "name": "root",
                "rank": "no rank",
            },
            "focused_node": {
                "taxid": 33090,
                "name": "Viridiplantae",
                "rank": "kingdom",
            },
        },
        "filters": {
            "min_reads": 1000,
            "count_mode": "subtree",
        },
        "metadata": {
            "active_field": None,
        },
        "report_state": {
            "active": False,
            "mode": None,
        },
    }


def test_context_rejects_dataset_missing_from_backend() -> None:
    context_module = require_agent_module("unicorn_agent.context")
    store = FixtureStore()
    request = scoped_request()
    request["graph_scope"]["datasets"] = ["missing.bdamage.txt"]

    with pytest.raises(context_module.ContextBuildError) as caught:
        context_module.build_backend_context(request, store=store)

    assert caught.value.code == "dataset_not_found"
    assert caught.value.to_dict()["details"]["missing"] == [
        "missing.bdamage.txt"
    ]


def test_context_rejects_taxonomy_filename_mismatch() -> None:
    context_module = require_agent_module("unicorn_agent.context")
    store = FixtureStore()
    store.taxonomy.names_fileinfo = SimpleNamespace(name="other_names.dmp")

    with pytest.raises(context_module.ContextBuildError) as caught:
        context_module.build_backend_context(scoped_request(), store=store)

    assert caught.value.code == "taxonomy_mismatch"


@pytest.mark.parametrize(
    ("field", "value", "expected_code"),
    [
        ("selected_taxids", [33154], "selected_taxid_not_visible"),
        ("focused_taxid", 33154, "focused_taxid_not_visible"),
    ],
)
def test_context_rejects_taxids_outside_threshold_visible_scope(
    field: str,
    value: Any,
    expected_code: str,
) -> None:
    context_module = require_agent_module("unicorn_agent.context")
    request = scoped_request()
    request["graph_scope"][field] = value

    with pytest.raises(context_module.ContextBuildError) as caught:
        context_module.build_backend_context(request, store=FixtureStore())

    assert caught.value.code == expected_code


def test_context_rejects_path_like_backend_identifiers() -> None:
    context_module = require_agent_module("unicorn_agent.context")
    request = copy.deepcopy(scoped_request())
    request["graph_scope"]["nodes_file"] = "../nodes.dmp"

    with pytest.raises(context_module.ContextBuildError) as caught:
        context_module.build_backend_context(request, store=FixtureStore())

    assert caught.value.code == "invalid_backend_filename"
