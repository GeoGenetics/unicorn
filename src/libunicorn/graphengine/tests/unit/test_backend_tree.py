from __future__ import annotations

from typing import Any

import pytest

from unicorn_backend.models import (
    DatasetModel,
    FileInfo,
    SelectionModel,
    TaxonomyModel,
    TreeModel,
    TreeNodeModel,
)
from unicorn_backend.tree import (
    TreeServiceError,
    build_lineage,
    build_visible_tree_payload,
    collect_expandable_taxids,
    dataset_breakdown,
    filtered_child_count,
    node_passes_filter,
    normalize_taxids,
    require_node_present,
    resolve_node_in_context,
    resolve_selection_and_tree,
    response_context,
    visible_tree_response,
)


def _dataset(name: str, counts: dict[int, int]) -> DatasetModel:
    return DatasetModel(
        fileinfo=FileInfo(name=name, size=10, modified_at=1.0),
        counts_map=counts,
        names_map={},
        counts_payload=[],
        total_reads=sum(counts.values()),
        total_taxa=len(counts),
    )


@pytest.fixture()
def tree_context() -> tuple[SelectionModel, TaxonomyModel, TreeModel]:
    datasets = [
        _dataset("sample_a.bdamage.txt", {10: 30, 11: 30, 12: 5}),
        _dataset("sample_b.bdamage.txt", {10: 10, 11: 30, 20: 20}),
    ]
    species_a = TreeNodeModel(
        taxid=11,
        parent=10,
        rank="species",
        name="Species A",
        direct=60,
        direct_by_source=[30, 30],
        total=60,
        total_by_source=[30, 30],
        depth=2,
    )
    species_low = TreeNodeModel(
        taxid=12,
        parent=10,
        rank="species",
        name="Low Species",
        direct=5,
        direct_by_source=[5, 0],
        total=5,
        total_by_source=[5, 0],
        depth=2,
    )
    clade = TreeNodeModel(
        taxid=10,
        parent=1,
        rank="kingdom",
        name="Clade A",
        direct=15,
        direct_by_source=[10, 5],
        total=80,
        total_by_source=[45, 35],
        children=[species_a, species_low],
        depth=1,
    )
    species_b = TreeNodeModel(
        taxid=20,
        parent=1,
        rank="species",
        name="Species B",
        direct=20,
        direct_by_source=[0, 20],
        total=20,
        total_by_source=[0, 20],
        depth=1,
    )
    root = TreeNodeModel(
        taxid=1,
        parent=1,
        rank="no rank",
        name="root",
        direct=0,
        direct_by_source=[0, 0],
        total=100,
        total_by_source=[45, 55],
        children=[clade, species_b],
        depth=0,
    )
    selection = SelectionModel(
        dataset_names=tuple(
            dataset.fileinfo.name
            for dataset in datasets
        ),
        datasets=datasets,
        direct_counts={10: 15, 11: 60, 12: 5, 20: 20},
        names_map={},
        total_reads=100,
        total_taxa=8,
    )
    nodes_fileinfo = FileInfo(
        name="nodes.dmp",
        size=20,
        modified_at=1.0,
    )
    taxonomy = TaxonomyModel(
        nodes_fileinfo=nodes_fileinfo,
        names_fileinfo=None,
        nodes_map={},
        names_map={},
    )
    tree = TreeModel(
        root=root,
        missing_taxids=[],
        dataset_names=selection.dataset_names,
        taxonomy_key=taxonomy.cache_key(),
    )
    return selection, taxonomy, tree


def test_taxid_normalization_is_stable_and_unique() -> None:
    assert normalize_taxids([10, "11", 10, "bad", None]) == [10, 11]


def test_visible_tree_preserves_root_and_filters_descendants(
    tree_context: tuple[SelectionModel, TaxonomyModel, TreeModel],
) -> None:
    _, _, tree = tree_context

    payload, expanded = build_visible_tree_payload(
        tree,
        expanded_taxids={10, 12, 999},
        min_reads=10,
    )

    assert payload["taxid"] == 1
    assert [child["taxid"] for child in payload["children"]] == [10, 20]
    clade = payload["children"][0]
    assert clade["child_count"] == 1
    assert [child["taxid"] for child in clade["children"]] == [11]
    assert expanded == [10]

    root_only, _ = build_visible_tree_payload(
        tree,
        expanded_taxids=set(),
        min_reads=101,
    )
    assert root_only["taxid"] == 1
    assert root_only["children"] == []


