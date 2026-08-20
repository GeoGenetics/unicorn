from __future__ import annotations

import asyncio
from dataclasses import dataclass, replace
from functools import partial
from io import BytesIO
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest
from fastapi import HTTPException
from starlette.datastructures import UploadFile

from unicorn_backend.config import BackendConfig, load_backend_config
from unicorn_backend.routers import compute as compute_routes
from unicorn_backend.routers import core as core_routes
from unicorn_backend.routers import reports as report_routes
from unicorn_backend.routers import tree as tree_routes
from unicorn_backend.store import GraphEngineStore
from tests.unit.damage_helpers import damage_row, wide_bdamage_text


@dataclass(frozen=True)
class BackendFixture:
    upload_dir: Path
    dataset_names: list[str]
    config: BackendConfig
    store: GraphEngineStore
    api: SimpleNamespace


@pytest.fixture()
def backend_fixture(
    tmp_path: Path,
) -> BackendFixture:
    dataset_a = tmp_path / "sample_a.bdamage.txt"
    dataset_b = tmp_path / "sample_b.bdamage.txt"
    dataset_a.write_text(
        wide_bdamage_text([
            damage_row(10, 5, "Clade A", subtree_count=8),
            damage_row(11, 3, "Species A"),
            damage_row(20, 2, "Species B"),
        ]),
        encoding="utf-8",
    )
    dataset_b.write_text(
        wide_bdamage_text([
            damage_row(10, 1, "Clade A", subtree_count=8),
            damage_row(11, 7, "Species A"),
            damage_row(20, 4, "Species B"),
        ]),
        encoding="utf-8",
    )
    (tmp_path / "nodes.dmp").write_text(
        "\n".join([
            "1 | 1 | no rank |",
            "10 | 1 | kingdom |",
            "11 | 10 | species |",
            "20 | 1 | species |",
        ])
        + "\n",
        encoding="utf-8",
    )
    (tmp_path / "names.dmp").write_text(
        "\n".join([
            "1 | root | | scientific name |",
            "10 | Clade A | | scientific name |",
            "11 | Species A | | scientific name |",
            "20 | Species B | | scientific name |",
        ])
        + "\n",
        encoding="utf-8",
    )
    (tmp_path / "metadata.txt").write_text(
        "\n".join([
            "dataset\tgroup\tsite",
            "sample_a.bdamage.txt\tfiltered\talpha",
            "sample_b.bdamage.txt\tunfiltered\tbeta",
            "unmatched.bdamage.txt\tunknown\telsewhere",
        ])
        + "\n",
        encoding="utf-8",
    )

    store_config = replace(
        load_backend_config(environ={}, repository_root=tmp_path),
        runtime_dir=tmp_path,
        upload_dir=tmp_path,
        agent_trace_dir=tmp_path / "logs" / "agent_trace",
        nodes_filename="nodes.dmp",
        names_filename="names.dmp",
        metadata_filename="metadata.txt",
    )
    store = GraphEngineStore(config=store_config)
    api = SimpleNamespace(
        ping=partial(core_routes.ping, store=store, config=store_config),
        upload=partial(core_routes.upload, store=store, config=store_config),
        upload_metadata=partial(
            core_routes.upload_metadata,
            store=store,
            config=store_config,
        ),
        metadata_status=partial(core_routes.metadata_status, store=store),
        list_datasets=partial(core_routes.list_datasets, store=store),
        taxonomy_status=partial(core_routes.taxonomy_status, store=store),
        model_status=partial(
            core_routes.model_status,
            store=store,
            config=store_config,
        ),
        render_data=partial(core_routes.render_data, store=store),
        tree_model=partial(core_routes.tree_model, store=store),
        root_view=partial(tree_routes.root_view, store=store),
        tree_view=partial(tree_routes.tree_view, store=store),
        expand_node=partial(tree_routes.expand_node, store=store),
        node_tooltip=partial(tree_routes.node_tooltip, store=store),
        uncollapse_to_tips=partial(
            tree_routes.uncollapse_to_tips,
            store=store,
        ),
        table_view=partial(report_routes.table_view, store=store),
        subtree_report=partial(report_routes.subtree_report, store=store),
        subtree_report_post=partial(
            report_routes.subtree_report_post,
            store=store,
        ),
        rank_report=partial(report_routes.rank_report, store=store),
        rank_report_post=partial(
            report_routes.rank_report_post,
            store=store,
        ),
        compute_barplot=partial(compute_routes.compute_barplot, store=store),
        compute_pcoa=partial(compute_routes.compute_pcoa, store=store),
    )

    return BackendFixture(
        upload_dir=tmp_path,
        dataset_names=[dataset_a.name, dataset_b.name],
        config=store_config,
        store=store,
        api=api,
    )


