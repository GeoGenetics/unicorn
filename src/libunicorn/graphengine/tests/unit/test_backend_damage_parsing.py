from __future__ import annotations

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
PRODUCER_FIXTURE = (
    GRAPHENGINE_DIR
    / "tests"
    / "fixtures"
    / "datasets"
    / "example43.bdamage.txt"
)


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
    order = [2, 0, 1, *range(3, BDAMAGE_COLUMN_COUNT)]
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
    assert profile.count_scope == "direct"
    assert profile.damage_scope == "subtree"
    assert profile.positions[0].k5 == 1.75
    assert profile.positions[0].n5 == 8.25


def test_damage_columns_are_resolved_by_header_name(
    tmp_path: Path,
) -> None:
    dataset = _load_text(
        tmp_path,
        _reorder_columns(
            wide_bdamage_text([
                damage_row(10, 7, "Clade A", A=0.125),
            ])
        ),
    )

    assert dataset.counts_map == {10: 7}
    assert dataset.names_map == {10: "Clade A"}
    assert dataset.damage_by_taxid[10].amplitude == 0.125


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
        ({"count": "3.5"}, "count"),
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


def test_legacy_three_column_file_is_rejected(tmp_path: Path) -> None:
    detail = _assert_parse_error(
        tmp_path,
        '#taxid\tcount\tname\n10\t7\t"Clade A"\n',
        "unsupported_bdamage_schema",
    )
    assert "Regenerate" in detail["message"]


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


def test_real_43_column_producer_fixture_loads_without_loss() -> None:
    store = _store(PRODUCER_FIXTURE.parent)
    dataset = store.get_or_load_dataset(PRODUCER_FIXTURE)

    assert dataset.damage_schema == BDAMAGE_SCHEMA_VERSION
    assert dataset.total_taxa == 16080
    assert dataset.damage_taxa == dataset.total_taxa
    assert dataset.total_reads == sum(dataset.counts_map.values())

    mapped = dataset.damage_by_taxid[28727]
    assert dataset.counts_map[28727] == 12
    assert dataset.names_map[28727] == "Cyanocitta cristata"
    assert mapped.amplitude == pytest.approx(0.074218735)
    assert mapped.positions[0].n3 == 5

    missing_zfit = dataset.damage_by_taxid[2283408]
    assert missing_zfit.missing_fields == ("Zfit",)
    assert missing_zfit.fit_valid is True
    json.dumps(missing_zfit.to_payload(), allow_nan=False)
