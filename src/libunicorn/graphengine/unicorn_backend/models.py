"""Backend data models for Unicorn Graph Engine."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
import math
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from unicorn_backend.damage_contract import (
    BDAMAGE_DIRECT_COUNT_SCOPE,
    BDAMAGE_DAMAGE_SCOPE,
    BDAMAGE_SUBTREE_COUNT_SCOPE,
)


def _json_safe_float(value: float) -> Optional[float]:
    return value if math.isfinite(value) else None


@dataclass(frozen=True)
class FileInfo:
    name: str
    size: int
    modified_at: float

    @classmethod
    def from_path(cls, path: Path) -> "FileInfo":
        stat = path.stat()
        return cls(
            name=path.name,
            size=stat.st_size,
            modified_at=stat.st_mtime,
        )

    def fingerprint(self) -> Tuple[int, float]:
        return (self.size, self.modified_at)

    def to_payload(self) -> Dict[str, Any]:
        return {
            "id": self.name,
            "filename": self.name,
            "bytes": self.size,
            "modified_at": datetime.fromtimestamp(
                self.modified_at,
                timezone.utc,
            ).isoformat(),
        }


@dataclass(frozen=True)
class TaxonomyNode:
    taxid: int
    parent: int
    rank: str


@dataclass(frozen=True)
class MetadataModel:
    fileinfo: FileInfo
    fields: Tuple[str, ...]
    rows_by_dataset: Dict[str, Dict[str, str]]
    rows_total: int

    def to_status_payload(
        self,
        available_dataset_names: Optional[set[str]] = None,
    ) -> Dict[str, Any]:
        available = available_dataset_names or set()
        matched_datasets = sorted(
            dataset_name
            for dataset_name in self.rows_by_dataset
            if dataset_name in available
        )
        matched_rows = len(matched_datasets)
        unmatched_rows = self.rows_total - matched_rows
        return {
            "filename": self.fileinfo.name,
            "fields": list(self.fields),
            "rows_total": self.rows_total,
            "matched_rows": matched_rows,
            "unmatched_rows": unmatched_rows,
            "datasets_with_metadata": matched_datasets,
        }


@dataclass(frozen=True, slots=True)
class DamagePosition:
    position: int
    k5: float
    n5: float
    k3: float
    n3: float
    dx5: float
    dx3: float

    def to_payload(self) -> Dict[str, Any]:
        return {
            "position": self.position,
            "k5": _json_safe_float(self.k5),
            "n5": _json_safe_float(self.n5),
            "k3": _json_safe_float(self.k3),
            "n3": _json_safe_float(self.n3),
            "dx5": _json_safe_float(self.dx5),
            "dx3": _json_safe_float(self.dx3),
        }


@dataclass(frozen=True, slots=True)
class DamageProfile:
    taxid: int
    direct_count: int
    subtree_count: int
    ct_frequency: float
    ga_frequency: float
    amplitude: float
    decay: float
    background: float
    phi: float
    zfit: float
    fit_ct0: float
    fit_ga0: float
    nll: float
    positions: Tuple[DamagePosition, ...]
    fit_valid: bool
    missing_fields: Tuple[str, ...]

    @property
    def direct_count_scope(self) -> str:
        return BDAMAGE_DIRECT_COUNT_SCOPE

    @property
    def subtree_count_scope(self) -> str:
        return BDAMAGE_SUBTREE_COUNT_SCOPE

    @property
    def damage_scope(self) -> str:
        return BDAMAGE_DAMAGE_SCOPE

    def to_payload(self) -> Dict[str, Any]:
        return {
            "taxid": self.taxid,
            "direct_count": self.direct_count,
            "subtree_count": self.subtree_count,
            "direct_count_scope": self.direct_count_scope,
            "subtree_count_scope": self.subtree_count_scope,
            "damage_scope": self.damage_scope,
            "ct_frequency": _json_safe_float(self.ct_frequency),
            "ga_frequency": _json_safe_float(self.ga_frequency),
            "A": _json_safe_float(self.amplitude),
            "q": _json_safe_float(self.decay),
            "c": _json_safe_float(self.background),
            "phi": _json_safe_float(self.phi),
            "zfit": _json_safe_float(self.zfit),
            "fit_ct0": _json_safe_float(self.fit_ct0),
            "fit_ga0": _json_safe_float(self.fit_ga0),
            "nll": _json_safe_float(self.nll),
            "positions": [
                position.to_payload()
                for position in self.positions
            ],
            "fit_valid": self.fit_valid,
            "missing_fields": list(self.missing_fields),
        }


@dataclass
class DatasetModel:
    fileinfo: FileInfo
    counts_map: Dict[int, int]
    names_map: Dict[int, str]
    counts_payload: List[Dict[str, Any]]
    total_reads: int
    total_taxa: int
    damage_by_taxid: Dict[int, DamageProfile] = field(default_factory=dict)
    damage_schema: Optional[str] = None

    @property
    def damage_taxa(self) -> int:
        return len(self.damage_by_taxid)

    @property
    def valid_damage_taxa(self) -> int:
        return sum(
            profile.fit_valid
            for profile in self.damage_by_taxid.values()
        )

    @property
    def invalid_damage_taxa(self) -> int:
        return self.damage_taxa - self.valid_damage_taxa

    def damage_summary_payload(self) -> Dict[str, Any]:
        return {
            "available": self.damage_schema is not None,
            "schema": self.damage_schema,
            "taxa": self.damage_taxa,
            "valid_taxa": self.valid_damage_taxa,
            "invalid_taxa": self.invalid_damage_taxa,
            "direct_count_scope": BDAMAGE_DIRECT_COUNT_SCOPE,
            "subtree_count_scope": BDAMAGE_SUBTREE_COUNT_SCOPE,
            "damage_scope": BDAMAGE_DAMAGE_SCOPE,
        }

    def to_render_payload(self) -> Dict[str, Any]:
        payload = self.fileinfo.to_payload()
        payload.update(
            {
                "total_reads": self.total_reads,
                "total_taxa": self.total_taxa,
                "counts": self.counts_payload,
            }
        )
        return payload

    def to_summary_payload(
        self,
        metadata: Optional[Dict[str, str]] = None,
    ) -> Dict[str, Any]:
        payload = self.fileinfo.to_payload()
        payload.update(
            {
                "total_reads": self.total_reads,
                "total_taxa": self.total_taxa,
                "damage": self.damage_summary_payload(),
            }
        )
        if metadata is not None:
            payload["metadata"] = dict(metadata)
        return payload


@dataclass
class TaxonomyModel:
    nodes_fileinfo: FileInfo
    names_fileinfo: Optional[FileInfo]
    nodes_map: Dict[int, TaxonomyNode]
    names_map: Dict[int, str]

    def cache_key(
        self,
    ) -> Tuple[
        str,
        Tuple[int, float],
        Optional[str],
        Optional[Tuple[int, float]],
    ]:
        return (
            self.nodes_fileinfo.name,
            self.nodes_fileinfo.fingerprint(),
            self.names_fileinfo.name if self.names_fileinfo else None,
            self.names_fileinfo.fingerprint() if self.names_fileinfo else None,
        )

    def to_status_payload(self) -> Dict[str, Any]:
        return {
            "nodes_file": self.nodes_fileinfo.to_payload(),
            "names_file": (
                self.names_fileinfo.to_payload()
                if self.names_fileinfo
                else None
            ),
            "node_count": len(self.nodes_map),
            "name_count": len(self.names_map),
        }


@dataclass
class TreeNodeModel:
    taxid: int
    parent: Optional[int]
    rank: str
    name: str
    direct: int
    direct_by_source: List[int]
    total: int = 0
    total_by_source: List[int] = field(default_factory=list)
    children: List["TreeNodeModel"] = field(default_factory=list)
    depth: int = 0

    def to_payload(self) -> Dict[str, Any]:
        return {
            "taxid": self.taxid,
            "parent": self.parent,
            "rank": self.rank,
            "name": self.name,
            "direct": self.direct,
            "direct_by_source": self.direct_by_source,
            "total": self.total,
            "total_by_source": self.total_by_source,
            "depth": self.depth,
            "children": [child.to_payload() for child in self.children],
        }

    def has_taxid(self, taxid: int) -> bool:
        if self.taxid == taxid:
            return True
        return any(child.has_taxid(taxid) for child in self.children)

    def find_taxid(self, taxid: int) -> Optional["TreeNodeModel"]:
        if self.taxid == taxid:
            return self
        for child in self.children:
            found = child.find_taxid(taxid)
            if found is not None:
                return found
        return None


@dataclass
class SelectionModel:
    dataset_names: Tuple[str, ...]
    datasets: List[DatasetModel]
    direct_counts: Dict[int, int]
    names_map: Dict[int, str]
    total_reads: int
    total_taxa: int

    def to_status_payload(self) -> Dict[str, Any]:
        return {
            "dataset_count": len(self.datasets),
            "datasets": [
                dataset.to_summary_payload()
                for dataset in self.datasets
            ],
            "total_reads": self.total_reads,
            "total_taxa": self.total_taxa,
            "direct_taxa": len(self.direct_counts),
        }


@dataclass
class TreeModel:
    root: TreeNodeModel
    missing_taxids: List[int]
    dataset_names: Tuple[str, ...]
    taxonomy_key: Tuple[
        str,
        Tuple[int, float],
        Optional[str],
        Optional[Tuple[int, float]],
    ]

    def to_status_payload(self) -> Dict[str, Any]:
        return {
            "root_taxid": self.root.taxid,
            "root_name": self.root.name,
            "root_total": self.root.total,
            "node_count": self.count_nodes(),
            "missing_taxids": len(self.missing_taxids),
            "missing_taxid_examples": self.missing_taxids[:10],
        }

    def count_nodes(self) -> int:
        count = 0
        stack = [self.root]
        while stack:
            node = stack.pop()
            count += 1
            stack.extend(node.children)
        return count