def _assert_exact_keys(payload: dict[str, Any], expected: set[str]) -> None:
    assert set(payload) == expected


def _assert_http_error(
    call,
    *,
    status_code: int,
    code: str | None = None,
) -> dict[str, Any]:
    with pytest.raises(HTTPException) as caught:
        call()
    assert caught.value.status_code == status_code
    assert isinstance(caught.value.detail, dict)
    if code is not None:
        assert caught.value.detail["code"] == code
    return caught.value.detail


def test_core_status_and_dataset_contracts(
    backend_fixture: BackendFixture,
) -> None:
    ping = backend_fixture.api.ping()
    _assert_exact_keys(
        ping,
        {
            "ok",
            "service",
            "timestamp",
            "upload_dir",
            "datasets",
            "taxonomy",
            "metadata",
            "cache",
        },
    )
    assert ping["ok"] is True
    assert ping["service"] == "unicorn-graphengine-prototype"
    assert ping["upload_dir"] == str(backend_fixture.upload_dir)
    assert ping["datasets"] == 2
    assert ping["taxonomy"] == {
        "nodes_file": "nodes.dmp",
        "names_file": "names.dmp",
    }
    assert ping["metadata"]["fields"] == ["group", "site"]
    assert ping["metadata"]["rows_total"] == 3
    assert ping["metadata"]["matched_rows"] == 2
    assert ping["metadata"]["unmatched_rows"] == 1

    datasets = backend_fixture.api.list_datasets()
    _assert_exact_keys(datasets, {"ok", "datasets", "metadata"})
    assert [entry["filename"] for entry in datasets["datasets"]] == (
        backend_fixture.dataset_names
    )
    assert [entry["total_reads"] for entry in datasets["datasets"]] == [10, 12]
    assert datasets["datasets"][0]["metadata"] == {
        "group": "filtered",
        "site": "alpha",
    }

    metadata = backend_fixture.api.metadata_status()
    assert metadata == {
        "ok": True,
        "metadata": datasets["metadata"],
    }


def test_taxonomy_model_render_and_tree_contracts(
    backend_fixture: BackendFixture,
) -> None:
    taxonomy = backend_fixture.api.taxonomy_status(
        nodes_file=None,
        names_file=None,
    )
    _assert_exact_keys(taxonomy, {"ok", "taxonomy", "cache"})
    assert taxonomy["taxonomy"]["node_count"] == 4
    assert taxonomy["taxonomy"]["name_count"] == 4
    assert taxonomy["taxonomy"]["nodes_file"]["filename"] == "nodes.dmp"
    assert taxonomy["taxonomy"]["names_file"]["filename"] == "names.dmp"

    model = backend_fixture.api.model_status(
        files=backend_fixture.dataset_names,
        nodes_file=None,
        names_file=None,
    )
    _assert_exact_keys(
        model,
        {
            "ok",
            "upload_dir",
            "available_datasets",
            "selection",
            "metadata",
            "taxonomy",
            "tree",
            "cache",
        },
    )
    assert model["selection"]["dataset_count"] == 2
    assert model["selection"]["total_reads"] == 22
    assert model["selection"]["total_taxa"] == 6
    assert model["selection"]["direct_taxa"] == 3
    assert model["tree"] == {
        "root_taxid": 1,
        "root_name": "root",
        "root_total": 22,
        "node_count": 4,
        "missing_taxids": 0,
        "missing_taxid_examples": [],
    }

    render = backend_fixture.api.render_data(
        files=backend_fixture.dataset_names,
    )
    _assert_exact_keys(
        render,
        {
            "ok",
            "datasets",
            "total_reads",
            "total_taxa",
            "direct_taxa",
        },
    )
    assert render["total_reads"] == 22
    assert render["total_taxa"] == 6
    assert render["direct_taxa"] == 3

    tree = backend_fixture.api.tree_model(
        files=backend_fixture.dataset_names,
        nodes_file=None,
        names_file=None,
    )
    _assert_exact_keys(
        tree,
        {
            "ok",
            "datasets",
            "taxonomy",
            "tree",
            "missing_taxids",
            "metadata",
            "cache",
        },
    )
    assert tree["tree"]["taxid"] == 1
    assert tree["tree"]["total"] == 22
    assert [child["taxid"] for child in tree["tree"]["children"]] == [10, 20]
    assert tree["missing_taxids"] == []


