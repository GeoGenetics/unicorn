from __future__ import annotations

from unicorn_backend.damage_contract import (
    BDAMAGE_BINARY_COLUMNS,
    BDAMAGE_COLUMNS,
    BDAMAGE_COLUMN_COUNT,
    BDAMAGE_DAMAGE_SCOPE,
    BDAMAGE_DIRECT_COUNT_COLUMN,
    BDAMAGE_DIRECT_COUNT_SCOPE,
    BDAMAGE_DIRECT_MMM_COLUMN,
    BDAMAGE_EXTERNAL_ID_COLUMN,
    BDAMAGE_FILE_SUFFIX,
    BDAMAGE_FIT_COLUMNS,
    BDAMAGE_FLOAT_COLUMNS,
    BDAMAGE_HEADER,
    BDAMAGE_HEADER_COLUMNS,
    BDAMAGE_IDENTITY_COLUMNS,
    BDAMAGE_INTEGER_COLUMNS,
    BDAMAGE_INTERNAL_ID_COLUMN,
    BDAMAGE_MAX_MMM_POSITIONS,
    BDAMAGE_MMM_BYTES_PER_POSITION,
    BDAMAGE_MMM_POSITIONS_COLUMN,
    BDAMAGE_MMM_VALUES_PER_POSITION,
    BDAMAGE_POSITION_COLUMNS,
    BDAMAGE_POSITION_COUNT,
    BDAMAGE_POSITION_METRICS,
    BDAMAGE_SCHEMA_VERSION,
    BDAMAGE_SUBTREE_COUNT_COLUMN,
    BDAMAGE_SUBTREE_COUNT_SCOPE,
    BDAMAGE_TEXT_COLUMNS,
)


def test_wide_bdamage_contract_identity_and_scopes_are_frozen() -> None:
    assert BDAMAGE_SCHEMA_VERSION == "unicorn_bdamage_v2"
    assert BDAMAGE_FILE_SUFFIX == ".bdamage.txt"
    assert BDAMAGE_DIRECT_COUNT_SCOPE == "direct"
    assert BDAMAGE_SUBTREE_COUNT_SCOPE == "subtree"
    assert BDAMAGE_DAMAGE_SCOPE == "subtree"
    assert BDAMAGE_POSITION_COUNT == 5


def test_wide_bdamage_contract_has_exact_unique_46_column_header() -> None:
    assert BDAMAGE_COLUMN_COUNT == 46
    assert len(BDAMAGE_COLUMNS) == 46
    assert len(BDAMAGE_HEADER_COLUMNS) == 46
    assert len(set(BDAMAGE_COLUMNS)) == 46
    assert len(set(BDAMAGE_HEADER_COLUMNS)) == 46
    assert BDAMAGE_HEADER_COLUMNS[0] == BDAMAGE_EXTERNAL_ID_COLUMN
    assert BDAMAGE_COLUMNS[0] == BDAMAGE_INTERNAL_ID_COLUMN
    assert BDAMAGE_HEADER.split("\t") == list(BDAMAGE_HEADER_COLUMNS)


def test_wide_bdamage_fixed_columns_are_frozen() -> None:
    assert BDAMAGE_IDENTITY_COLUMNS == (
        "taxid",
        "direct_count",
        "subtree_count",
        "name",
    )
    assert BDAMAGE_FIT_COLUMNS == (
        "CTfreq",
        "GAfreq",
        "A",
        "q",
        "c",
        "phi",
        "Zfit",
        "fitCT0",
        "fitGA0",
        "nll",
    )
    assert BDAMAGE_INTEGER_COLUMNS == (
        "taxid",
        "direct_count",
        "subtree_count",
        "mmm_positions",
    )


def test_wide_bdamage_position_columns_follow_producer_order() -> None:
    assert BDAMAGE_POSITION_METRICS == (
        "K5",
        "N5",
        "K3",
        "N3",
        "Dx5",
        "Dx3",
    )
    assert len(BDAMAGE_POSITION_COLUMNS) == 30
    assert BDAMAGE_POSITION_COLUMNS[:6] == (
        "K5_0",
        "N5_0",
        "K3_0",
        "N3_0",
        "Dx5_0",
        "Dx3_0",
    )
    assert BDAMAGE_POSITION_COLUMNS[-6:] == (
        "K5_4",
        "N5_4",
        "K3_4",
        "N3_4",
        "Dx5_4",
        "Dx3_4",
    )


def test_wide_bdamage_numeric_field_partition_is_complete() -> None:
    assert set(BDAMAGE_INTEGER_COLUMNS).isdisjoint(BDAMAGE_FLOAT_COLUMNS)
    assert set(BDAMAGE_INTEGER_COLUMNS).isdisjoint(BDAMAGE_TEXT_COLUMNS)
    assert set(BDAMAGE_FLOAT_COLUMNS).isdisjoint(BDAMAGE_TEXT_COLUMNS)
    assert set(BDAMAGE_INTEGER_COLUMNS) | set(BDAMAGE_FLOAT_COLUMNS) | set(
        BDAMAGE_TEXT_COLUMNS
    ) == set(BDAMAGE_COLUMNS)


def test_wide_bdamage_raw_matrix_contract_is_frozen() -> None:
    assert BDAMAGE_DIRECT_COUNT_COLUMN == "direct_count"
    assert BDAMAGE_SUBTREE_COUNT_COLUMN == "subtree_count"
    assert BDAMAGE_MMM_POSITIONS_COLUMN == "mmm_positions"
    assert BDAMAGE_DIRECT_MMM_COLUMN == "direct_mmm_base64"
    assert BDAMAGE_BINARY_COLUMNS == ("direct_mmm_base64",)
    assert BDAMAGE_MMM_VALUES_PER_POSITION == 32
    assert BDAMAGE_MMM_BYTES_PER_POSITION == 128
    assert BDAMAGE_MAX_MMM_POSITIONS == 255
