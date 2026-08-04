from __future__ import annotations

import base64
import csv
from io import StringIO
import json
import os
from pathlib import Path

import pytest
from fastapi import HTTPException

from tests.unit.damage_helpers import damage_row, wide_bdamage_text
from unicorn_backend.config import BackendConfig
from unicorn_backend.damage_contract import (
    BDAMAGE_COLUMN_COUNT,
    BDAMAGE_SCHEMA_VERSION,
)
from unicorn_backend.store import GraphEngineStore


GRAPHENGINE_DIR = Path(__file__).resolve().parents[2]
V2_FIXTURE = (
    GRAPHENGINE_DIR
    / "tests"
    / "fixtures"
    / "datasets"
    / "example_v2.bdamage.txt"
)
STALE_V1_FIXTURE = (
    GRAPHENGINE_DIR
    / "tests"
    / "fixtures"
    / "datasets"
    / "invalid"
    / "stale_v1.bdamage.txt"
)
MALFORMED_FIXTURE_DIR = STALE_V1_FIXTURE.parent


def _store(tmp_path: Path) -> GraphEngineStore:
    return GraphEngineStore(
        config=BackendConfig(
            host="127.0.0.1",
            port=8000,
            runtime_dir=tmp_path,
            upload_dir=tmp_path,
            agent_trace_dir=tmp_path / "logs" / "agent_trace",
            nodes_filename="nodes.dmp",
            names_filename="names.dmp",
            metadata_filename="metadata.txt",
        )
    )


def _load_text(tmp_path: Path, text: str):
    path = tmp_path / "sample.bdamage.txt"
    path.write_text(text, encoding="utf-8")
    return _store(tmp_path).get_or_load_dataset(path)


def _assert_parse_error(
    tmp_path: Path,
    text: str,
    code: str,
) -> dict:
    with pytest.raises(HTTPException) as caught:
        _load_text(tmp_path, text)
    assert caught.value.status_code == 400
    assert caught.value.detail["code"] == code
    return caught.value.detail


def _reorder_columns(text: str) -> str:
    rows = list(csv.reader(StringIO(text), delimiter="\t"))
    order = [3, 0, 1, 2, *range(4, BDAMAGE_COLUMN_COUNT)]
    output = StringIO(newline="")
    writer = csv.writer(output, delimiter="\t", lineterminator="\n")
    for row in rows:
        writer.writerow([row[index] for index in order])
    return output.getvalue()


def test_wide_damage_file_parses_counts_names_and_profiles(
    tmp_path: Path,
) -> None:
    dataset = _load_text(
        tmp_path,
        wide_bdamage_text([
            damage_row(
                10,
                7,
                "Quoted\tClade",
                subtree_count=23,
                K5_0=1.75,
                N5_0=8.25,
            ),
            damage_row(11, 3, "Species A"),
        ]),
    )

    assert dataset.damage_schema == BDAMAGE_SCHEMA_VERSION
    assert dataset.counts_map == {10: 7, 11: 3}
    assert dataset.names_map[10] == "Quoted\tClade"
    assert dataset.total_reads == 10
    assert dataset.total_taxa == 2
    assert dataset.damage_taxa == 2
    assert dataset.valid_damage_taxa == 2
    assert dataset.invalid_damage_taxa == 0
    profile = dataset.damage_by_taxid[10]
    assert profile.direct_count_scope == "direct"
    assert profile.subtree_count_scope == "subtree"
    assert profile.damage_scope == "subtree"
    assert profile.direct_count == 7
    assert profile.subtree_count == 23
    assert profile.positions[0].k5 == 1.75
    assert profile.positions[0].n5 == 8.25


def test_damage_columns_must_follow_the_frozen_v2_order(
    tmp_path: Path,
) -> None:
    detail = _assert_parse_error(
        tmp_path,
        _reorder_columns(
            wide_bdamage_text([
                damage_row(10, 7, "Clade A"),
            ])
        ),
        "unsupported_bdamage_schema",
    )

    assert "column order" in detail["message"]


def test_nan_is_retained_as_missing_and_serialized_as_null(
    tmp_path: Path,
) -> None:
    dataset = _load_text(
        tmp_path,
        wide_bdamage_text([
            damage_row(
                10,
                7,
                "Clade A",
                A="NaN",
                Dx5_0="nan",
            ),
        ]),
    )
    profile = dataset.damage_by_taxid[10]
    payload = profile.to_payload()

    assert profile.fit_valid is False
    assert profile.missing_fields == ("A", "Dx5_0")
    assert payload["A"] is None
    assert payload["positions"][0]["dx5"] is None
    json.dumps(payload, allow_nan=False)