def test_dataset_and_metadata_upload_contracts(
    backend_fixture: BackendFixture,
) -> None:
    dataset_upload = UploadFile(
        BytesIO(b'30\t9\t"Uploaded Species"\n'),
        filename="../../uploaded.bdamage.txt",
    )
    uploaded = asyncio.run(backend_fixture.api.upload(dataset_upload))
    assert uploaded == {
        "ok": True,
        "filename": "uploaded.bdamage.txt",
        "bytes": 24,
        "saved_to": str(
            backend_fixture.upload_dir / "uploaded.bdamage.txt"
        ),
    }
    assert (backend_fixture.upload_dir / "uploaded.bdamage.txt").read_bytes() == (
        b'30\t9\t"Uploaded Species"\n'
    )

    metadata_upload = UploadFile(
        BytesIO(
            b"dataset\tcohort\n"
            b"sample_a.bdamage.txt\tone\n"
            b"sample_b.bdamage.txt\ttwo\n"
        ),
        filename="replacement.tsv",
    )
    result = asyncio.run(
        backend_fixture.api.upload_metadata(metadata_upload)
    )
    _assert_exact_keys(result, {"ok", "metadata", "saved_to"})
    assert result["metadata"]["filename"] == "metadata.txt"
    assert result["metadata"]["fields"] == ["cohort"]
    assert result["metadata"]["matched_rows"] == 2
    assert result["saved_to"] == str(
        backend_fixture.upload_dir / "metadata.txt"
    )


def test_invalid_metadata_upload_is_structured_and_removed(
    backend_fixture: BackendFixture,
) -> None:
    invalid_upload = UploadFile(
        BytesIO(b"group\tsite\nfiltered\talpha\n"),
        filename="invalid.tsv",
    )
    detail = _assert_http_error(
        lambda: asyncio.run(
            backend_fixture.api.upload_metadata(invalid_upload)
        ),
        status_code=400,
        code="metadata_missing_dataset_column",
    )
    assert detail["message"] == (
        "Metadata file must contain a 'dataset' header column."
    )
    assert not (backend_fixture.upload_dir / "metadata.txt").exists()


