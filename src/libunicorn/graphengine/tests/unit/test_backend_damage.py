from __future__ import annotations

import json
from pathlib import Path

import pytest
from fastapi import HTTPException
from pydantic import ValidationError

import unicorn_backend.damage as damage_module
from tests.unit.damage_helpers import damage_row, wide_bdamage_text
from unicorn_backend.config import BackendConfig
from unicorn_backend.damage import (
    damage_node_payload,
    damage_selected_payload,
)
from unicorn_backend.damage_contract import BDAMAGE_SCHEMA_VERSION
from unicorn_backend.routers.damage import (
    MAX_SELECTED_DAMAGE_TAXIDS,
    DamageNodeRequest,
    DamageSelectedRequest,
    damage_node,
    damage_selected,
)
from unicorn_backend.store import GraphEngineStore


@pytest.fixture()
def damage_store(tmp_path: Path) -> GraphEngineStore:
    (tmp_path / "sample_a.bdamage.txt").write_text(
        wide_bdamage_text([
            damage_row(10, 5, "Clade A"),
            damage_row(11, 3, "Species A", nll="nan"),
        ]),
        encoding="utf-8",
    )
    (tmp_path / "sample_b.bdamage.txt").write_text(
        wide_bdamage_text([
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
            "99 | 1 | species |",
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
            "99 | Unobserved Species | | scientific name |",
        ])
        + "\n",
        encoding="utf-8",
    )
    config = BackendConfig(
        host="127.0.0.1",
        port=8000,
        runtime_dir=tmp_path,
        upload_dir=tmp_path,
        agent_trace_dir=tmp_path / "logs" / "agent_trace",
        nodes_filename="nodes.dmp",
        names_filename="names.dmp",
        metadata_filename="metadata.txt",
    )
    return GraphEngineStore(config=config)


def test_dataset_summary_exposes_compact_damage_capability(
    damage_store: GraphEngineStore,
) -> None:
    dataset = damage_store.get_or_load_dataset(
        damage_store.config.upload_dir / "sample_a.bdamage.txt"
    )
    summary = dataset.to_summary_payload()

    assert summary["damage"] == {
        "available": True,
        "schema": BDAMAGE_SCHEMA_VERSION,
        "taxa": 2,
        "valid_taxa": 1,
        "invalid_taxa": 1,
        "count_scope": "direct",
        "damage_scope": "subtree",
    }
    assert "damage" not in dataset.to_render_payload()
    assert "damage_by_taxid" not in summary


def test_damage_node_returns_valid_and_missing_dataset_profiles(
    damage_store: GraphEngineStore,
) -> None:
    response = damage_node_payload(
        damage_store,
        taxid=10,
        files=[
            "sample_a.bdamage.txt",
            "sample_b.bdamage.txt",
        ],
        nodes_file=None,
        names_file=None,
    )

    assert response["taxid"] == 10
    assert response["name"] == "Clade A"
    assert response["count_scope"] == "direct"
    assert response["damage_scope"] == "subtree"
    assert response["request_context"]["dataset_names"] == [
        "sample_a.bdamage.txt",
        "sample_b.bdamage.txt",
    ]
    valid, missing = response["datasets"]
    assert valid["dataset"] == "sample_a.bdamage.txt"
    assert valid["direct_count"] == 5
    assert valid["profile_present"] is True
    assert valid["profile_status"] == "valid"
    assert valid["fit_valid"] is True
    assert valid["observed"]["positions"][0]["ct_frequency"] == pytest.approx(
        1.5 / 10.5
    )
    assert valid["fit"]["positions"][0]["dx5"] == 0.1
    assert missing == {
        "dataset": "sample_b.bdamage.txt",
        "direct_count": 0,
        "profile_present": False,
        "profile_status": "missing",
        "fit_valid": None,
        "missing_fields": [],
        "observed": None,
        "fit": None,
    }
    json.dumps(response, allow_nan=False)


def test_damage_node_keeps_invalid_profile_explicit(
    damage_store: GraphEngineStore,
) -> None:
    response = damage_node(
        DamageNodeRequest(
            taxid=11,
            files=[
                "sample_a.bdamage.txt",
                "sample_b.bdamage.txt",
            ],
        ),
        store=damage_store,
    )
    invalid, missing = response["datasets"]

    assert invalid["profile_status"] == "invalid"
    assert invalid["fit_valid"] is False
    assert invalid["missing_fields"] == ["nll"]
    assert invalid["fit"]["nll"] is None
    assert missing["profile_status"] == "missing"
    json.dumps(response, allow_nan=False)