def test_filtering_lineage_and_dataset_breakdown(
    tree_context: tuple[SelectionModel, TaxonomyModel, TreeModel],
) -> None:
    selection, _, tree = tree_context
    clade = tree.root.find_taxid(10)
    species = tree.root.find_taxid(11)
    low_species = tree.root.find_taxid(12)
    assert clade is not None
    assert species is not None
    assert low_species is not None

    assert node_passes_filter(tree.root, tree, 1_000) is True
    assert node_passes_filter(low_species, tree, 10) is False
    assert filtered_child_count(clade, 10) == 1
    assert [entry["taxid"] for entry in build_lineage(species, tree)] == [
        1,
        10,
        11,
    ]
    assert dataset_breakdown(selection, clade) == [
        {
            "dataset": "sample_a.bdamage.txt",
            "direct": 10,
            "subtree": 45,
        },
        {
            "dataset": "sample_b.bdamage.txt",
            "direct": 5,
            "subtree": 35,
        },
    ]


def test_node_resolution_distinguishes_missing_and_filtered_nodes(
    tree_context: tuple[SelectionModel, TaxonomyModel, TreeModel],
) -> None:
    _, _, tree = tree_context
    context = {"min_reads": 10}

    assert require_node_present(tree, 10, context).taxid == 10

    with pytest.raises(TreeServiceError) as missing:
        resolve_node_in_context(tree, 999, 10, context)
    assert missing.value.status_code == 404
    assert missing.value.detail["code"] == "node_not_in_active_tree"

    with pytest.raises(TreeServiceError) as filtered:
        resolve_node_in_context(tree, 12, 10, context)
    assert filtered.value.status_code == 409
    assert filtered.value.detail["code"] == "node_filtered_out"
    assert filtered.value.detail["node_subtree_reads"] == 5


def test_uncollapse_collection_uses_current_filter(
    tree_context: tuple[SelectionModel, TaxonomyModel, TreeModel],
) -> None:
    _, _, tree = tree_context
    clade = tree.root.find_taxid(10)
    assert clade is not None
    expanded: set[int] = set()

    collect_expandable_taxids(
        clade,
        tree,
        min_reads=10,
        expanded_taxids=expanded,
    )

    assert expanded == {10}


class _ResponseStore:
    def dataset_summary_payload(self, dataset: DatasetModel) -> dict[str, Any]:
        return dataset.to_summary_payload()

    def metadata_summary_payload(self) -> None:
        return None

    def cache_status(self) -> dict[str, Any]:
        return {"tree_cache_entries": 1}


def test_visible_tree_response_preserves_backend_payload_shape(
    tree_context: tuple[SelectionModel, TaxonomyModel, TreeModel],
) -> None:
    selection, taxonomy, tree = tree_context

    payload = visible_tree_response(
        _ResponseStore(),
        selection,
        taxonomy,
        tree,
        min_reads=10,
        expanded_taxids={10},
    )

    assert payload["ok"] is True
    assert payload["expanded_taxids"] == [10]
    assert payload["total_reads"] == 100
    assert payload["direct_taxa"] == 4
    assert payload["request_context"] == response_context(
        selection,
        taxonomy,
        10,
        [10],
    )


class _ResolutionStore:
    def __init__(
        self,
        selection: SelectionModel,
        taxonomy: TaxonomyModel,
        tree: TreeModel,
    ) -> None:
        self.selection = selection
        self.taxonomy = taxonomy
        self.tree = tree

    def list_files(self) -> list[Any]:
        return [
            type("File", (), {"name": name})()
            for name in self.selection.dataset_names
        ]

    def build_selection(self, names: list[str]) -> SelectionModel:
        assert names == list(self.selection.dataset_names)
        return self.selection

    def get_or_load_taxonomy(
        self,
        nodes_name: str | None = None,
        names_name: str | None = None,
    ) -> TaxonomyModel:
        assert nodes_name == "nodes.dmp"
        assert names_name is None
        return self.taxonomy

    def build_tree_model(
        self,
        selection: SelectionModel,
        taxonomy: TaxonomyModel,
    ) -> TreeModel:
        assert selection is self.selection
        assert taxonomy is self.taxonomy
        return self.tree


def test_selection_and_tree_resolution_uses_explicit_store(
    tree_context: tuple[SelectionModel, TaxonomyModel, TreeModel],
) -> None:
    selection, taxonomy, tree = tree_context
    store = _ResolutionStore(selection, taxonomy, tree)

    resolved = resolve_selection_and_tree(
        store,
        files=None,
        nodes_file="nodes.dmp",
        names_file=None,
    )

    assert resolved == (selection, taxonomy, tree)

    store.selection = SelectionModel(
        dataset_names=(),
        datasets=[],
        direct_counts={},
        names_map={},
        total_reads=0,
        total_taxa=0,
    )
    with pytest.raises(TreeServiceError) as empty:
        resolve_selection_and_tree(
            store,
            files=None,
            nodes_file=None,
            names_file=None,
        )
    assert empty.value.status_code == 400