def test_zero_or_missing_evidence_invalidates_fit(
    tmp_path: Path,
) -> None:
    overrides = {}
    for position in range(5):
        overrides[f"N5_{position}"] = 0
        overrides[f"N3_{position}"] = "nan"
    dataset = _load_text(
        tmp_path,
        wide_bdamage_text([
            damage_row(10, 7, "Clade A", **overrides),
        ]),
    )

    assert dataset.damage_by_taxid[10].fit_valid is False
    assert dataset.valid_damage_taxa == 0
    assert dataset.invalid_damage_taxa == 1


def test_missing_zfit_diagnostic_does_not_invalidate_usable_fit(
    tmp_path: Path,
) -> None:
    dataset = _load_text(
        tmp_path,
        wide_bdamage_text([
            damage_row(10, 7, "Clade A", Zfit="nan"),
        ]),
    )
    profile = dataset.damage_by_taxid[10]

    assert profile.fit_valid is True
    assert profile.missing_fields == ("Zfit",)
    assert profile.to_payload()["zfit"] is None


def test_missing_required_column_is_rejected(tmp_path: Path) -> None:
    text = wide_bdamage_text([damage_row(10, 7, "Clade A")])
    text = text.replace("\tGAfreq", "", 1)

    detail = _assert_parse_error(
        tmp_path,
        text,
        "missing_bdamage_columns",
    )
    assert detail["missing_columns"] == ["GAfreq"]


def test_changed_or_duplicate_header_is_rejected(tmp_path: Path) -> None:
    text = wide_bdamage_text([damage_row(10, 7, "Clade A")])
    plain_taxid = text.replace("#taxid", "taxid", 1)
    duplicate_header = text.replace("GAfreq", "CTfreq", 1)
    unknown_header = text.replace("GAfreq", "future_metric", 1)

    _assert_parse_error(
        tmp_path,
        plain_taxid,
        "unsupported_bdamage_schema",
    )
    duplicate_detail = _assert_parse_error(
        tmp_path,
        duplicate_header,
        "unsupported_bdamage_schema",
    )
    assert duplicate_detail["duplicate_columns"] == ["CTfreq"]
    unknown_detail = _assert_parse_error(
        tmp_path,
        unknown_header,
        "unsupported_bdamage_schema",
    )
    assert unknown_detail["unknown_columns"] == ["future_metric"]


@pytest.mark.parametrize(
    ("overrides", "column"),
    [
        ({"direct_count": "3.5"}, "direct_count"),
        ({"subtree_count": "3.5"}, "subtree_count"),
        ({"mmm_positions": "3.5"}, "mmm_positions"),
        ({"mmm_positions": 256}, "mmm_positions"),
        ({"A": "not-a-number"}, "A"),
        ({"nll": "inf"}, "nll"),
    ],
)
def test_invalid_numeric_values_are_rejected(
    tmp_path: Path,
    overrides: dict,
    column: str,
) -> None:
    row = damage_row(10, 7, "Clade A")
    row.update(overrides)
    detail = _assert_parse_error(
        tmp_path,
        wide_bdamage_text([row]),
        "invalid_bdamage_value",
    )
    assert detail["column"] == column


def test_duplicate_taxid_is_rejected(tmp_path: Path) -> None:
    detail = _assert_parse_error(
        tmp_path,
        wide_bdamage_text([
            damage_row(10, 7, "Clade A"),
            damage_row(10, 2, "Clade A duplicate"),
        ]),
        "duplicate_bdamage_taxid",
    )
    assert detail["taxid"] == 10


def test_pre_v2_file_is_rejected_with_regeneration_guidance(
    tmp_path: Path,
) -> None:
    detail = _assert_parse_error(
        tmp_path,
        '#taxid\tcount\tname\n10\t7\t"Clade A"\n',
        "unsupported_bdamage_schema",
    )
    assert "Regenerate" in detail["message"]


def test_subtree_count_must_include_the_direct_count(tmp_path: Path) -> None:
    detail = _assert_parse_error(
        tmp_path,
        wide_bdamage_text([
            damage_row(10, 7, "Clade A", subtree_count=6),
        ]),
        "invalid_bdamage_value",
    )

    assert detail["direct_count"] == 7
    assert detail["subtree_count"] == 6