def test_damage_selected_returns_compact_multi_node_payload(
    damage_store: GraphEngineStore,
) -> None:
    response = damage_selected_payload(
        damage_store,
        taxids=[10, 11],
        files=[
            "sample_a.bdamage.txt",
            "sample_b.bdamage.txt",
        ],
        nodes_file=None,
        names_file=None,
    )

    assert response["count_scope"] == "direct"
    assert response["damage_scope"] == "subtree"
    assert response["request_context"]["taxids"] == [10, 11]
    assert [
        (node["taxid"], node["name"])
        for node in response["nodes"]
    ] == [
        (10, "Clade A"),
        (11, "Species A"),
    ]
    assert "request_context" not in response["nodes"][0]
    assert [
        dataset["profile_status"]
        for dataset in response["nodes"][1]["datasets"]
    ] == ["invalid", "missing"]
    json.dumps(response, allow_nan=False)


def test_damage_selected_route_wrapper_uses_shared_contract(
    damage_store: GraphEngineStore,
) -> None:
    response = damage_selected(
        DamageSelectedRequest(
            taxids=[10, 20],
            files=[
                "sample_a.bdamage.txt",
                "sample_b.bdamage.txt",
            ],
        ),
        store=damage_store,
    )

    assert [node["taxid"] for node in response["nodes"]] == [10, 20]
    assert len(response["nodes"][0]["datasets"]) == 2


def test_damage_selected_rejects_oversized_profile_matrix(
    damage_store: GraphEngineStore,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        damage_module,
        "MAX_SELECTED_DAMAGE_PROFILES",
        3,
    )

    with pytest.raises(
        damage_module.DamageServiceError,
    ) as caught:
        damage_selected_payload(
            damage_store,
            taxids=[10, 11],
            files=[
                "sample_a.bdamage.txt",
                "sample_b.bdamage.txt",
            ],
            nodes_file=None,
            names_file=None,
        )

    assert caught.value.status_code == 422
    assert caught.value.detail == {
        "message": (
            "Selected damage table is too large. Narrow the selected "
            "taxids or datasets."
        ),
        "code": "damage_selection_too_large",
        "selected_taxids": 2,
        "selected_datasets": 2,
        "requested_profiles": 4,
        "maximum_profiles": 3,
    }


def test_damage_node_rejects_missing_dataset(
    damage_store: GraphEngineStore,
) -> None:
    with pytest.raises(HTTPException) as caught:
        damage_node_payload(
            damage_store,
            taxid=10,
            files=["missing.bdamage.txt"],
            nodes_file=None,
            names_file=None,
        )

    assert caught.value.status_code == 404
    assert caught.value.detail["missing"] == ["missing.bdamage.txt"]


def test_damage_node_rejects_taxid_outside_active_tree(
    damage_store: GraphEngineStore,
) -> None:
    with pytest.raises(HTTPException) as caught:
        damage_node(
            DamageNodeRequest(
                taxid=99,
                files=[
                    "sample_a.bdamage.txt",
                    "sample_b.bdamage.txt",
                ],
            ),
            store=damage_store,
        )

    assert caught.value.status_code == 404
    assert caught.value.detail["code"] == "node_not_in_active_tree"
    assert caught.value.detail["taxid"] == 99


@pytest.mark.parametrize(
    "files",
    [
        [""],
        ["sample_a.bdamage.txt", "sample_a.bdamage.txt"],
    ],
)
def test_damage_node_request_rejects_ambiguous_dataset_lists(
    files: list[str],
) -> None:
    with pytest.raises(ValidationError):
        DamageNodeRequest(taxid=10, files=files)


@pytest.mark.parametrize(
    "taxids",
    [
        [],
        [10, 10],
        [0],
        list(range(1, MAX_SELECTED_DAMAGE_TAXIDS + 2)),
    ],
)
def test_damage_selected_request_rejects_invalid_taxid_sets(
    taxids: list[int],
) -> None:
    with pytest.raises(ValidationError):
        DamageSelectedRequest(
            taxids=taxids,
            files=["sample_a.bdamage.txt"],
        )
