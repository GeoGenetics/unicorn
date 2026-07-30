"""Backend data models for Unicorn Graph Engine."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


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


@dataclass
class DatasetModel:
    fileinfo: FileInfo
    counts_map: Dict[int, int]
    names_map: Dict[int, str]
    counts_payload: List[Dict[str, Any]]
    total_reads: int
    total_taxa: int

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
