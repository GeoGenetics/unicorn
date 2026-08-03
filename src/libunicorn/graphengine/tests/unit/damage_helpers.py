from __future__ import annotations

import csv
from io import StringIO
from typing import Any, Iterable

from unicorn_backend.damage_contract import (
    BDAMAGE_COLUMNS,
    BDAMAGE_HEADER_COLUMNS,
    BDAMAGE_POSITION_COUNT,
)


def damage_row(
    taxid: int,
    direct_count: int,
    name: str,
    subtree_count: int | None = None,
    **overrides: Any,
) -> dict[str, Any]:
    row: dict[str, Any] = {
        "taxid": taxid,
        "direct_count": direct_count,
        "subtree_count": (
            direct_count if subtree_count is None else subtree_count
        ),
        "name": name,
        "CTfreq": 0.10,
        "GAfreq": 0.08,
        "A": 0.09,
        "q": 0.20,
        "c": 0.01,
        "phi": 100.0,
        "Zfit": 3.0,
        "fitCT0": 0.10,
        "fitGA0": 0.09,
        "nll": 10.0,
        "mmm_positions": 0,
        "direct_mmm_base64": "",
    }
    for position in range(BDAMAGE_POSITION_COUNT):
        row.update(
            {
                f"K5_{position}": 1.5 + position,
                f"N5_{position}": 10.5 + position,
                f"K3_{position}": 1.25 + position,
                f"N3_{position}": 9.5 + position,
                f"Dx5_{position}": 0.10 / (position + 1),
                f"Dx3_{position}": 0.09 / (position + 1),
            }
        )
    row.update(overrides)
    return row


def wide_bdamage_text(rows: Iterable[dict[str, Any]]) -> str:
    output = StringIO(newline="")
    writer = csv.writer(
        output,
        delimiter="\t",
        quotechar='"',
        lineterminator="\n",
    )
    writer.writerow(BDAMAGE_HEADER_COLUMNS)
    for row in rows:
        writer.writerow([row[column] for column in BDAMAGE_COLUMNS])
    return output.getvalue()