def test_visible_tree_expansion_tooltip_and_table_contracts(
    backend_fixture: BackendFixture,
) -> None:
    root = backend_fixture.api.root_view(
        files=backend_fixture.dataset_names,
        nodes_file=None,
        names_file=None,
        min_reads=0,
        expanded=None,
    )
    _assert_exact_keys(
        root,
        {
            "ok",
            "datasets",
            "taxonomy",
            "tree",
            "missing_taxids",
            "expanded_taxids",
            "requested_expanded_taxids",
            "taxonomy_only",
            "min_reads",
            "total_reads",
            "direct_taxa",
            "request_context",
            "metadata",
            "cache",
        },
    )
    assert root["tree"]["taxid"] == 1
    assert root["tree"]["total"] == 22
    assert [child["taxid"] for child in root["tree"]["children"]] == [10, 20]
    assert root["expanded_taxids"] == []
    assert root["requested_expanded_taxids"] == []
    assert root["taxonomy_only"] is False

    taxonomy_only = backend_fixture.api.tree_view({
        "files": [],
        "nodes_file": None,
        "names_file": None,
        "min_reads": 10_000,
        "expanded_taxids": [10],
        "taxonomy_only": True,
    })
    assert taxonomy_only["taxonomy_only"] is True
    assert taxonomy_only["datasets"] == []
    assert taxonomy_only["total_reads"] == 0
    assert taxonomy_only["direct_taxa"] == 0
    assert taxonomy_only["tree"]["taxid"] == 1
    clade = next(
        child
        for child in taxonomy_only["tree"]["children"]
        if child["taxid"] == 10
    )
    assert [child["taxid"] for child in clade["children"]] == [11]

    posted = backend_fixture.api.tree_view({
        "files": backend_fixture.dataset_names,
        "nodes_file": None,
        "names_file": None,
        "min_reads": 0,
        "expanded_taxids": [10],
    })
    assert posted["expanded_taxids"] == [10]
    assert posted["requested_expanded_taxids"] == [10]

    expanded = backend_fixture.api.expand_node(
        taxid=10,
        files=backend_fixture.dataset_names,
        nodes_file=None,
        names_file=None,
        min_reads=0,
        expanded=None,
    )
    assert expanded["expanded_taxids"] == [10]
    assert expanded["requested_expanded_taxids"] == [10]
    clade = next(
        child for child in expanded["tree"]["children"]
        if child["taxid"] == 10
    )
    assert [child["taxid"] for child in clade["children"]] == [11]

    tooltip = backend_fixture.api.node_tooltip(
        taxid=10,
        files=backend_fixture.dataset_names,
        nodes_file=None,
        names_file=None,
        min_reads=0,
    )
    _assert_exact_keys(tooltip, {"ok", "node", "request_context"})
    assert tooltip["node"]["direct"] == 6
    assert tooltip["node"]["subtree"] == 16
    assert tooltip["node"]["child_count"] == 1
    assert [entry["taxid"] for entry in tooltip["node"]["lineage"]] == [1, 10]
    assert [entry["direct"] for entry in tooltip["node"]["datasets"]] == [5, 1]

    table = backend_fixture.api.table_view(
        scope="root",
        taxid=None,
        files=backend_fixture.dataset_names,
        nodes_file=None,
        names_file=None,
        min_reads=0,
        sort="direct",
        limit=40,
    )
    _assert_exact_keys(
        table,
        {
            "ok",
            "scope",
            "target",
            "sort",
            "limit",
            "row_count",
            "rows",
            "request_context",
        },
    )
    assert table["row_count"] == 3
    assert [row["taxid"] for row in table["rows"]] == [11, 10, 20]

    uncollapsed = backend_fixture.api.uncollapse_to_tips({
        "taxids": [10],
        "files": backend_fixture.dataset_names,
        "nodes_file": None,
        "names_file": None,
        "min_reads": 0,
        "expanded_taxids": [],
    })
    assert uncollapsed["expanded_taxids"] == [10]
    assert uncollapsed["requested_expanded_taxids"] == [10]


def test_get_and_post_report_contracts_match(
    backend_fixture: BackendFixture,
) -> None:
    subtree_get = backend_fixture.api.subtree_report(
        taxid=None,
        taxids=[10, 20],
        files=backend_fixture.dataset_names,
        nodes_file=None,
        names_file=None,
        min_reads=0,
    )
    subtree_post = backend_fixture.api.subtree_report_post({
        "taxids": [10, 20],
        "files": backend_fixture.dataset_names,
        "min_reads": 0,
    })
    assert subtree_post == subtree_get
    assert subtree_get["report"]["summary"] == {
        "selected_taxids": [10, 20],
        "selected_node_count": 2,
        "dataset_names": backend_fixture.dataset_names,
        "total_direct": 12,
        "total_subtree": 22,
    }
    assert subtree_get["report"]["matrix"]["row_count"] == 2
    assert subtree_get["report"]["matrix"]["rows"][0]["datasets"] == [
        {
            "dataset": "sample_a.bdamage.txt",
            "direct": 5,
            "subtree": 8,
        },
        {
            "dataset": "sample_b.bdamage.txt",
            "direct": 1,
            "subtree": 8,
        },
    ]

    rank_get = backend_fixture.api.rank_report(
        taxids=[11, 20],
        files=backend_fixture.dataset_names,
        nodes_file=None,
        names_file=None,
        min_reads=0,
    )
    rank_post = backend_fixture.api.rank_report_post({
        "taxids": [11, 20],
        "files": backend_fixture.dataset_names,
        "min_reads": 0,
    })
    assert rank_post == rank_get
    assert rank_get["report"]["summary"] == {
        "selected_node_count": 2,
        "selected_taxids": [11, 20],
        "total_direct": 16,
        "dataset_names": backend_fixture.dataset_names,
    }
    assert rank_get["report"]["rows"] == [{
        "rank": "species",
        "direct": 16,
        "node_count": 2,
        "datasets": [
            {"dataset": "sample_a.bdamage.txt", "direct": 5},
            {"dataset": "sample_b.bdamage.txt", "direct": 11},
        ],
    }]


