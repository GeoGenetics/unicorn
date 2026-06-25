from __future__ import annotations

from typing import Any, Dict, List, Mapping, Optional


VALID_COUNT_MODES = {"direct", "subtree"}


def _normalize_count_mode(count_mode: Any) -> str:
    normalized = str(count_mode or "").strip().lower()
    if normalized not in VALID_COUNT_MODES:
        raise ValueError("count_mode must be either 'direct' or 'subtree'.")
    return normalized


def _normalize_dataset_colors(dataset_colors: Any) -> Dict[str, str]:
    if not isinstance(dataset_colors, Mapping):
        return {}
    normalized: Dict[str, str] = {}
    for key, value in dataset_colors.items():
        name = str(key or "").strip()
        color = str(value or "").strip()
        if not name:
            continue
        normalized[name] = color or ""
    return normalized


def _node_label(row: Mapping[str, Any]) -> str:
    name = str(row.get("name") or row.get("taxid") or "node")
    taxid = row.get("taxid")
    if taxid is None:
        return name
    return f"{name} ({taxid})"


def build_count_matrix_barplot_spec(
    report: Mapping[str, Any],
    *,
    count_mode: Any,
    dataset_colors: Optional[Mapping[str, str]] = None,
) -> Dict[str, Any]:
    if not isinstance(report, Mapping):
        raise ValueError("report must be an object.")
    summary = report.get("summary")
    matrix = report.get("matrix")
    if not isinstance(summary, Mapping):
        raise ValueError("report.summary must be an object.")
    if not isinstance(matrix, Mapping):
        raise ValueError("report.matrix must be an object.")

    normalized_mode = _normalize_count_mode(count_mode)
    normalized_colors = _normalize_dataset_colors(dataset_colors)

    rows = matrix.get("rows")
    dataset_names = matrix.get("dataset_names")
    if not isinstance(rows, list) or not rows:
        raise ValueError("report.matrix.rows must be a non-empty array.")
    if not isinstance(dataset_names, list) or not dataset_names:
        raise ValueError("report.matrix.dataset_names must be a non-empty array.")

    x = [_node_label(row) if isinstance(row, Mapping) else "node" for row in rows]
    traces: List[Dict[str, Any]] = []
    for dataset_index, dataset_name_value in enumerate(dataset_names):
        dataset_name = str(dataset_name_value or "").strip()
        if not dataset_name:
            raise ValueError("report.matrix.dataset_names cannot contain empty values.")
        y: List[int] = []
        for row in rows:
            if not isinstance(row, Mapping):
                raise ValueError("report.matrix.rows entries must be objects.")
            datasets = row.get("datasets")
            if not isinstance(datasets, list):
                raise ValueError("report.matrix.rows[*].datasets must be arrays.")
            entry = datasets[dataset_index] if dataset_index < len(datasets) and isinstance(datasets[dataset_index], Mapping) else {}
            value = entry.get(normalized_mode) if isinstance(entry, Mapping) else 0
            try:
                y.append(int(value or 0))
            except (TypeError, ValueError):
                raise ValueError(f"Dataset value for '{dataset_name}' could not be interpreted as an integer.")
        traces.append(
            {
                "name": dataset_name,
                "color": normalized_colors.get(dataset_name) or None,
                "y": y,
            }
        )

    count_label = "direct" if normalized_mode == "direct" else "cumulative"
    return {
        "compute_version": "v1",
        "plot_type": "barplot",
        "count_mode": normalized_mode,
        "title": f"Selected-node {count_label} counts",
        "x": x,
        "traces": traces,
        "summary": {
            "selected_node_count": int(summary.get("selected_node_count") or len(rows)),
            "dataset_count": len(dataset_names),
        },
    }

