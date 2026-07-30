"""Damage-profile services for Unicorn Graph Engine."""

from __future__ import annotations

import math
from typing import Any, Dict, List, Optional

from unicorn_backend.damage_contract import (
    BDAMAGE_COUNT_SCOPE,
    BDAMAGE_DAMAGE_SCOPE,
)
from unicorn_backend.models import (
    DamageProfile,
    DatasetModel,
    SelectionModel,
    TaxonomyModel,
)
from unicorn_backend.store import GraphEngineStore
from unicorn_backend.tree import (
    require_node_present,
    resolve_selection_and_tree,
)

MAX_SELECTED_DAMAGE_PROFILES = 5000


class DamageServiceError(Exception):
    """A damage-domain failure awaiting HTTP mapping at the route boundary."""

    def __init__(self, status_code: int, detail: Any) -> None:
        super().__init__(
            detail.get("message")
            if isinstance(detail, dict)
            else str(detail)
        )
        self.status_code = status_code
        self.detail = detail


def damage_node_payload(
    store: GraphEngineStore,
    *,
    taxid: int,
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
) -> Dict[str, Any]:
    selection, taxonomy, tree = resolve_selection_and_tree(
        store,
        files,
        nodes_file,
        names_file,
    )
    request_context = _request_context(selection, taxonomy, taxid)
    node = require_node_present(tree, taxid, request_context)

    return _node_payload(
        selection,
        node.taxid,
        node.name,
        request_context=request_context,
    )


def damage_selected_payload(
    store: GraphEngineStore,
    *,
    taxids: List[int],
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
) -> Dict[str, Any]:
    selection, taxonomy, tree = resolve_selection_and_tree(
        store,
        files,
        nodes_file,
        names_file,
    )
    profile_count = len(taxids) * len(selection.datasets)
    if profile_count > MAX_SELECTED_DAMAGE_PROFILES:
        raise DamageServiceError(
            422,
            {
                "message": (
                    "Selected damage table is too large. Narrow the selected "
                    "taxids or datasets."
                ),
                "code": "damage_selection_too_large",
                "selected_taxids": len(taxids),
                "selected_datasets": len(selection.datasets),
                "requested_profiles": profile_count,
                "maximum_profiles": MAX_SELECTED_DAMAGE_PROFILES,
            },
        )
    request_context = _selected_request_context(
        selection,
        taxonomy,
        taxids,
    )
    nodes = [
        require_node_present(tree, taxid, request_context)
        for taxid in taxids
    ]
    return {
        "ok": True,
        "count_scope": BDAMAGE_COUNT_SCOPE,
        "damage_scope": BDAMAGE_DAMAGE_SCOPE,
        "nodes": [
            _node_payload(
                selection,
                node.taxid,
                node.name,
            )
            for node in nodes
        ],
        "request_context": request_context,
    }


def _node_payload(
    selection: SelectionModel,
    taxid: int,
    name: str,
    *,
    request_context: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    payload = {
        "taxid": taxid,
        "name": name,
        "count_scope": BDAMAGE_COUNT_SCOPE,
        "damage_scope": BDAMAGE_DAMAGE_SCOPE,
        "datasets": [
            _dataset_profile_payload(dataset, taxid)
            for dataset in selection.datasets
        ],
    }
    if request_context is not None:
        payload["ok"] = True
        payload["request_context"] = request_context
    return payload


def _request_context(
    selection: SelectionModel,
    taxonomy: TaxonomyModel,
    taxid: int,
) -> Dict[str, Any]:
    return {
        "dataset_names": list(selection.dataset_names),
        "nodes_file": taxonomy.nodes_fileinfo.name,
        "names_file": (
            taxonomy.names_fileinfo.name
            if taxonomy.names_fileinfo
            else None
        ),
        "taxid": taxid,
    }


def _selected_request_context(
    selection: SelectionModel,
    taxonomy: TaxonomyModel,
    taxids: List[int],
) -> Dict[str, Any]:
    return {
        "dataset_names": list(selection.dataset_names),
        "nodes_file": taxonomy.nodes_fileinfo.name,
        "names_file": (
            taxonomy.names_fileinfo.name
            if taxonomy.names_fileinfo
            else None
        ),
        "taxids": list(taxids),
    }


def _dataset_profile_payload(
    dataset: DatasetModel,
    taxid: int,
) -> Dict[str, Any]:
    profile = dataset.damage_by_taxid.get(taxid)
    base = {
        "dataset": dataset.fileinfo.name,
        "direct_count": dataset.counts_map.get(taxid, 0),
        "profile_present": profile is not None,
    }
    if profile is None:
        base.update(
            {
                "profile_status": "missing",
                "fit_valid": None,
                "missing_fields": [],
                "observed": None,
                "fit": None,
            }
        )
        return base

    base.update(
        {
            "profile_status": (
                "valid" if profile.fit_valid else "invalid"
            ),
            "fit_valid": profile.fit_valid,
            "missing_fields": list(profile.missing_fields),
            "observed": _observed_payload(profile),
            "fit": _fit_payload(profile),
        }
    )
    return base


def _observed_payload(profile: DamageProfile) -> Dict[str, Any]:
    return {
        "ct_frequency": _safe_float(profile.ct_frequency),
        "ga_frequency": _safe_float(profile.ga_frequency),
        "positions": [
            {
                "position": position.position,
                "k5": _safe_float(position.k5),
                "n5": _safe_float(position.n5),
                "ct_frequency": _safe_ratio(
                    position.k5,
                    position.n5,
                ),
                "k3": _safe_float(position.k3),
                "n3": _safe_float(position.n3),
                "ga_frequency": _safe_ratio(
                    position.k3,
                    position.n3,
                ),
            }
            for position in profile.positions
        ],
    }


def _fit_payload(profile: DamageProfile) -> Dict[str, Any]:
    return {
        "A": _safe_float(profile.amplitude),
        "q": _safe_float(profile.decay),
        "c": _safe_float(profile.background),
        "phi": _safe_float(profile.phi),
        "zfit": _safe_float(profile.zfit),
        "fit_ct0": _safe_float(profile.fit_ct0),
        "fit_ga0": _safe_float(profile.fit_ga0),
        "nll": _safe_float(profile.nll),
        "positions": [
            {
                "position": position.position,
                "dx5": _safe_float(position.dx5),
                "dx3": _safe_float(position.dx3),
            }
            for position in profile.positions
        ],
    }


def _safe_float(value: float) -> Optional[float]:
    return value if math.isfinite(value) else None


def _safe_ratio(numerator: float, denominator: float) -> Optional[float]:
    if (
        not math.isfinite(numerator)
        or not math.isfinite(denominator)
        or denominator <= 0
    ):
        return None
    return numerator / denominator
