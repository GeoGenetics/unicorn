from __future__ import annotations

from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple

import numpy as np


VALID_COUNT_MODES = {"direct", "subtree"}
VALID_CORRECTION_METHODS = {"none", "auto", "lingoes", "cailliez"}
DISTANCE_METRIC_ALIASES = {
    "jaccard": "jaccard",
    "bray_curtis": "bray_curtis",
    "bray-curtis": "bray_curtis",
    "log_chord": "log_chord",
    "log-chord": "log_chord",
    "log_cord": "log_chord",
    "log-cord": "log_chord",
    "chao": "chao",
}
IMPLEMENTED_DISTANCE_METRICS = {"jaccard", "bray_curtis", "log_chord", "chao"}


def _normalize_count_mode(count_mode: Any) -> str:
    normalized = str(count_mode or "").strip().lower()
    if normalized not in VALID_COUNT_MODES:
        raise ValueError("count_mode must be either 'direct' or 'subtree'.")
    return normalized


def _normalize_distance_metric(distance_metric: Any) -> str:
    normalized = str(distance_metric or "").strip().lower()
    canonical = DISTANCE_METRIC_ALIASES.get(normalized)
    if canonical is None:
        raise ValueError(
            "distance_metric must be one of 'jaccard', 'bray_curtis', 'log_chord', or 'chao'."
        )
    return canonical


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


def _normalize_correction_method(correction_method: Any) -> str:
    normalized = str(correction_method or "auto").strip().lower()
    if normalized not in VALID_CORRECTION_METHODS:
        raise ValueError("correction_method must be one of 'none', 'auto', 'lingoes', or 'cailliez'.")
    return normalized


def _node_label(row: Mapping[str, Any]) -> str:
    name = str(row.get("name") or row.get("taxid") or "node")
    taxid = row.get("taxid")
    if taxid is None:
        return name
    return f"{name} ({taxid})"


def _coerce_nonnegative_float(value: Any, *, context: str) -> float:
    try:
        numeric = float(value or 0.0)
    except (TypeError, ValueError):
        raise ValueError(f"{context} could not be interpreted as a number.")
    if not np.isfinite(numeric):
        raise ValueError(f"{context} must be finite.")
    if numeric < 0:
        raise ValueError(f"{context} must be non-negative.")
    return numeric


def _extract_feature_matrix(
    report: Mapping[str, Any],
    *,
    count_mode: str,
) -> Tuple[List[str], List[str], np.ndarray, Mapping[str, Any]]:
    if not isinstance(report, Mapping):
        raise ValueError("report must be an object.")
    summary = report.get("summary")
    matrix = report.get("matrix")
    if not isinstance(summary, Mapping):
        raise ValueError("report.summary must be an object.")
    if not isinstance(matrix, Mapping):
        raise ValueError("report.matrix must be an object.")

    rows = matrix.get("rows")
    dataset_names = matrix.get("dataset_names")
    if not isinstance(rows, list) or not rows:
        raise ValueError("report.matrix.rows must be a non-empty array.")
    if not isinstance(dataset_names, list) or not dataset_names:
        raise ValueError("report.matrix.dataset_names must be a non-empty array.")

    normalized_dataset_names: List[str] = []
    for dataset_name_value in dataset_names:
        dataset_name = str(dataset_name_value or "").strip()
        if not dataset_name:
            raise ValueError("report.matrix.dataset_names cannot contain empty values.")
        normalized_dataset_names.append(dataset_name)

    feature_labels: List[str] = []
    values = np.zeros((len(normalized_dataset_names), len(rows)), dtype=float)
    for row_index, row in enumerate(rows):
        if not isinstance(row, Mapping):
            raise ValueError("report.matrix.rows entries must be objects.")
        datasets = row.get("datasets")
        if not isinstance(datasets, list):
            raise ValueError("report.matrix.rows[*].datasets must be arrays.")
        feature_labels.append(_node_label(row))
        for dataset_index, dataset_name in enumerate(normalized_dataset_names):
            if dataset_index >= len(datasets) or not isinstance(datasets[dataset_index], Mapping):
                entry: Mapping[str, Any] = {}
            else:
                entry = datasets[dataset_index]
            value = _coerce_nonnegative_float(
                entry.get(count_mode),
                context=f"Dataset value for '{dataset_name}' and feature '{feature_labels[-1]}'",
            )
            values[dataset_index, row_index] = value

    if values.shape[0] < 2:
        raise ValueError("PCoA requires at least two datasets.")
    if values.shape[1] < 1:
        raise ValueError("PCoA requires at least one selected node.")

    return normalized_dataset_names, feature_labels, values, summary


