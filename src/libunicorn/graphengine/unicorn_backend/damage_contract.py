"""Frozen input contract for Unicorn wide bdamage datasets."""

from __future__ import annotations


BDAMAGE_SCHEMA_VERSION = "unicorn_bdamage_v1"
BDAMAGE_FILE_SUFFIX = ".bdamage.txt"
BDAMAGE_POSITION_COUNT = 5

BDAMAGE_COUNT_SCOPE = "direct"
BDAMAGE_DAMAGE_SCOPE = "subtree"

BDAMAGE_EXTERNAL_ID_COLUMN = "#taxid"
BDAMAGE_INTERNAL_ID_COLUMN = "taxid"

BDAMAGE_IDENTITY_COLUMNS = (
    BDAMAGE_INTERNAL_ID_COLUMN,
    "count",
    "name",
)

BDAMAGE_FIT_COLUMNS = (
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

BDAMAGE_POSITION_METRICS = (
    "K5",
    "N5",
    "K3",
    "N3",
    "Dx5",
    "Dx3",
)

BDAMAGE_POSITION_COLUMNS = tuple(
    f"{metric}_{position}"
    for position in range(BDAMAGE_POSITION_COUNT)
    for metric in BDAMAGE_POSITION_METRICS
)

BDAMAGE_COLUMNS = (
    *BDAMAGE_IDENTITY_COLUMNS,
    *BDAMAGE_FIT_COLUMNS,
    *BDAMAGE_POSITION_COLUMNS,
)

BDAMAGE_HEADER_COLUMNS = (
    BDAMAGE_EXTERNAL_ID_COLUMN,
    *BDAMAGE_COLUMNS[1:],
)

BDAMAGE_INTEGER_COLUMNS = (
    BDAMAGE_INTERNAL_ID_COLUMN,
    "count",
)

BDAMAGE_FLOAT_COLUMNS = (
    *BDAMAGE_FIT_COLUMNS,
    *BDAMAGE_POSITION_COLUMNS,
)

BDAMAGE_COLUMN_COUNT = len(BDAMAGE_COLUMNS)
BDAMAGE_HEADER = "\t".join(BDAMAGE_HEADER_COLUMNS)

