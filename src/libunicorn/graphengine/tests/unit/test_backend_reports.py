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
from unicorn_backend.reports import (
    ReportServiceError,
    rank_report_payload,
    selected_count_matrix_report,
    subtree_report_payload,
    subtree_table_rows,
    table_view_payload,
    top_children_rows,
)
from unicorn_backend.tree import TreeServiceError


def _dataset(name: str) -> DatasetModel:
    return DatasetModel(
        fileinfo=FileInfo(name=name, size=10, modified_at=1.0),
        counts_map={},
        names_map={},
        counts_payload=[],
        total_reads=0,
        total_taxa=0,
    )


@pytest.fixture()
def report_context() -> tuple[Any, SelectionModel, TaxonomyModel, TreeModel]:
    datasets = [
        _dataset("sample_a.bdamage.txt"),
        _dataset("sample_b.bdamage.txt"),
    ]
    species_a = TreeNodeModel(
        taxid=11,
        parent=10,
        rank="species",
        name="Species A",
        direct=15,
        direct_by_source=[7, 8],
        total=15,
        total_by_source=[7, 8],
        depth=2,
    )
    clade = TreeNodeModel(
        taxid=10,
        parent=1,
        rank="kingdom",
        name="Clade A",
        direct=5,
        direct_by_source=[3, 2],
        total=20,
        total_by_source=[10, 10],
        children=[species_a],
        depth=1,
    )
    species_b = TreeNodeModel(
        taxid=20,
        parent=1,
        rank="species",
        name="Species B",
        direct=10,
        direct_by_source=[4, 6],
        total=10,
        total_by_source=[4, 6],
        depth=1,
    )
    root = TreeNodeModel(
        taxid=1,
        parent=1,
        rank="no rank",
        name="root",
        direct=0,
        direct_by_source=[0, 0],
        total=30,
        total_by_source=[14, 16],
        children=[clade, species_b],
        depth=0,
    )
    selection = SelectionModel(
        dataset_names=tuple(
            dataset.fileinfo.name
            for dataset in datasets
        ),
        datasets=datasets,
        direct_counts={10: 5, 11: 15, 20: 10},
        names_map={},
        total_reads=30,
        total_taxa=3,
    )
    taxonomy = TaxonomyModel(
        nodes_fileinfo=FileInfo(
            name="nodes.dmp",
            size=20,
            modified_at=1.0,
        ),
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
    store = _ReportStore(selection, taxonomy, tree)
    return store, selection, taxonomy, tree


class _ReportStore:
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
        return self.taxonomy

    def build_tree_model(
        self,
        selection: SelectionModel,
        taxonomy: TaxonomyModel,
    ) -> TreeModel:
        assert selection is self.selection
        assert taxonomy is self.taxonomy
        return self.tree


def test_subtree_rows_support_direct_subtree_filtering_and_limits(
    report_context: tuple[Any, SelectionModel, TaxonomyModel, TreeModel],
) -> None:
    _, _, _, tree = report_context

    direct_rows = subtree_table_rows(tree.root, 0, "direct", 10)
    subtree_rows = subtree_table_rows(tree.root, 0, "subtree", 2)
    filtered_rows = subtree_table_rows(tree.root, 12, "direct", 10)

    assert [row["taxid"] for row in direct_rows] == [11, 20, 10]
    assert [row["taxid"] for row in subtree_rows] == [10, 11]
    assert [row["taxid"] for row in filtered_rows] == [11, 10]
    assert filtered_rows[1]["child_count"] == 1


def test_top_children_rows_preserve_order_and_limit(
    report_context: tuple[Any, SelectionModel, TaxonomyModel, TreeModel],
) -> None:
    _, _, _, tree = report_context

    rows = top_children_rows(tree.root, min_reads=0, limit=1)

    assert len(rows) == 1
    assert rows[0]["taxid"] == 10
    assert rows[0]["subtree"] == 20


def test_selected_count_matrix_preserves_node_and_dataset_order(
    report_context: tuple[Any, SelectionModel, TaxonomyModel, TreeModel],
) -> None:
    _, selection, _, tree = report_context
    species_b = tree.root.find_taxid(20)
    clade = tree.root.find_taxid(10)
    assert species_b is not None
    assert clade is not None

    report = selected_count_matrix_report(
        selection,
        [species_b, clade],
    )

    assert report["summary"]["selected_taxids"] == [20, 10]
    assert report["summary"]["total_direct"] == 15
    assert report["summary"]["total_subtree"] == 30
    assert report["matrix"]["rows"][0]["datasets"] == [
        {
            "dataset": "sample_a.bdamage.txt",
            "direct": 4,
            "subtree": 4,
        },
        {
            "dataset": "sample_b.bdamage.txt",
            "direct": 6,
            "subtree": 6,
        },
    ]


def test_table_view_supports_root_and_node_scopes(
    report_context: tuple[Any, SelectionModel, TaxonomyModel, TreeModel],
) -> None:
    store, _, _, _ = report_context

    root_payload = table_view_payload(
        store,
        scope="root",
        taxid=None,
        files=None,
        nodes_file=None,
        names_file=None,
        min_reads=0,
        sort="direct",
        limit=10,
    )
    node_payload = table_view_payload(
        store,
        scope="node",
        taxid=10,
        files=None,
        nodes_file=None,
        names_file=None,
        min_reads=0,
        sort="subtree",
        limit=10,
    )

    assert root_payload["target"]["taxid"] == 1
    assert [row["taxid"] for row in root_payload["rows"]] == [
        11,
        20,
        10,
    ]
    assert node_payload["target"]["taxid"] == 10
    assert [row["taxid"] for row in node_payload["rows"]] == [
        10,
        11,
    ]


@pytest.mark.parametrize(
    ("scope", "sort", "taxid", "code"),
    [
        ("invalid", "direct", None, "invalid_scope"),
        ("root", "invalid", None, "invalid_sort"),
        ("node", "direct", None, "missing_taxid"),
    ],
)
def test_table_view_rejects_invalid_requests(
    report_context: tuple[Any, SelectionModel, TaxonomyModel, TreeModel],
    scope: str,
    sort: str,
    taxid: int | None,
    code: str,
) -> None:
    store, _, _, _ = report_context

    with pytest.raises(ReportServiceError) as error:
        table_view_payload(
            store,
            scope=scope,
            taxid=taxid,
            files=None,
            nodes_file=None,
            names_file=None,
            min_reads=0,
            sort=sort,
            limit=10,
        )

    assert error.value.status_code == 400
    assert error.value.detail["code"] == code


def test_subtree_and_rank_reports_preserve_selection_semantics(
    report_context: tuple[Any, SelectionModel, TaxonomyModel, TreeModel],
) -> None:
    store, _, _, _ = report_context

    subtree = subtree_report_payload(
        store,
        taxid=10,
        taxids=[20, 10, 20],
        files=None,
        nodes_file=None,
        names_file=None,
        min_reads=0,
    )
    rank = rank_report_payload(
        store,
        taxids=[10, 11, 20],
        files=None,
        nodes_file=None,
        names_file=None,
        min_reads=0,
    )

    assert subtree["report"]["summary"]["selected_taxids"] == [20, 10]
    assert rank["report"]["summary"]["selected_taxids"] == [10, 11, 20]
    assert rank["report"]["summary"]["total_direct"] == 30
    assert rank["report"]["rows"][0] == {
        "rank": "species",
        "direct": 25,
        "node_count": 2,
        "datasets": [
            {
                "dataset": "sample_a.bdamage.txt",
                "direct": 11,
            },
            {
                "dataset": "sample_b.bdamage.txt",
                "direct": 14,
            },
        ],
    }


def test_reports_reject_empty_and_missing_taxid_selections(
    report_context: tuple[Any, SelectionModel, TaxonomyModel, TreeModel],
) -> None:
    store, _, _, _ = report_context

    with pytest.raises(ReportServiceError) as empty:
        rank_report_payload(
            store,
            taxids=[],
            files=None,
            nodes_file=None,
            names_file=None,
            min_reads=0,
        )
    assert empty.value.detail["code"] == "missing_taxids"

    with pytest.raises(TreeServiceError) as missing:
        subtree_report_payload(
            store,
            taxid=None,
            taxids=[999],
            files=None,
            nodes_file=None,
            names_file=None,
            min_reads=0,
        )
    assert missing.value.detail["code"] == "node_not_in_active_tree"