def _compute_jaccard_distance_matrix(feature_matrix: np.ndarray) -> np.ndarray:
    present = feature_matrix > 0
    n_samples = present.shape[0]
    distances = np.zeros((n_samples, n_samples), dtype=float)
    for i in range(n_samples):
        for j in range(i + 1, n_samples):
            intersection = float(np.logical_and(present[i], present[j]).sum())
            union = float(np.logical_or(present[i], present[j]).sum())
            distance = 0.0 if union == 0.0 else 1.0 - (intersection / union)
            distances[i, j] = distance
            distances[j, i] = distance
    return distances


def _compute_bray_curtis_distance_matrix(feature_matrix: np.ndarray) -> np.ndarray:
    n_samples = feature_matrix.shape[0]
    distances = np.zeros((n_samples, n_samples), dtype=float)
    for i in range(n_samples):
        for j in range(i + 1, n_samples):
            numerator = float(np.abs(feature_matrix[i] - feature_matrix[j]).sum())
            denominator = float((feature_matrix[i] + feature_matrix[j]).sum())
            distance = 0.0 if denominator == 0.0 else numerator / denominator
            distances[i, j] = distance
            distances[j, i] = distance
    return distances


def _compute_log_chord_distance_matrix(feature_matrix: np.ndarray) -> np.ndarray:
    logged = np.log1p(feature_matrix)
    norms = np.linalg.norm(logged, axis=1)
    normalized = np.zeros_like(logged, dtype=float)
    nonzero = norms > 0
    if np.any(nonzero):
        normalized[nonzero] = logged[nonzero] / norms[nonzero, np.newaxis]

    n_samples = normalized.shape[0]
    distances = np.zeros((n_samples, n_samples), dtype=float)
    for i in range(n_samples):
        for j in range(i + 1, n_samples):
            distance = float(np.linalg.norm(normalized[i] - normalized[j]))
            distances[i, j] = distance
            distances[j, i] = distance
    return distances


def _chao_bias_ratio(singletons: int, doubletons: int) -> float:
    if singletons <= 0:
        return 0.0
    if doubletons > 0:
        return float(singletons) / (2.0 * float(doubletons))
    return float(singletons * max(singletons - 1, 0)) / 2.0


def _chao_jaccard_similarity(x: np.ndarray, y: np.ndarray) -> float:
    n = float(np.sum(x))
    m = float(np.sum(y))
    if n <= 0.0 and m <= 0.0:
        return 1.0
    if n <= 0.0 or m <= 0.0:
        return 0.0

    shared = (x > 0) & (y > 0)
    if not np.any(shared):
        return 0.0

    u_obs = float(np.sum(x[shared]) / n)
    v_obs = float(np.sum(y[shared]) / m)

    f1_plus = int(np.sum((x == 1) & (y > 0)))
    f2_plus = int(np.sum((x == 2) & (y > 0)))
    f_plus1 = int(np.sum((x > 0) & (y == 1)))
    f_plus2 = int(np.sum((x > 0) & (y == 2)))

    u_hat = u_obs + ((m - 1.0) / m) * _chao_bias_ratio(f1_plus, f2_plus) * (1.0 - v_obs)
    v_hat = v_obs + ((n - 1.0) / n) * _chao_bias_ratio(f_plus1, f_plus2) * (1.0 - u_obs)

    u_hat = min(max(u_hat, 0.0), 1.0)
    v_hat = min(max(v_hat, 0.0), 1.0)

    denominator = u_hat + v_hat - (u_hat * v_hat)
    if denominator <= 0.0:
        return 0.0
    return (u_hat * v_hat) / denominator


