from __future__ import annotations

from pathlib import Path

import pytest
from fastapi import HTTPException

from unicorn_backend.config import BackendConfig
from unicorn_backend.models import FileInfo
from unicorn_backend.store import GraphEngineStore
from tests.unit.damage_helpers import damage_row, wide_bdamage_text


def _config(upload_dir: Path, *, names_filename: str = "names.dmp") -> BackendConfig:
    runtime_dir = upload_dir.parent
    return BackendConfig(
        host="127.0.0.1",
        port=8000,
        runtime_dir=runtime_dir,
        upload_dir=upload_dir,
        agent_trace_dir=runtime_dir / "logs" / "agent_trace",
        nodes_filename="nodes.dmp",
        names_filename=names_filename,
        metadata_filename="metadata.txt",
    )


@pytest.fixture()
def store_fixture(tmp_path: Path) -> tuple[GraphEngineStore, Path, Path]:
    dataset = tmp_path / "sample.bdamage.txt"
    dataset.write_text(
        wide_bdamage_text([
            damage_row(10, 7, "Clade A"),
            damage_row(11, 3, "Species A"),
        ]),
        encoding="utf-8",
    )
    ignored = tmp_path / "ignored.txt"
    ignored.write_text("10\t999\tignored\n", encoding="utf-8")
    nodes = tmp_path / "nodes.dmp"
    nodes.write_text(
        "\n".join([
            "1 | 1 | no rank |",
            "10 | 1 | kingdom |",
            "11 | 10 | species |",
        ])
        + "\n",
        encoding="utf-8",
    )
    names = tmp_path / "names.dmp"
    names.write_text(
        "\n".join([
            "1 | root | | scientific name |",
            "10 | Clade A | | scientific name |",
            "11 | Species A | | scientific name |",
        ])
        + "\n",
        encoding="utf-8",
    )
    return GraphEngineStore(config=_config(tmp_path)), dataset, nodes


def test_dataset_discovery_parsing_and_selection_cache(
    store_fixture: tuple[GraphEngineStore, Path, Path],
) -> None:
    store, dataset_path, _ = store_fixture

    assert store.list_files() == [dataset_path]
    dataset = store.get_or_load_dataset(dataset_path)
    selection = store.build_selection([dataset_path.name])
    cached_selection = store.build_selection([dataset_path.name])

    assert dataset.counts_map == {10: 7, 11: 3}
    assert dataset.total_reads == 10
    assert dataset.total_taxa == 2
    assert dataset.damage_taxa == 2
    assert dataset.valid_damage_taxa == 2
    assert selection.direct_counts == {10: 7, 11: 3}
    assert cached_selection is selection
    assert store.cache_status()["dataset_cache_entries"] == 1
    assert store.cache_status()["selection_cache_entries"] == 1


def test_dataset_change_invalidates_selection_and_tree_caches(
    store_fixture: tuple[GraphEngineStore, Path, Path],
) -> None:
    store, dataset_path, _ = store_fixture
    selection = store.build_selection([dataset_path.name])
    taxonomy = store.get_or_load_taxonomy()
    tree = store.build_tree_model(selection, taxonomy)

    dataset_path.write_text(
        wide_bdamage_text([
            damage_row(10, 50, "Clade A", A=0.25),
            damage_row(11, 30, "Species A"),
        ]),
        encoding="utf-8",
    )
    refreshed_selection = store.build_selection([dataset_path.name])
    refreshed_tree = store.build_tree_model(
        refreshed_selection,
        taxonomy,
    )

    assert refreshed_selection is not selection
    assert refreshed_selection.total_reads == 80
    assert (
        refreshed_selection.datasets[0]
        .damage_by_taxid[10]
        .amplitude
        == 0.25
    )
    assert refreshed_tree is not tree
    assert refreshed_tree.root.total == 80


def test_taxonomy_loading_optional_names_and_tree_cache(
    store_fixture: tuple[GraphEngineStore, Path, Path],
) -> None:
    store, dataset_path, _ = store_fixture
    taxonomy = store.get_or_load_taxonomy()
    cached_taxonomy = store.get_or_load_taxonomy()
    selection = store.build_selection([dataset_path.name])
    tree = store.build_tree_model(selection, taxonomy)
    cached_tree = store.build_tree_model(selection, taxonomy)

    assert cached_taxonomy is taxonomy
    assert taxonomy.names_map[11] == "Species A"
    assert cached_tree is tree
    assert tree.root.taxid == 1
    assert tree.root.total == 10

    unnamed_store = GraphEngineStore(
        config=_config(
            store.config.upload_dir,
            names_filename="missing-names.dmp",
        )
    )
    unnamed_taxonomy = unnamed_store.get_or_load_taxonomy()
    assert unnamed_taxonomy.names_fileinfo is None
    assert unnamed_taxonomy.names_map == {}


def test_metadata_loading_replacement_and_matching_summary(
    store_fixture: tuple[GraphEngineStore, Path, Path],
) -> None:
    store, dataset_path, _ = store_fixture
    metadata_path = store.config.upload_dir / "metadata.txt"
    metadata_path.write_text(
        "dataset\tgroup\n"
        f"{dataset_path.name}\tfiltered\n"
        "unmatched.bdamage.txt\tunknown\n",
        encoding="utf-8",
    )

    first = store.get_metadata()
    assert first is not None
    assert store.metadata_summary_payload() == {
        "filename": "metadata.txt",
        "fields": ["group"],
        "rows_total": 2,
        "matched_rows": 1,
        "unmatched_rows": 1,
        "datasets_with_metadata": [dataset_path.name],
    }

    metadata_path.write_text(
        "dataset\tgroup\tsite\n"
        f"{dataset_path.name}\tunfiltered\tbeta\n",
        encoding="utf-8",
    )
    replacement = store.get_metadata()

    assert replacement is not None
    assert replacement is not first
    assert replacement.fields == ("group", "site")
    assert store.metadata_for_dataset(dataset_path.name) == {
        "group": "unfiltered",
        "site": "beta",
    }


def test_missing_dataset_and_taxonomy_errors_are_structured(
    tmp_path: Path,
) -> None:
    store = GraphEngineStore(config=_config(tmp_path))

    with pytest.raises(HTTPException) as missing_dataset:
        store.build_selection(["missing.bdamage.txt"])
    assert missing_dataset.value.status_code == 404
    assert missing_dataset.value.detail["missing"] == [
        "missing.bdamage.txt"
    ]

    with pytest.raises(HTTPException) as missing_taxonomy:
        store.get_or_load_taxonomy()
    assert missing_taxonomy.value.status_code == 404
    assert missing_taxonomy.value.detail["nodes_file"] == "nodes.dmp"


def test_unreadable_file_error_remains_structured(
    store_fixture: tuple[GraphEngineStore, Path, Path],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    store, dataset_path, _ = store_fixture

    def deny_stat(cls, path: Path) -> FileInfo:
        raise PermissionError(path)

    monkeypatch.setattr(FileInfo, "from_path", classmethod(deny_stat))

    with pytest.raises(HTTPException) as unreadable:
        store.get_or_load_dataset(dataset_path)

    assert unreadable.value.status_code == 403
    assert unreadable.value.detail["code"] == "backend_file_not_readable"
    assert unreadable.value.detail["filename"] == dataset_path.name
