from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path

from unicorn_backend.models import (
    DatasetModel,
    FileInfo,
    MetadataModel,
    SelectionModel,
    TaxonomyModel,
    TaxonomyNode,
    TreeModel,
    TreeNodeModel,
)


def _file_info(name: str = "sample.bdamage.txt") -> FileInfo:
    return FileInfo(name=name, size=42, modified_at=1_700_000_000.0)


def _dataset() -> DatasetModel:
    return DatasetModel(
        fileinfo=_file_info(),
        counts_map={10: 5, 11: 3},
        names_map={10: "Clade A", 11: "Species A"},
        counts_payload=[
            {"taxid": 10, "count": 5, "name": "Clade A"},
            {"taxid": 11, "count": 3, "name": "Species A"},
        ],
        total_reads=8,
        total_taxa=2,
    )


def test_file_info_creation_fingerprint_and_payload(tmp_path: Path) -> None:
    path = tmp_path / "sample.bdamage.txt"
    path.write_bytes(b"12345")

    fileinfo = FileInfo.from_path(path)

    assert fileinfo.name == path.name
    assert fileinfo.size == 5
    assert fileinfo.fingerprint() == (5, path.stat().st_mtime)
    assert fileinfo.to_payload() == {
        "id": path.name,
        "filename": path.name,
        "bytes": 5,
        "modified_at": datetime.fromtimestamp(
            path.stat().st_mtime,
            timezone.utc,
        ).isoformat(),
    }


def test_dataset_payloads_preserve_counts_and_optional_metadata() -> None:
    dataset = _dataset()

    render_payload = dataset.to_render_payload()
    summary_payload = dataset.to_summary_payload(
        {"group": "filtered"}
    )

    assert dataset.counts_map[10] == 5
    assert render_payload["counts"] == dataset.counts_payload
    assert render_payload["total_reads"] == 8
    assert summary_payload["metadata"] == {"group": "filtered"}
    assert summary_payload["damage"] == {
        "available": False,
        "schema": None,
        "taxa": 0,
        "valid_taxa": 0,
        "invalid_taxa": 0,
        "count_scope": "direct",
        "damage_scope": "subtree",
    }
    assert "damage" not in render_payload
    assert "counts" not in summary_payload


def test_taxonomy_models_preserve_nodes_relationships_and_status() -> None:
    nodes_fileinfo = _file_info("nodes.dmp")
    names_fileinfo = _file_info("names.dmp")
    nodes = {
        1: TaxonomyNode(taxid=1, parent=1, rank="no rank"),
        10: TaxonomyNode(taxid=10, parent=1, rank="kingdom"),
    }
    taxonomy = TaxonomyModel(
        nodes_fileinfo=nodes_fileinfo,
        names_fileinfo=names_fileinfo,
        nodes_map=nodes,
        names_map={1: "root", 10: "Clade A"},
    )

    assert taxonomy.nodes_map[10].parent == 1
    assert taxonomy.cache_key() == (
        "nodes.dmp",
        nodes_fileinfo.fingerprint(),
        "names.dmp",
        names_fileinfo.fingerprint(),
    )
    assert taxonomy.to_status_payload()["node_count"] == 2
    assert taxonomy.to_status_payload()["name_count"] == 2


def test_tree_node_lookup_payload_and_independent_defaults() -> None:
    leaf = TreeNodeModel(
        taxid=11,
        parent=10,
        rank="species",
        name="Species A",
        direct=3,
        direct_by_source=[3],
        total=3,
        total_by_source=[3],
        depth=2,
    )
    root = TreeNodeModel(
        taxid=10,
        parent=1,
        rank="kingdom",
        name="Clade A",
        direct=5,
        direct_by_source=[5],
        total=8,
        total_by_source=[8],
        children=[leaf],
        depth=1,
    )
    unrelated = TreeNodeModel(
        taxid=20,
        parent=1,
        rank="species",
        name="Species B",
        direct=2,
        direct_by_source=[2],
    )

    assert root.has_taxid(11) is True
    assert root.has_taxid(20) is False
    assert root.find_taxid(11) is leaf
    assert root.find_taxid(20) is None
    assert root.to_payload()["children"][0]["taxid"] == 11
    assert unrelated.children == []
    assert unrelated.total_by_source == []


def test_selection_and_tree_status_payloads() -> None:
    dataset = _dataset()
    leaf = TreeNodeModel(
        taxid=11,
        parent=10,
        rank="species",
        name="Species A",
        direct=3,
        direct_by_source=[3],
        total=3,
        total_by_source=[3],
    )
    root = TreeNodeModel(
        taxid=10,
        parent=1,
        rank="kingdom",
        name="Clade A",
        direct=5,
        direct_by_source=[5],
        total=8,
        total_by_source=[8],
        children=[leaf],
    )
    selection = SelectionModel(
        dataset_names=(dataset.fileinfo.name,),
        datasets=[dataset],
        direct_counts={10: 5, 11: 3},
        names_map={10: "Clade A", 11: "Species A"},
        total_reads=8,
        total_taxa=2,
    )
    tree = TreeModel(
        root=root,
        missing_taxids=[99, 100],
        dataset_names=selection.dataset_names,
        taxonomy_key=("nodes.dmp", (42, 1.0), None, None),
    )

    assert selection.to_status_payload()["direct_taxa"] == 2
    assert tree.count_nodes() == 2
    assert tree.to_status_payload() == {
        "root_taxid": 10,
        "root_name": "Clade A",
        "root_total": 8,
        "node_count": 2,
        "missing_taxids": 2,
        "missing_taxid_examples": [99, 100],
    }


def test_metadata_summary_matches_only_available_datasets() -> None:
    metadata = MetadataModel(
        fileinfo=_file_info("metadata.txt"),
        fields=("group", "site"),
        rows_by_dataset={
            "sample_a.bdamage.txt": {
                "group": "filtered",
                "site": "alpha",
            },
            "unmatched.bdamage.txt": {
                "group": "unknown",
                "site": "elsewhere",
            },
        },
        rows_total=2,
    )

    assert metadata.to_status_payload({"sample_a.bdamage.txt"}) == {
        "filename": "metadata.txt",
        "fields": ["group", "site"],
        "rows_total": 2,
        "matched_rows": 1,
        "unmatched_rows": 1,
        "datasets_with_metadata": ["sample_a.bdamage.txt"],
    }