def _compute_chao_distance_matrix(feature_matrix: np.ndarray) -> np.ndarray:
    n_samples = feature_matrix.shape[0]
    distances = np.zeros((n_samples, n_samples), dtype=float)
    for i in range(n_samples):
        for j in range(i + 1, n_samples):
            similarity = _chao_jaccard_similarity(feature_matrix[i], feature_matrix[j])
            distance = 1.0 - similarity
            distances[i, j] = distance
            distances[j, i] = distance
    return distances


def _gram_from_distance_matrix(distance_matrix: np.ndarray) -> np.ndarray:
    n_samples = distance_matrix.shape[0]
    squared = distance_matrix ** 2
    centering = np.eye(n_samples) - np.full((n_samples, n_samples), 1.0 / n_samples)
    return -0.5 * centering @ squared @ centering


def _ordered_eigendecomposition(gram: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
    eigenvalues, eigenvectors = np.linalg.eigh(gram)
    order = np.argsort(eigenvalues)[::-1]
    return eigenvalues[order], eigenvectors[:, order]


def _eigen_tolerance(eigenvalues: np.ndarray) -> float:
    scale = max(1.0, float(np.max(np.abs(eigenvalues))) if eigenvalues.size else 0.0)
    return 1e-12 * scale


def _distance_matrix_min_eigenvalue(distance_matrix: np.ndarray) -> float:
    gram = _gram_from_distance_matrix(distance_matrix)
    eigenvalues, _ = _ordered_eigendecomposition(gram)
    return float(eigenvalues[-1]) if eigenvalues.size else 0.0


def _apply_lingoes_correction(distance_matrix: np.ndarray) -> Tuple[np.ndarray, float]:
    gram = _gram_from_distance_matrix(distance_matrix)
    eigenvalues, _ = _ordered_eigendecomposition(gram)
    if not eigenvalues.size:
        return distance_matrix.copy(), 0.0
    smallest = float(eigenvalues[-1])
    tolerance = _eigen_tolerance(eigenvalues)
    if smallest >= -tolerance:
        return distance_matrix.copy(), 0.0

    constant = -smallest
    corrected_squared = distance_matrix ** 2
    corrected_squared = corrected_squared + (2.0 * constant)
    np.fill_diagonal(corrected_squared, 0.0)
    corrected = np.sqrt(np.maximum(corrected_squared, 0.0))
    return corrected, constant


def _apply_cailliez_correction(distance_matrix: np.ndarray) -> Tuple[np.ndarray, float]:
    gram = _gram_from_distance_matrix(distance_matrix)
    eigenvalues, _ = _ordered_eigendecomposition(gram)
    if not eigenvalues.size:
        return distance_matrix.copy(), 0.0
    tolerance = _eigen_tolerance(eigenvalues)
    if float(eigenvalues[-1]) >= -tolerance:
        return distance_matrix.copy(), 0.0

    lower = 0.0
    upper = max(1.0, float(np.max(distance_matrix)))
    max_iterations = 80
    for _ in range(max_iterations):
        trial = distance_matrix.copy()
        mask = ~np.eye(trial.shape[0], dtype=bool)
        trial[mask] = trial[mask] + upper
        min_eigenvalue = _distance_matrix_min_eigenvalue(trial)
        if min_eigenvalue >= -tolerance:
            break
        upper *= 2.0
    else:
        raise ValueError("Could not bracket a Cailliez correction constant.")

    for _ in range(max_iterations):
        midpoint = (lower + upper) / 2.0
        trial = distance_matrix.copy()
        mask = ~np.eye(trial.shape[0], dtype=bool)
        trial[mask] = trial[mask] + midpoint
        min_eigenvalue = _distance_matrix_min_eigenvalue(trial)
        if min_eigenvalue >= -tolerance:
            upper = midpoint
        else:
            lower = midpoint

    constant = upper
    corrected = distance_matrix.copy()
    mask = ~np.eye(corrected.shape[0], dtype=bool)
    corrected[mask] = corrected[mask] + constant
    return corrected, constant


def apply_pcoa_correction(
    distance_matrix: Sequence[Sequence[float]] | np.ndarray,
    *,
    correction_method: Any,
) -> Dict[str, Any]:
    distances = np.asarray(distance_matrix, dtype=float)
    normalized_method = _normalize_correction_method(correction_method)
    raw_min_eigenvalue = _distance_matrix_min_eigenvalue(distances)
    raw_tolerance = _eigen_tolerance(np.asarray([raw_min_eigenvalue]))
    requires_correction = raw_min_eigenvalue < -raw_tolerance

    if normalized_method == "none":
        return {
            "distance_matrix": distances,
            "correction_required": requires_correction,
            "correction_applied": None,
            "correction_constant": 0.0,
            "raw_min_eigenvalue": raw_min_eigenvalue,
        }
    if normalized_method == "auto":
        if not requires_correction:
            return {
                "distance_matrix": distances,
                "correction_required": False,
                "correction_applied": None,
                "correction_constant": 0.0,
                "raw_min_eigenvalue": raw_min_eigenvalue,
            }
        normalized_method = "lingoes"

    if normalized_method == "lingoes":
        corrected, constant = _apply_lingoes_correction(distances)
    elif normalized_method == "cailliez":
        corrected, constant = _apply_cailliez_correction(distances)
    else:
        raise ValueError("Unsupported correction_method.")

    return {
        "distance_matrix": corrected,
        "correction_required": requires_correction,
        "correction_applied": normalized_method,
        "correction_constant": float(constant),
        "raw_min_eigenvalue": raw_min_eigenvalue,
    }


def compute_distance_matrix(
    feature_matrix: Sequence[Sequence[float]] | np.ndarray,
    *,
    distance_metric: Any,
) -> np.ndarray:
    matrix = np.asarray(feature_matrix, dtype=float)
    if matrix.ndim != 2:
        raise ValueError("feature_matrix must be a 2D array.")
    if matrix.shape[0] < 2:
        raise ValueError("feature_matrix must contain at least two samples.")
    if matrix.shape[1] < 1:
        raise ValueError("feature_matrix must contain at least one feature.")

    normalized_metric = _normalize_distance_metric(distance_metric)
    if normalized_metric == "jaccard":
        return _compute_jaccard_distance_matrix(matrix)
    if normalized_metric == "bray_curtis":
        return _compute_bray_curtis_distance_matrix(matrix)
    if normalized_metric == "log_chord":
        return _compute_log_chord_distance_matrix(matrix)
    if normalized_metric == "chao":
        return _compute_chao_distance_matrix(matrix)
    raise NotImplementedError(f"distance_metric '{normalized_metric}' is not implemented.")


def compute_pcoa(
    distance_matrix: Sequence[Sequence[float]] | np.ndarray,
    *,
    correction_method: Any = "auto",
) -> Dict[str, Any]:
    distances = np.asarray(distance_matrix, dtype=float)
    if distances.ndim != 2 or distances.shape[0] != distances.shape[1]:
        raise ValueError("distance_matrix must be a square 2D array.")
    n_samples = distances.shape[0]
    if n_samples < 2:
        raise ValueError("distance_matrix must contain at least two samples.")
    if not np.all(np.isfinite(distances)):
        raise ValueError("distance_matrix must be finite.")

    distances = (distances + distances.T) / 2.0
    np.fill_diagonal(distances, 0.0)

    corrected = apply_pcoa_correction(
        distances,
        correction_method=correction_method,
    )
    corrected_distances = corrected["distance_matrix"]

    gram = _gram_from_distance_matrix(corrected_distances)
    eigenvalues, eigenvectors = _ordered_eigendecomposition(gram)

    tolerance = _eigen_tolerance(eigenvalues)
    positive_mask = eigenvalues > tolerance
    negative_mask = eigenvalues < -tolerance
    positive_eigenvalues = eigenvalues[positive_mask]
    positive_eigenvectors = eigenvectors[:, positive_mask]

    if positive_eigenvalues.size:
        coordinates = positive_eigenvectors * np.sqrt(positive_eigenvalues)
    else:
        coordinates = np.zeros((n_samples, 0), dtype=float)

    first_two = np.zeros((n_samples, 2), dtype=float)
    for axis_index in range(min(2, coordinates.shape[1])):
        first_two[:, axis_index] = coordinates[:, axis_index]

    explained_sum = float(positive_eigenvalues.sum())
    explained_fractions: List[float] = []
    for axis_index in range(2):
        if axis_index < positive_eigenvalues.size and explained_sum > tolerance:
            explained_fractions.append(float(positive_eigenvalues[axis_index] / explained_sum))
        else:
            explained_fractions.append(0.0)

    return {
        "coordinates": first_two,
        "distance_matrix": corrected_distances,
        "eigenvalues": eigenvalues,
        "explained_fractions": explained_fractions,
        "diagnostics": {
            "negative_eigenvalue_count": int(np.count_nonzero(negative_mask)),
            "min_eigenvalue": float(eigenvalues.min(initial=0.0)),
            "max_eigenvalue": float(eigenvalues.max(initial=0.0)),
            "correction_required": bool(corrected["correction_required"]),
            "correction_applied": corrected["correction_applied"],
            "correction_constant": float(corrected["correction_constant"]),
            "raw_min_eigenvalue": float(corrected["raw_min_eigenvalue"]),
        },
    }


def build_count_matrix_pcoa_spec(
    report: Mapping[str, Any],
    *,
    count_mode: Any,
    distance_metric: Any,
    correction_method: Any = "auto",
    dataset_colors: Optional[Mapping[str, str]] = None,
) -> Dict[str, Any]:
    normalized_mode = _normalize_count_mode(count_mode)
    normalized_metric = _normalize_distance_metric(distance_metric)
    normalized_colors = _normalize_dataset_colors(dataset_colors)

    dataset_names, feature_labels, feature_matrix, summary = _extract_feature_matrix(
        report,
        count_mode=normalized_mode,
    )
    distance_matrix = compute_distance_matrix(
        feature_matrix,
        distance_metric=normalized_metric,
    )
    pcoa = compute_pcoa(
        distance_matrix,
        correction_method=correction_method,
    )
    coordinates = pcoa["coordinates"]
    corrected_distance_matrix = pcoa["distance_matrix"]

    count_label = "direct" if normalized_mode == "direct" else "cumulative"
    metric_label = {
        "jaccard": "Jaccard",
        "bray_curtis": "Bray-Curtis",
        "log_chord": "log-chord",
        "chao": "Chao",
    }[normalized_metric]

    points: List[Dict[str, Any]] = []
    for index, dataset_name in enumerate(dataset_names):
        points.append(
            {
                "dataset": dataset_name,
                "label": dataset_name,
                "color": normalized_colors.get(dataset_name) or None,
                "x": float(coordinates[index, 0]),
                "y": float(coordinates[index, 1]),
            }
        )

    return {
        "compute_version": "v1",
        "plot_type": "pcoa",
        "count_mode": normalized_mode,
        "distance_metric": normalized_metric,
        "title": f"{metric_label} PCoA of selected-node {count_label} counts",
        "axes": [
            {
                "id": "PC1",
                "explained_fraction": float(pcoa["explained_fractions"][0]),
            },
            {
                "id": "PC2",
                "explained_fraction": float(pcoa["explained_fractions"][1]),
            },
        ],
        "points": points,
        "distance_matrix": {
            "labels": dataset_names,
            "values": corrected_distance_matrix.tolist(),
        },
        "feature_labels": feature_labels,
        "summary": {
            "selected_node_count": int(summary.get("selected_node_count") or len(feature_labels)),
            "dataset_count": len(dataset_names),
            "feature_count": len(feature_labels),
        },
        "diagnostics": pcoa["diagnostics"],
        "correction_method": _normalize_correction_method(correction_method),
        "eigenvalues": [float(value) for value in pcoa["eigenvalues"].tolist()],
    }