@pytest.mark.parametrize(
    ("positions", "encoded", "expected_bytes"),
    [
        (0, "AQ==", None),
        (0, " ", None),
        (1, "not base64", None),
        (1, base64.b64encode(b"short").decode("ascii"), 128),
    ],
)
def test_direct_mmm_envelope_is_validated_and_not_retained(
    tmp_path: Path,
    positions: int,
    encoded: str,
    expected_bytes: int | None,
) -> None:
    detail = _assert_parse_error(
        tmp_path,
        wide_bdamage_text([
            damage_row(
                10,
                7,
                "Clade A",
                mmm_positions=positions,
                direct_mmm_base64=encoded,
            ),
        ]),
        "invalid_bdamage_matrix",
    )

    assert detail["column"] == "direct_mmm_base64"
    if expected_bytes is not None:
        assert detail["expected_bytes"] == expected_bytes


def test_valid_direct_mmm_envelope_is_not_stored_in_damage_profile(
    tmp_path: Path,
) -> None:
    dataset = _load_text(
        tmp_path,
        wide_bdamage_text([
            damage_row(
                10,
                7,
                "Clade A",
                subtree_count=19,
                mmm_positions=1,
                direct_mmm_base64=base64.b64encode(
                    bytes(128)
                ).decode("ascii"),
            ),
        ]),
    )

    profile = dataset.damage_by_taxid[10]
    assert dataset.counts_map == {10: 7}
    assert dataset.total_reads == 7
    assert profile.direct_count == 7
    assert profile.subtree_count == 19
    assert "direct_mmm_base64" not in profile.to_payload()


def test_count_totals_remain_direct_count_sums(tmp_path: Path) -> None:
    dataset = _load_text(
        tmp_path,
        wide_bdamage_text([
            damage_row(10, 0, "Clade A"),
            damage_row(11, 5, "Species A"),
            damage_row(12, 8, "Species B"),
        ]),
    )

    assert dataset.total_reads == sum(dataset.counts_map.values()) == 13


def test_damage_change_invalidates_cached_dataset(tmp_path: Path) -> None:
    path = tmp_path / "sample.bdamage.txt"
    path.write_text(
        wide_bdamage_text([
            damage_row(10, 7, "Clade A", A=0.125),
        ]),
        encoding="utf-8",
    )
    store = _store(tmp_path)
    initial = store.get_or_load_dataset(path)
    original_mtime = path.stat().st_mtime

    path.write_text(
        wide_bdamage_text([
            damage_row(10, 7, "Clade A", A=0.250),
        ]),
        encoding="utf-8",
    )
    os.utime(path, (original_mtime + 2, original_mtime + 2))
    refreshed = store.get_or_load_dataset(path)

    assert refreshed is not initial
    assert initial.damage_by_taxid[10].amplitude == 0.125
    assert refreshed.damage_by_taxid[10].amplitude == 0.250
    assert refreshed.counts_map == initial.counts_map


def test_real_v2_fixture_parses_with_direct_count_tree_semantics() -> None:
    store = _store(V2_FIXTURE.parent)
    dataset = store.get_or_load_dataset(V2_FIXTURE)

    assert dataset.damage_schema == BDAMAGE_SCHEMA_VERSION
    assert dataset.counts_map
    assert dataset.total_reads == sum(dataset.counts_map.values())
    assert dataset.damage_by_taxid


def test_stale_v1_fixture_is_rejected_with_regeneration_guidance() -> None:
    store = _store(STALE_V1_FIXTURE.parent)
    with pytest.raises(HTTPException) as caught:
        store.get_or_load_dataset(STALE_V1_FIXTURE)

    assert caught.value.status_code == 400
    assert caught.value.detail["code"] == "unsupported_bdamage_schema"
    assert "Regenerate" in caught.value.detail["message"]


@pytest.mark.parametrize(
    ("filename", "code", "column"),
    [
        (
            "missing_subtree_count.bdamage.txt",
            "missing_bdamage_columns",
            None,
        ),
        (
            "subtree_less_than_direct.bdamage.txt",
            "invalid_bdamage_value",
            None,
        ),
        (
            "invalid_direct_mmm_base64.bdamage.txt",
            "invalid_bdamage_matrix",
            "direct_mmm_base64",
        ),
        (
            "incorrect_direct_mmm_length.bdamage.txt",
            "invalid_bdamage_matrix",
            "direct_mmm_base64",
        ),
        (
            "matrix_data_with_zero_positions.bdamage.txt",
            "invalid_bdamage_matrix",
            "direct_mmm_base64",
        ),
    ],
)
def test_malformed_v2_fixtures_are_rejected_cleanly(
    filename: str,
    code: str,
    column: str | None,
) -> None:
    path = MALFORMED_FIXTURE_DIR / filename
    store = _store(path.parent)
    with pytest.raises(HTTPException) as caught:
        store.get_or_load_dataset(path)

    detail = caught.value.detail
    assert caught.value.status_code == 400
    assert detail["code"] == code
    if column is not None:
        assert detail["column"] == column
