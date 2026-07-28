from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from .helpers import require_agent_module, valid_browser_turn_request


@dataclass
class FixtureNode:
    taxid: int
    parent: int | None
    name: str
    rank: str
    direct: int
    total: int
    direct_by_source: list[int]
    total_by_source: list[int]
    depth: int
    children: list["FixtureNode"] = field(default_factory=list)


class FixtureStore:
    def __init__(self) -> None:
        triticum = FixtureNode(
            4565, 33090, "Triticum", "genus", 3000, 7000,
            [1000, 2000], [2500, 4500], 2,
        )
        viridiplantae = FixtureNode(
            33090, 1, "Viridiplantae", "kingdom", 100, 8000,
            [40, 60], [3000, 5000], 1, [triticum],
        )
        opisthokonta = FixtureNode(
            33154, 1, "Opisthokonta", "clade", 50, 2000,
            [20, 30], [800, 1200], 1,
        )
        hidden = FixtureNode(
            2, 1, "Hidden node", "species", 500, 500,
            [200, 300], [200, 300], 1,
        )
        root = FixtureNode(
            1, None, "root", "no rank", 0, 10500,
            [0, 0], [4000, 6500], 0,
            [viridiplantae, opisthokonta, hidden],
        )
        self.tree = SimpleNamespace(root=root)
        self.datasets = [
            SimpleNamespace(
                fileinfo=SimpleNamespace(name="sample_a.bdamage.txt"),
                total_reads=4000,
                total_taxa=3,
            ),
            SimpleNamespace(
                fileinfo=SimpleNamespace(name="sample_b.bdamage.txt"),
                total_reads=6500,
                total_taxa=4,
            ),
        ]
        self.selection = SimpleNamespace(
            datasets=self.datasets,
            total_reads=10500,
            direct_counts={33090: 100, 4565: 3000, 33154: 50, 2: 500},
        )
        self.taxonomy = SimpleNamespace(
            nodes_fileinfo=SimpleNamespace(name="nodes.dmp"),
            names_fileinfo=SimpleNamespace(name="names.dmp"),
        )

    def list_files(self) -> list[Path]:
        return [Path(dataset.fileinfo.name) for dataset in self.datasets]

    def build_selection(self, names: list[str]) -> Any:
        assert names == [
            "sample_a.bdamage.txt",
            "sample_b.bdamage.txt",
        ]
        return self.selection

    def get_or_load_taxonomy(
        self,
        nodes_name: str | None = None,
        names_name: str | None = None,
    ) -> Any:
        assert nodes_name == "nodes.dmp"
        assert names_name == "names.dmp"
        return self.taxonomy

    def build_tree_model(self, selection: Any, taxonomy: Any) -> Any:
        assert selection is self.selection
        assert taxonomy is self.taxonomy
        return self.tree


def tool_request() -> dict[str, Any]:
    request = valid_browser_turn_request()
    request["graph_scope"]["datasets"] = [
        "sample_a.bdamage.txt",
        "sample_b.bdamage.txt",
    ]
    request["graph_scope"]["selected_taxids"] = [33090]
    request["graph_scope"]["focused_taxid"] = None
    return request


def build_registry() -> Any:
    tools = require_agent_module("unicorn_agent.tools")
    return tools.create_read_only_registry(
        store=FixtureStore(),
        request=tool_request(),
    )


def test_read_only_registry_advertises_expected_tools() -> None:
    registry = build_registry()

    assert [tool["tool_id"] for tool in registry.provider_tools()] == [
        "datasets.selected",
        "graph.context",
        "node.details",
        "nodes.find_visible",
        "nodes.selected",
        "table.view",
    ]
    assert all(tool["when_to_use"] for tool in registry.provider_tools())
    assert all(tool["output_summary"] for tool in registry.provider_tools())
    assert all(tool["mutation"] is False for tool in registry.provider_tools())


def test_graph_context_and_dataset_tools_use_backend_state() -> None:
    registry = build_registry()

    context = registry.dispatch("graph.context", {})
    datasets = registry.dispatch("datasets.selected", {})

    assert context["datasets"]["selected_count"] == 2
    assert context["tree"]["visible_node_count"] == 4
    assert datasets == {
        "count": 2,
        "total_reads": 10500,
        "direct_taxa": 4,
        "datasets": [
            {
                "filename": "sample_a.bdamage.txt",
                "total_reads": 4000,
                "total_taxa": 3,
            },
            {
                "filename": "sample_b.bdamage.txt",
                "total_reads": 6500,
                "total_taxa": 4,
            },
        ],
    }


def test_selected_and_visible_lookup_tools_are_grounded() -> None:
    registry = build_registry()

    selected = registry.dispatch("nodes.selected", {})
    matches = registry.dispatch(
        "nodes.find_visible",
        {
            "query": "Virid",
            "limit": 5,
        },
    )

    assert selected["count"] == 1
    assert selected["nodes"][0]["taxid"] == 33090
    assert matches["count"] == 1
    assert matches["matches"][0]["taxid"] == 33090
    assert matches["matches"][0]["match_type"] == "name_prefix"


def test_node_details_supports_multiple_taxids() -> None:
    registry = build_registry()

    result = registry.dispatch(
        "node.details",
        {
            "taxid": None,
            "taxids": [33090, 4565],
        },
    )

    assert result["count"] == 2
    assert result["node"] is None
    assert [node["taxid"] for node in result["nodes"]] == [33090, 4565]
    assert result["nodes"][1]["lineage"][-1]["name"] == "Triticum"
    assert result["nodes"][1]["datasets"][1]["subtree"] == 4500


def test_table_view_returns_ranked_backend_rows() -> None:
    registry = build_registry()

    result = registry.dispatch(
        "table.view",
        {
            "scope": "node",
            "taxid": 33090,
            "sort": "subtree",
            "limit": 10,
        },
    )

    assert result["target"]["taxid"] == 33090
    assert result["row_count"] == 2
    assert [row["taxid"] for row in result["rows"]] == [33090, 4565]