def test_barplot_and_pcoa_compute_contracts(
    backend_fixture: BackendFixture,
) -> None:
    common_payload = {
        "taxids": [10, 20],
        "files": backend_fixture.dataset_names,
        "nodes_file": None,
        "names_file": None,
        "min_reads": 0,
        "count_mode": "direct",
    }
    barplot = backend_fixture.api.compute_barplot({
        **common_payload,
        "dataset_colors": {
            "sample_a.bdamage.txt": "#112233",
            "sample_b.bdamage.txt": "#445566",
        },
    })
    _assert_exact_keys(barplot, {"ok", "spec", "request_context"})
    assert barplot["spec"]["plot_type"] == "barplot"
    assert barplot["spec"]["count_mode"] == "direct"
    assert [trace["y"] for trace in barplot["spec"]["traces"]] == [
        [5, 2],
        [1, 4],
    ]

    pcoa = backend_fixture.api.compute_pcoa({
        **common_payload,
        "distance_metric": "bray_curtis",
    })
    _assert_exact_keys(pcoa, {"ok", "spec", "request_context"})
    assert pcoa["spec"]["plot_type"] == "pcoa"
    assert pcoa["spec"]["distance_metric"] == "bray_curtis"
    assert pcoa["spec"]["summary"] == {
        "selected_node_count": 2,
        "dataset_count": 2,
        "feature_count": 2,
    }
    assert [point["dataset"] for point in pcoa["spec"]["points"]] == (
        backend_fixture.dataset_names
    )


def test_representative_backend_errors_are_frozen(
    backend_fixture: BackendFixture,
) -> None:
    missing_dataset = _assert_http_error(
        lambda: backend_fixture.api.render_data(
            files=["missing.bdamage.txt"]
        ),
        status_code=404,
    )
    assert missing_dataset == {
        "message": "Requested dataset(s) not found in upload directory.",
        "missing": ["missing.bdamage.txt"],
    }

    missing_taxonomy = _assert_http_error(
        lambda: backend_fixture.api.taxonomy_status(
            nodes_file="missing.dmp",
            names_file=None,
        ),
        status_code=404,
    )
    assert missing_taxonomy == {
        "message": "Taxonomy nodes file not found on the backend.",
        "nodes_file": "missing.dmp",
    }

    invalid_taxid = _assert_http_error(
        lambda: backend_fixture.api.expand_node(
            taxid=999,
            files=backend_fixture.dataset_names,
            nodes_file=None,
            names_file=None,
            min_reads=0,
            expanded=None,
        ),
        status_code=404,
        code="node_not_in_active_tree",
    )
    assert invalid_taxid["taxid"] == 999

    filtered_taxid = _assert_http_error(
        lambda: backend_fixture.api.node_tooltip(
            taxid=20,
            files=backend_fixture.dataset_names,
            nodes_file=None,
            names_file=None,
            min_reads=7,
        ),
        status_code=409,
        code="node_filtered_out",
    )
    assert filtered_taxid["node_subtree_reads"] == 6
    assert filtered_taxid["min_reads"] == 7

    _assert_http_error(
        lambda: backend_fixture.api.table_view(
            scope="invalid",
            taxid=None,
            files=backend_fixture.dataset_names,
            nodes_file=None,
            names_file=None,
            min_reads=0,
            sort="direct",
            limit=40,
        ),
        status_code=400,
        code="invalid_scope",
    )
    _assert_http_error(
        lambda: backend_fixture.api.subtree_report(
            taxid=None,
            taxids=None,
            files=backend_fixture.dataset_names,
            nodes_file=None,
            names_file=None,
            min_reads=0,
        ),
        status_code=400,
        code="missing_taxids",
    )
    _assert_http_error(
        lambda: backend_fixture.api.compute_barplot({
            "taxids": [10],
            "files": backend_fixture.dataset_names,
            "count_mode": "invalid",
        }),
        status_code=400,
        code="invalid_barplot_request",
    )
