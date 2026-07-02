from __future__ import annotations

import csv
import logging
import os
import json
import re
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from threading import RLock
from typing import Any, Dict, List, Optional, Tuple
from urllib import error as urllib_error
from urllib import request as urllib_request

from fastapi import Body, FastAPI, File, HTTPException, Query, UploadFile
from fastapi.middleware.cors import CORSMiddleware

from unicorn_compute.barplot import build_count_matrix_barplot_spec
from unicorn_compute.pcoa import build_count_matrix_pcoa_spec


HOST = os.environ.get("UNICORN_GRAPHENGINE_HOST", "127.0.0.1")
PORT = int(os.environ.get("UNICORN_GRAPHENGINE_PORT", "8000"))
UPLOAD_DIR = Path(os.environ.get("UNICORN_GRAPHENGINE_UPLOAD_DIR", "uploads")).resolve()
UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
NODES_FILENAME = os.environ.get("UNICORN_GRAPHENGINE_NODES_FILE", "nodes.dmp")
NAMES_FILENAME = os.environ.get("UNICORN_GRAPHENGINE_NAMES_FILE", "names.dmp")
OPENAI_RESPONSES_ENDPOINT = "https://api.openai.com/v1/responses"
GOOGLE_INTERACTIONS_ENDPOINT = "https://generativelanguage.googleapis.com/v1beta/interactions"
LOCAL_OPENAI_COMPAT_DEFAULT_ENDPOINT = "http://localhost:8542/v1/chat/completions"
AGENT_PROVIDER_LOG_PATH = Path(
    os.environ.get("UNICORN_GRAPHENGINE_AGENT_LOG", str(UPLOAD_DIR / "graphengine_agent_provider.jsonl"))
).resolve()
AGENT_PROVIDER_RAW_LOG_DIR = Path(
    os.environ.get("UNICORN_GRAPHENGINE_AGENT_RAW_LOG_DIR", "logs/agent_provider")
).resolve()
METADATA_FILENAME = "metadata.txt"


app = FastAPI(title="Unicorn Graph Engine Prototype API")
LOGGER = logging.getLogger("unicorn.graphengine")


def _utc_iso(timestamp: float) -> str:
    return datetime.fromtimestamp(timestamp, timezone.utc).isoformat()


def _clean_name(value: str) -> str:
    return value.strip().strip('"')


def _redact_provider_runtime_config(runtime_config: Dict[str, Any]) -> Dict[str, Any]:
    redacted = dict(runtime_config or {})
    if "api_key" in redacted and redacted["api_key"]:
        redacted["api_key"] = "***redacted***"
    return redacted


def _append_agent_provider_log(event: str, payload: Dict[str, Any]) -> None:
    AGENT_PROVIDER_LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    line = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "event": event,
        "payload": payload,
    }
    with AGENT_PROVIDER_LOG_PATH.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(line, ensure_ascii=True) + "\n")


def _dump_agent_provider_raw_response(
    provider_name: str,
    response_body: str,
    *,
    provider_payload: Optional[Dict[str, Any]] = None,
    runtime_config: Optional[Dict[str, Any]] = None,
    endpoint: str = "",
) -> Path:
    AGENT_PROVIDER_RAW_LOG_DIR.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    outpath = AGENT_PROVIDER_RAW_LOG_DIR / f"{timestamp}.log"
    redacted_runtime_config = _redact_provider_runtime_config(runtime_config or {})
    payload = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "provider_name": provider_name,
        "endpoint": endpoint,
        "runtime_config": redacted_runtime_config,
        "provider_payload": provider_payload or {},
        "raw_response": response_body,
    }
    with outpath.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    return outpath


def _normalize_requested_files(files: Optional[List[str]]) -> List[str]:
    if not files:
        return []
    normalized = []
    for value in files:
        for item in value.split(","):
            item = item.strip()
            if item:
                normalized.append(Path(item).name)
    return normalized


def _is_bdamage_file(path: Path) -> bool:
    return path.is_file() and path.name.endswith(".bdamage.txt")


def _list_bdamage_files() -> List[Path]:
    return sorted(path for path in UPLOAD_DIR.iterdir() if _is_bdamage_file(path))


def _default_taxonomy_path(filename: str) -> Optional[Path]:
    if not filename:
        return None
    candidate = Path(filename)
    if candidate.is_absolute():
        return candidate if candidate.exists() else None
    candidate = UPLOAD_DIR / filename
    return candidate if candidate.exists() else None


def _resolve_taxonomy_paths(
    nodes_name: Optional[str] = None,
    names_name: Optional[str] = None,
) -> Tuple[Optional[Path], Optional[Path]]:
    nodes_path = _default_taxonomy_path(nodes_name or NODES_FILENAME)
    names_path = _default_taxonomy_path(names_name or NAMES_FILENAME)
    return nodes_path, names_path


def _raise_filesystem_http_error(path: Path, *, operation: str, file_role: str) -> None:
    try:
        path_text = str(path)
    except Exception:
        path_text = path.name
    raise HTTPException(
        status_code=403,
        detail={
            "message": f"Backend could not {operation} {file_role} because the file is not readable.",
            "code": "backend_file_not_readable",
            "file_role": file_role,
            "path": path_text,
            "filename": path.name,
        },
    )


@dataclass(frozen=True)
class FileInfo:
    name: str
    size: int
    modified_at: float

    @classmethod
    def from_path(cls, path: Path) -> "FileInfo":
        try:
            stat = path.stat()
        except PermissionError:
            _raise_filesystem_http_error(path, operation="stat", file_role="backend file")
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
            "modified_at": _utc_iso(self.modified_at),
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
    
    def to_status_payload(self, available_dataset_names: Optional[set[str]] = None) -> Dict[str, Any]:
        available = available_dataset_names or set()
        matched_datasets = sorted(dataset_name for dataset_name in self.rows_by_dataset if dataset_name in available)
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

    def to_summary_payload(self, metadata: Optional[Dict[str, str]] = None) -> Dict[str, Any]:
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

    def cache_key(self) -> Tuple[str, Tuple[int, float], Optional[str], Optional[Tuple[int, float]]]:
        return (
            self.nodes_fileinfo.name,
            self.nodes_fileinfo.fingerprint(),
            self.names_fileinfo.name if self.names_fileinfo else None,
            self.names_fileinfo.fingerprint() if self.names_fileinfo else None,
        )

    def to_status_payload(self) -> Dict[str, Any]:
        return {
            "nodes_file": self.nodes_fileinfo.to_payload(),
            "names_file": self.names_fileinfo.to_payload() if self.names_fileinfo else None,
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
            "datasets": [dataset.to_summary_payload() for dataset in self.datasets],
            "total_reads": self.total_reads,
            "total_taxa": self.total_taxa,
            "direct_taxa": len(self.direct_counts),
        }


@dataclass
class TreeModel:
    root: TreeNodeModel
    missing_taxids: List[int]
    dataset_names: Tuple[str, ...]
    taxonomy_key: Tuple[str, Tuple[int, float], Optional[str], Optional[Tuple[int, float]]]

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


class GraphEngineStore:
    def __init__(self) -> None:
        self._dataset_cache: Dict[str, DatasetModel] = {}
        self._selection_cache: Dict[Tuple[str, ...], SelectionModel] = {}
        self._taxonomy_cache: Optional[TaxonomyModel] = None
        self._metadata_cache: Optional[MetadataModel] = None
        self._tree_cache: Dict[
            Tuple[Tuple[str, ...], Tuple[str, Tuple[int, float], Optional[str], Optional[Tuple[int, float]]]],
            TreeModel,
        ] = {}
        self._lock = RLock()

    def _parse_dataset(self, path: Path) -> DatasetModel:
        fileinfo = FileInfo.from_path(path)
        counts_map: Dict[int, int] = {}
        names_map: Dict[int, str] = {}
        counts_payload: List[Dict[str, Any]] = []
        total_reads = 0
        total_taxa = 0
        try:
            with path.open("r", encoding="utf-8") as handle:
                for raw_line in handle:
                    line = raw_line.strip()
                    if not line or line.startswith("#"):
                        continue
                    parts = line.split("\t")
                    if len(parts) < 2:
                        continue
                    try:
                        taxid = int(parts[0])
                        count = int(parts[1])
                    except ValueError:
                        continue
                    name = _clean_name("\t".join(parts[2:])) if len(parts) > 2 else ""
                    clean_name = name or "NA"
                    counts_map[taxid] = counts_map.get(taxid, 0) + count
                    if clean_name != "NA" and taxid not in names_map:
                        names_map[taxid] = clean_name
                    counts_payload.append(
                        {
                            "taxid": taxid,
                            "count": count,
                            "name": clean_name,
                        }
                    )
                    total_reads += count
                    total_taxa += 1
        except PermissionError:
            _raise_filesystem_http_error(path, operation="read", file_role="dataset file")
        return DatasetModel(
            fileinfo=fileinfo,
            counts_map=counts_map,
            names_map=names_map,
            counts_payload=counts_payload,
            total_reads=total_reads,
            total_taxa=total_taxa,
        )

    def _parse_nodes(self, path: Path) -> Dict[int, TaxonomyNode]:
        nodes: Dict[int, TaxonomyNode] = {}
        try:
            with path.open("r", encoding="utf-8", errors="replace") as handle:
                for raw_line in handle:
                    line = raw_line.strip()
                    if not line:
                        continue
                    parts = [part.strip() for part in line.split("|")]
                    if len(parts) < 2:
                        continue
                    try:
                        taxid = int(parts[0])
                        parent = int(parts[1])
                    except ValueError:
                        continue
                    rank = parts[2] if len(parts) > 2 and parts[2] else "no rank"
                    nodes[taxid] = TaxonomyNode(taxid=taxid, parent=parent, rank=rank)
        except PermissionError:
            _raise_filesystem_http_error(path, operation="read", file_role="taxonomy nodes file")
        return nodes

    def _parse_names(self, path: Optional[Path]) -> Dict[int, str]:
        names: Dict[int, str] = {}
        if not path or not path.exists():
            return names
        try:
            with path.open("r", encoding="utf-8", errors="replace") as handle:
                for raw_line in handle:
                    line = raw_line.strip()
                    if not line:
                        continue
                    parts = [part.strip() for part in line.split("|")]
                    if len(parts) < 2:
                        continue
                    try:
                        taxid = int(parts[0])
                    except ValueError:
                        continue
                    name = parts[1]
                    cls = parts[3] if len(parts) > 3 else ""
                    if not name:
                        continue
                    if cls == "scientific name" or taxid not in names:
                        names[taxid] = name
        except PermissionError:
            _raise_filesystem_http_error(path, operation="read", file_role="taxonomy names file")
        return names

    def _parse_metadata(self, path: Path) -> MetadataModel:
        fileinfo = FileInfo.from_path(path)
        try:
            with path.open("r", encoding="utf-8", errors="replace", newline="") as handle:
                reader = csv.reader(handle, delimiter="\t")
                header: Optional[List[str]] = None
                rows_by_dataset: Dict[str, Dict[str, str]] = {}
                rows_total = 0
                dataset_column = -1
                fields: List[str] = []
                for raw_row in reader:
                    row = [cell.strip() for cell in raw_row]
                    if not row or not any(row):
                        continue
                    if header is None:
                        header = row
                        if "dataset" not in header:
                            raise HTTPException(
                                status_code=400,
                                detail=_error_detail(
                                    "Metadata file must contain a 'dataset' header column.",
                                    code="metadata_missing_dataset_column",
                                ),
                            )
                        dataset_column = header.index("dataset")
                        fields = [column for column in header if column and column != "dataset"]
                        continue
                    if header is None:
                        continue
                    if len(row) < len(header):
                        row = row + ([""] * (len(header) - len(row)))
                    elif len(row) > len(header):
                        row = row[:len(header)]
                    dataset_name = row[dataset_column].strip()
                    if not dataset_name:
                        raise HTTPException(
                            status_code=400,
                            detail=_error_detail(
                                "Metadata rows must include a non-empty dataset value.",
                                code="metadata_missing_dataset_value",
                                row_number=rows_total + 2,
                            ),
                        )
                    dataset_key = Path(dataset_name).name
                    if dataset_key in rows_by_dataset:
                        raise HTTPException(
                            status_code=400,
                            detail=_error_detail(
                                "Metadata file contains duplicate dataset rows.",
                                code="metadata_duplicate_dataset",
                                dataset=dataset_key,
                            ),
                        )
                    row_payload: Dict[str, str] = {}
                    for index, column in enumerate(header):
                        if not column or column == "dataset":
                            continue
                        row_payload[column] = row[index] if index < len(row) else ""
                    rows_by_dataset[dataset_key] = row_payload
                    rows_total += 1
                if header is None:
                    raise HTTPException(
                        status_code=400,
                        detail=_error_detail(
                            "Metadata file must contain a header row.",
                            code="metadata_missing_header",
                        ),
                    )
        except PermissionError:
            _raise_filesystem_http_error(path, operation="read", file_role="metadata file")

        return MetadataModel(
            fileinfo=fileinfo,
            fields=tuple(fields),
            rows_by_dataset=rows_by_dataset,
            rows_total=rows_total,
        )

    def _prune_stale_caches(self, available_names: set[str]) -> None:
        stale_datasets = [name for name in self._dataset_cache if name not in available_names]
        for name in stale_datasets:
            del self._dataset_cache[name]
        stale_selections = [
            key for key in self._selection_cache
            if any(name not in available_names for name in key)
        ]
        for key in stale_selections:
            del self._selection_cache[key]
        stale_trees = [
            key for key in self._tree_cache
            if any(name not in available_names for name in key[0])
        ]
        for key in stale_trees:
            del self._tree_cache[key]

    def list_files(self) -> List[Path]:
        with self._lock:
            files = _list_bdamage_files()
            self._prune_stale_caches({path.name for path in files})
            return files

    def get_metadata(self) -> Optional[MetadataModel]:
        metadata_path = UPLOAD_DIR / METADATA_FILENAME
        with self._lock:
            cached = self._metadata_cache
            if not metadata_path.exists():
                self._metadata_cache = None
                return None
            current_info = FileInfo.from_path(metadata_path)
            if cached and cached.fileinfo.fingerprint() == current_info.fingerprint():
                return cached
        try:
            metadata = self._parse_metadata(metadata_path)
        except HTTPException as error:
            LOGGER.warning(
                "Could not auto-load backend metadata file %s: %s",
                metadata_path,
                error.detail.get("message") if isinstance(error.detail, dict) else error.detail,
            )
            with self._lock:
                self._metadata_cache = None
            return None
        with self._lock:
            self._metadata_cache = metadata
        return metadata

    def load_metadata(self, path: Path) -> MetadataModel:
        metadata = self._parse_metadata(path)
        with self._lock:
            self._metadata_cache = metadata
        return metadata

    def metadata_summary_payload(self) -> Optional[Dict[str, Any]]:
        metadata = self.get_metadata()
        if metadata is None:
            return None
        available_dataset_names = {path.name for path in self.list_files()}
        return metadata.to_status_payload(available_dataset_names)

    def metadata_for_dataset(self, dataset_name: str) -> Optional[Dict[str, str]]:
        metadata = self.get_metadata()
        if metadata is None:
            return None
        row = metadata.rows_by_dataset.get(Path(dataset_name).name)
        return dict(row) if row is not None else None

    def dataset_summary_payload(self, dataset: DatasetModel) -> Dict[str, Any]:
        return dataset.to_summary_payload(self.metadata_for_dataset(dataset.fileinfo.name))

    def selection_status_payload(self, selection: SelectionModel) -> Dict[str, Any]:
        return {
            "dataset_count": len(selection.datasets),
            "datasets": [self.dataset_summary_payload(dataset) for dataset in selection.datasets],
            "total_reads": selection.total_reads,
            "total_taxa": selection.total_taxa,
            "direct_taxa": len(selection.direct_counts),
        }

    def get_or_load_dataset(self, path: Path) -> DatasetModel:
        with self._lock:
            current_info = FileInfo.from_path(path)
            cached = self._dataset_cache.get(path.name)
            if cached and cached.fileinfo.fingerprint() == current_info.fingerprint():
                return cached
            loaded = self._parse_dataset(path)
            self._dataset_cache[path.name] = loaded
            stale_selections = [key for key in self._selection_cache if path.name in key]
            for key in stale_selections:
                del self._selection_cache[key]
            stale_trees = [key for key in self._tree_cache if path.name in key[0]]
            for key in stale_trees:
                del self._tree_cache[key]
            return loaded

    def get_or_load_taxonomy(
        self,
        nodes_name: Optional[str] = None,
        names_name: Optional[str] = None,
    ) -> TaxonomyModel:
        nodes_path, names_path = _resolve_taxonomy_paths(nodes_name, names_name)
        if not nodes_path or not nodes_path.exists():
            raise HTTPException(
                status_code=404,
                detail={
                    "message": "Taxonomy nodes file not found on the backend.",
                    "nodes_file": nodes_name or NODES_FILENAME,
                },
            )

        nodes_info = FileInfo.from_path(nodes_path)
        names_info = FileInfo.from_path(names_path) if names_path and names_path.exists() else None
        with self._lock:
            cached = self._taxonomy_cache
            if (
                cached
                and cached.nodes_fileinfo.fingerprint() == nodes_info.fingerprint()
                and cached.nodes_fileinfo.name == nodes_info.name
                and (
                    (not cached.names_fileinfo and not names_info)
                    or (
                        cached.names_fileinfo
                        and names_info
                        and cached.names_fileinfo.name == names_info.name
                        and cached.names_fileinfo.fingerprint() == names_info.fingerprint()
                    )
                )
            ):
                return cached

        taxonomy = TaxonomyModel(
            nodes_fileinfo=nodes_info,
            names_fileinfo=names_info,
            nodes_map=self._parse_nodes(nodes_path),
            names_map=self._parse_names(names_path),
        )
        with self._lock:
            self._taxonomy_cache = taxonomy
            self._tree_cache.clear()
        return taxonomy

    def build_selection(self, names: List[str]) -> SelectionModel:
        selection_key = tuple(sorted(names))
        with self._lock:
            cached = self._selection_cache.get(selection_key)
            if cached:
                still_valid = True
                for dataset in cached.datasets:
                    path = UPLOAD_DIR / dataset.fileinfo.name
                    if not path.exists():
                        still_valid = False
                        break
                    current_info = FileInfo.from_path(path)
                    if dataset.fileinfo.fingerprint() != current_info.fingerprint():
                        still_valid = False
                        break
                if still_valid:
                    return cached

        available = {path.name: path for path in self.list_files()}
        missing = [name for name in selection_key if name not in available]
        if missing:
            raise HTTPException(
                status_code=404,
                detail={
                    "message": "Requested dataset(s) not found in upload directory.",
                    "missing": missing,
                },
            )

        datasets = [self.get_or_load_dataset(available[name]) for name in selection_key]
        direct_counts: Dict[int, int] = {}
        names_map: Dict[int, str] = {}
        total_reads = 0
        total_taxa = 0
        for dataset in datasets:
            total_reads += dataset.total_reads
            total_taxa += dataset.total_taxa
            for taxid, count in dataset.counts_map.items():
                direct_counts[taxid] = direct_counts.get(taxid, 0) + count
            for taxid, name in dataset.names_map.items():
                names_map.setdefault(taxid, name)

        selection = SelectionModel(
            dataset_names=selection_key,
            datasets=datasets,
            direct_counts=direct_counts,
            names_map=names_map,
            total_reads=total_reads,
            total_taxa=total_taxa,
        )
        with self._lock:
            self._selection_cache[selection_key] = selection
        return selection

    def _compute_totals(self, node: TreeNodeModel, depth: int) -> None:
        node.depth = depth
        node.total = node.direct
        node.total_by_source = list(node.direct_by_source)
        for child in node.children:
            self._compute_totals(child, depth + 1)
            node.total += child.total
            for i, value in enumerate(child.total_by_source):
                node.total_by_source[i] += value
        node.children.sort(key=lambda child: (-child.total, child.name))

    def build_tree_model(
        self,
        selection: SelectionModel,
        taxonomy: TaxonomyModel,
    ) -> TreeModel:
        tree_key = (selection.dataset_names, taxonomy.cache_key())
        with self._lock:
            cached = self._tree_cache.get(tree_key)
            if cached:
                return cached

        counts = selection.direct_counts
        included: set[int] = set()
        missing_taxids: set[int] = set()
        for taxid, count in counts.items():
            if not count or taxid == 0:
                continue
            if taxid not in taxonomy.nodes_map:
                missing_taxids.add(taxid)
                continue
            cur = taxid
            seen: set[int] = set()
            while cur in taxonomy.nodes_map and cur not in seen:
                seen.add(cur)
                included.add(cur)
                parent = taxonomy.nodes_map[cur].parent
                if not parent or parent == cur:
                    break
                cur = parent

        objects: Dict[int, TreeNodeModel] = {}
        for taxid in included:
            raw = taxonomy.nodes_map[taxid]
            objects[taxid] = TreeNodeModel(
                taxid=taxid,
                parent=raw.parent,
                rank=raw.rank,
                name=selection.names_map.get(taxid) or taxonomy.names_map.get(taxid) or str(taxid),
                direct=counts.get(taxid, 0),
                direct_by_source=[dataset.counts_map.get(taxid, 0) for dataset in selection.datasets],
                total_by_source=[0] * len(selection.datasets),
            )

        roots: List[TreeNodeModel] = []
        for node in objects.values():
            parent = objects.get(node.parent) if node.parent is not None else None
            if parent and parent.taxid != node.taxid:
                parent.children.append(node)
            else:
                roots.append(node)

        unknown = counts.get(0, 0)
        if unknown:
            unclassified = TreeNodeModel(
                taxid=0,
                parent=None,
                rank="unclassified",
                name=selection.names_map.get(0) or taxonomy.names_map.get(0) or "unclassified",
                direct=unknown,
                direct_by_source=[dataset.counts_map.get(0, 0) for dataset in selection.datasets],
                total_by_source=[0] * len(selection.datasets),
            )
            if len(roots) == 1:
                roots[0].children.append(unclassified)
            else:
                roots.append(unclassified)

        if len(roots) == 1:
            root = roots[0]
        else:
            root = TreeNodeModel(
                taxid=-1,
                parent=None,
                rank="synthetic root",
                name="root",
                direct=0,
                direct_by_source=[0] * len(selection.datasets),
                total_by_source=[0] * len(selection.datasets),
                children=roots,
            )

        self._compute_totals(root, 0)
        tree = TreeModel(
            root=root,
            missing_taxids=sorted(missing_taxids),
            dataset_names=selection.dataset_names,
            taxonomy_key=taxonomy.cache_key(),
        )
        with self._lock:
            self._tree_cache[tree_key] = tree
        return tree

    def cache_status(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "dataset_cache_entries": len(self._dataset_cache),
                "selection_cache_entries": len(self._selection_cache),
                "tree_cache_entries": len(self._tree_cache),
                "taxonomy_cached": self._taxonomy_cache is not None,
                "cached_datasets": sorted(self._dataset_cache.keys()),
                "cached_selections": [list(key) for key in sorted(self._selection_cache.keys())],
            }

    def build_visible_tree_payload(
        self,
        tree: TreeModel,
        expanded_taxids: set[int],
        min_reads: int,
    ) -> Tuple[Dict[str, Any], List[int]]:
        threshold = max(0, min_reads)
        active_expanded_taxids: set[int] = set()

        def build_node_payload(node: TreeNodeModel, force_expanded: bool = False) -> Optional[Dict[str, Any]]:
            if node is not tree.root and node.total < threshold:
                return None
            eligible_children = [
                child for child in node.children
                if child.total >= threshold
            ]
            expanded = force_expanded or node.taxid in expanded_taxids
            if not force_expanded and expanded and eligible_children:
                active_expanded_taxids.add(node.taxid)
            visible_children = []
            if expanded:
                for child in eligible_children:
                    payload = build_node_payload(child, force_expanded=False)
                    if payload is not None:
                        visible_children.append(payload)
            return {
                "taxid": node.taxid,
                "parent": node.parent,
                "rank": node.rank,
                "name": node.name,
                "direct": node.direct,
                "direct_by_source": node.direct_by_source,
                "total": node.total,
                "total_by_source": node.total_by_source,
                "depth": node.depth,
                "child_count": len(eligible_children),
                "has_children": bool(eligible_children),
                "expanded": expanded,
                "children": visible_children,
            }

        payload = build_node_payload(tree.root, force_expanded=True)
        if payload is None:
            raise HTTPException(status_code=500, detail="Could not build visible tree payload.")
        return payload, sorted(active_expanded_taxids)


STORE = GraphEngineStore()


app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get("/ping")
def ping() -> Dict[str, Any]:
    files = STORE.list_files()
    nodes_path, names_path = _resolve_taxonomy_paths()
    return {
        "ok": True,
        "service": "unicorn-graphengine-prototype",
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "upload_dir": str(UPLOAD_DIR),
        "datasets": len(files),
        "taxonomy": {
            "nodes_file": nodes_path.name if nodes_path else None,
            "names_file": names_path.name if names_path else None,
        },
        "metadata": STORE.metadata_summary_payload(),
        "cache": STORE.cache_status(),
    }


@app.post("/upload")
async def upload(file: UploadFile = File(...)) -> Dict[str, Any]:
    filename = Path(file.filename or "upload.dat").name
    outpath = UPLOAD_DIR / filename
    data = await file.read()
    outpath.write_bytes(data)
    return {
        "ok": True,
        "filename": filename,
        "bytes": len(data),
        "saved_to": str(outpath),
    }


@app.post("/metadata/upload")
async def upload_metadata(file: UploadFile = File(...)) -> Dict[str, Any]:
    outpath = UPLOAD_DIR / METADATA_FILENAME
    data = await file.read()
    outpath.write_bytes(data)
    try:
        metadata = STORE.load_metadata(outpath)
    except Exception:
        try:
            outpath.unlink(missing_ok=True)
        except Exception:
            pass
        raise
    return {
        "ok": True,
        "metadata": STORE.metadata_summary_payload(),
        "saved_to": str(outpath),
    }


@app.get("/metadata/status")
def metadata_status() -> Dict[str, Any]:
    return {
        "ok": True,
        "metadata": STORE.metadata_summary_payload(),
    }


@app.get("/datasets")
def list_datasets() -> Dict[str, Any]:
    datasets = [STORE.dataset_summary_payload(STORE.get_or_load_dataset(path)) for path in STORE.list_files()]
    return {
        "ok": True,
        "datasets": datasets,
        "metadata": STORE.metadata_summary_payload(),
    }


@app.get("/taxonomy/status")
def taxonomy_status(
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
) -> Dict[str, Any]:
    taxonomy = STORE.get_or_load_taxonomy(nodes_name=nodes_file, names_name=names_file)
    return {
        "ok": True,
        "taxonomy": taxonomy.to_status_payload(),
        "cache": STORE.cache_status(),
    }


@app.get("/model/status")
def model_status(
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
) -> Dict[str, Any]:
    available = STORE.list_files()
    requested = _normalize_requested_files(files)
    selected_names = requested if requested else sorted(path.name for path in available)
    selection = STORE.build_selection(selected_names) if selected_names else None
    taxonomy = STORE.get_or_load_taxonomy(nodes_name=nodes_file, names_name=names_file)
    tree = STORE.build_tree_model(selection, taxonomy) if selection else None
    return {
        "ok": True,
        "upload_dir": str(UPLOAD_DIR),
        "available_datasets": [path.name for path in available],
        "selection": STORE.selection_status_payload(selection) if selection else {
            "dataset_count": 0,
            "datasets": [],
            "total_reads": 0,
            "total_taxa": 0,
            "direct_taxa": 0,
        },
        "metadata": STORE.metadata_summary_payload(),
        "taxonomy": taxonomy.to_status_payload(),
        "tree": tree.to_status_payload() if tree else None,
        "cache": STORE.cache_status(),
    }


@app.get("/render-data")
def render_data(files: Optional[List[str]] = Query(default=None)) -> Dict[str, Any]:
    available = STORE.list_files()
    requested = _normalize_requested_files(files)
    selected_names = requested if requested else sorted(path.name for path in available)
    if not selected_names:
        return {
            "ok": True,
            "datasets": [],
            "total_reads": 0,
            "total_taxa": 0,
        }

    selection = STORE.build_selection(selected_names)
    return {
        "ok": True,
        "datasets": [dataset.to_render_payload() for dataset in selection.datasets],
        "total_reads": selection.total_reads,
        "total_taxa": selection.total_taxa,
        "direct_taxa": len(selection.direct_counts),
    }


@app.get("/tree-model")
def tree_model(
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
) -> Dict[str, Any]:
    available = STORE.list_files()
    requested = _normalize_requested_files(files)
    selected_names = requested if requested else sorted(path.name for path in available)
    if not selected_names:
        return {
            "ok": True,
            "datasets": [],
            "taxonomy": None,
            "tree": None,
        }
    selection = STORE.build_selection(selected_names)
    taxonomy = STORE.get_or_load_taxonomy(nodes_name=nodes_file, names_name=names_file)
    tree = STORE.build_tree_model(selection, taxonomy)
    return {
        "ok": True,
        "datasets": [STORE.dataset_summary_payload(dataset) for dataset in selection.datasets],
        "taxonomy": taxonomy.to_status_payload(),
        "tree": tree.root.to_payload(),
        "missing_taxids": tree.missing_taxids,
        "metadata": STORE.metadata_summary_payload(),
        "cache": STORE.cache_status(),
    }


def _resolve_selection_and_tree(
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
) -> Tuple[SelectionModel, TaxonomyModel, TreeModel]:
    available = STORE.list_files()
    requested = _normalize_requested_files(files)
    selected_names = requested if requested else sorted(path.name for path in available)
    if not selected_names:
        raise HTTPException(
            status_code=400,
            detail={"message": "No datasets selected for tree rendering."},
        )
    selection = STORE.build_selection(selected_names)
    taxonomy = STORE.get_or_load_taxonomy(nodes_name=nodes_file, names_name=names_file)
    tree = STORE.build_tree_model(selection, taxonomy)
    return selection, taxonomy, tree


def _response_context(
    selection: SelectionModel,
    taxonomy: TaxonomyModel,
    min_reads: int,
    expanded_taxids: List[int],
) -> Dict[str, Any]:
    return {
        "dataset_names": list(selection.dataset_names),
        "nodes_file": taxonomy.nodes_fileinfo.name,
        "names_file": taxonomy.names_fileinfo.name if taxonomy.names_fileinfo else None,
        "min_reads": min_reads,
        "expanded_taxids": expanded_taxids,
    }


def _error_detail(
    message: str,
    *,
    code: str,
    request_context: Optional[Dict[str, Any]] = None,
    **extra: Any,
) -> Dict[str, Any]:
    detail: Dict[str, Any] = {
        "message": message,
        "code": code,
    }
    if request_context is not None:
        detail["request_context"] = request_context
    detail.update(extra)
    return detail


def _resolve_openai_responses_endpoint(base_url: str) -> str:
    trimmed = str(base_url or "").strip()
    if not trimmed:
        return OPENAI_RESPONSES_ENDPOINT
    without_trailing_slash = trimmed.rstrip("/")
    if without_trailing_slash.endswith("/v1/responses"):
        return without_trailing_slash
    if without_trailing_slash.endswith("/v1"):
        return f"{without_trailing_slash}/responses"
    return f"{without_trailing_slash}/v1/responses"


def _normalize_openai_input_role(role: str) -> str:
    if role in {"developer", "assistant", "system"}:
        return role
    return "user"


def _to_openai_input_message(role: str, content: str) -> Optional[Dict[str, Any]]:
    normalized_role = _normalize_openai_input_role(role)
    text = str(content or "").strip()
    if not text:
        return None
    content_type = "output_text" if normalized_role == "assistant" else "input_text"
    return {
        "role": normalized_role,
        "content": [
            {
                "type": content_type,
                "text": text,
            }
        ],
    }


def _build_openai_internal_response_format() -> Dict[str, Any]:
    return {
        "type": "json_schema",
        "name": "unicorn_provider_turn_response",
        "strict": True,
        "schema": {
            "type": "object",
            "properties": {
                "type": {
                    "type": "string",
                    "enum": ["assistant_message", "tool_call", "final_answer", "error"],
                },
                "content": {
                    "type": ["string", "null"],
                },
                "tool_name": {
                    "type": ["string", "null"],
                },
                "args": {
                    "type": ["object", "null"],
                    "properties": {
                        "taxid": {"type": ["integer", "null"]},
                        "scope": {"type": ["string", "null"], "enum": ["root", "node", None]},
                        "sort": {"type": ["string", "null"], "enum": ["direct", "subtree", None]},
                        "limit": {"type": ["integer", "null"]},
                    },
                    "required": ["taxid", "scope", "sort", "limit"],
                    "additionalProperties": False,
                },
                "tool_summary": {
                    "type": ["array", "null"],
                    "items": {"type": "string"},
                },
                "notes": {
                    "type": ["string", "null"],
                },
                "code": {
                    "type": ["string", "null"],
                },
                "message": {
                    "type": ["string", "null"],
                },
            },
            "required": ["type", "content", "tool_name", "args", "tool_summary", "notes", "code", "message"],
            "additionalProperties": False,
        },
    }


def _build_unicorn_internal_response_schema() -> Dict[str, Any]:
    return {
        "type": "object",
        "properties": {
            "type": {
                "type": "string",
                "enum": ["assistant_message", "tool_call", "final_answer", "error"],
            },
            "content": {
                "type": ["string", "null"],
            },
            "tool_name": {
                "type": ["string", "null"],
            },
            "args": {
                "type": ["object", "null"],
                "properties": {
                    "taxid": {"type": ["integer", "null"]},
                    "scope": {"type": ["string", "null"], "enum": ["root", "node", None]},
                    "sort": {"type": ["string", "null"], "enum": ["direct", "subtree", None]},
                    "limit": {"type": ["integer", "null"]},
                },
                "required": ["taxid", "scope", "sort", "limit"],
                "additionalProperties": False,
            },
            "tool_summary": {
                "type": ["array", "null"],
                "items": {"type": "string"},
            },
            "notes": {
                "type": ["string", "null"],
            },
            "code": {
                "type": ["string", "null"],
            },
            "message": {
                "type": ["string", "null"],
            },
        },
        "required": ["type", "content", "tool_name", "args", "tool_summary", "notes", "code", "message"],
        "additionalProperties": False,
    }


def _build_provider_contract_instruction_text(provider_payload: Dict[str, Any], runtime_config: Dict[str, Any]) -> str:
    tool_names = [
        str(tool.get("name") or "").strip()
        for tool in provider_payload.get("tools", [])
        if isinstance(tool, dict) and str(tool.get("name") or "").strip()
    ]
    runtime_provider = str(runtime_config.get("runtime_provider") or runtime_config.get("configured_provider") or "provider")
    contract_lines = [
        "UNICORN PROVIDER CONTRACT",
        f"- runtime provider: {runtime_provider}",
        "- graph_context is authoritative. Treat it as the source of truth for current session state.",
        "- If the answer is already present in graph_context or tool_results, answer directly.",
        "- Use a Unicorn tool only when the answer is not already present in graph_context or tool_results.",
        "- Never invent a tool name.",
        "- You may only call a tool whose name exactly appears in the advertised tool list.",
        "- If no listed tool applies, return a final_answer or error object instead of a made-up tool.",
        "- Return exactly one JSON object and nothing else.",
        "- Do not emit prose, markdown, code fences, or chain-of-thought outside that JSON object.",
        "- For a tool request, return only a Unicorn internal tool_call object.",
        "- For a direct answer, return only a Unicorn internal final_answer object.",
        "",
        "ADVERTISED_TOOL_NAMES",
        json.dumps(tool_names, ensure_ascii=True),
        "",
        "POSITIVE_EXAMPLES",
        json.dumps(
            {
                "user_prompt": "What min_reads threshold am I using?",
                "graph_context_fact": {"filters": {"min_reads": 1000}},
                "response": {
                    "type": "final_answer",
                    "content": "The current min_reads threshold is 1000.",
                    "tool_summary": [],
                    "notes": "",
                },
            },
            ensure_ascii=True,
        ),
        json.dumps(
            {
                "user_prompt": "Tell me about taxid 2759.",
                "response": {
                    "type": "tool_call",
                    "tool_name": "get_node_details",
                    "args": {"taxid": 2759},
                },
            },
            ensure_ascii=True,
        ),
        json.dumps(
            {
                "user_prompt": "Use a threshold helper tool.",
                "response": {
                    "type": "error",
                    "code": "tool_not_available",
                    "message": "No advertised Unicorn tool matches that request.",
                },
            },
            ensure_ascii=True,
        ),
        "",
        "NEVER_RETURN_TOP_LEVEL_KEYS",
        json.dumps(["tool", "input", "output"], ensure_ascii=True),
    ]
    return "\n".join(contract_lines)


def _build_openai_developer_context_message(provider_payload: Dict[str, Any], runtime_config: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    developer_envelope = {
        "graph_context": provider_payload.get("graph_context") if isinstance(provider_payload.get("graph_context"), dict) else None,
        "tools": provider_payload.get("tools") if isinstance(provider_payload.get("tools"), list) else [],
        "tool_results": provider_payload.get("tool_results") if isinstance(provider_payload.get("tool_results"), list) else [],
        "turn_config": provider_payload.get("turn_config") if isinstance(provider_payload.get("turn_config"), dict) else {},
        "provider_contract": {
            "reply_mode": "json_only",
            "orchestrator": "unicorn",
            "native_provider_tool_calling": False,
        },
        "runtime_config": {
            "runtime_provider": runtime_config.get("runtime_provider") or "openai",
            "transport_mode": runtime_config.get("transport_mode") or "backend",
            "base_url": runtime_config.get("base_url") or "",
        },
    }
    return _to_openai_input_message(
        "developer",
        "\n\n".join([
            "Unicorn provider turn context follows as JSON.",
            _build_provider_contract_instruction_text(provider_payload, runtime_config),
            json.dumps(developer_envelope, indent=2),
        ]),
    )


def _build_openai_responses_request(provider_payload: Dict[str, Any], runtime_config: Dict[str, Any]) -> Dict[str, Any]:
    model = str(runtime_config.get("configured_model") or provider_payload.get("provider", {}).get("model") or "gpt-5")
    input_messages: List[Dict[str, Any]] = []
    developer_context = _build_openai_developer_context_message(provider_payload, runtime_config)
    if developer_context:
        input_messages.append(developer_context)
    for message in provider_payload.get("conversation", []):
        if not isinstance(message, dict):
            continue
        normalized = _to_openai_input_message(str(message.get("role") or ""), str(message.get("content") or ""))
        if normalized:
            input_messages.append(normalized)
    user_prompt = str(provider_payload.get("user_prompt") or "")
    if user_prompt:
        input_messages.append(_to_openai_input_message("user", user_prompt))
    return {
        "model": model,
        "instructions": str(provider_payload.get("system_prompt") or ""),
        "input": input_messages,
        "text": {
            "format": _build_openai_internal_response_format(),
        },
    }


def _resolve_google_interactions_endpoint(base_url: str) -> str:
    trimmed = str(base_url or "").strip()
    if not trimmed:
        return GOOGLE_INTERACTIONS_ENDPOINT
    without_trailing_slash = trimmed.rstrip("/")
    if without_trailing_slash.endswith("/v1beta/interactions"):
        return without_trailing_slash
    if without_trailing_slash.endswith("/v1beta"):
        return f"{without_trailing_slash}/interactions"
    return f"{without_trailing_slash}/v1beta/interactions"


def _resolve_local_openai_compat_chat_endpoint(base_url: str) -> str:
    trimmed = str(base_url or "").strip()
    if not trimmed:
        return LOCAL_OPENAI_COMPAT_DEFAULT_ENDPOINT
    without_trailing_slash = trimmed.rstrip("/")
    if without_trailing_slash.endswith("/v1/chat/completions"):
        return without_trailing_slash
    if without_trailing_slash.endswith("/v1"):
        return f"{without_trailing_slash}/chat/completions"
    return f"{without_trailing_slash}/v1/chat/completions"


def _build_google_interactions_input(provider_payload: Dict[str, Any], runtime_config: Dict[str, Any]) -> str:
    conversation = provider_payload.get("conversation") if isinstance(provider_payload.get("conversation"), list) else []
    tool_results = provider_payload.get("tool_results") if isinstance(provider_payload.get("tool_results"), list) else []
    tools = provider_payload.get("tools") if isinstance(provider_payload.get("tools"), list) else []
    turn_config = provider_payload.get("turn_config") if isinstance(provider_payload.get("turn_config"), dict) else {}
    graph_context = provider_payload.get("graph_context") if isinstance(provider_payload.get("graph_context"), dict) else None
    runtime_provider = runtime_config.get("runtime_provider") or runtime_config.get("configured_provider") or "google"

    sections = [
        "Unicorn provider turn context follows.",
        _build_provider_contract_instruction_text(provider_payload, runtime_config),
        "",
        "GRAPH_CONTEXT_JSON",
        json.dumps(graph_context, indent=2),
        "",
        "TOOLS_JSON",
        json.dumps(tools, indent=2),
        "",
        "TOOL_RESULTS_JSON",
        json.dumps(tool_results, indent=2),
        "",
        "TURN_CONFIG_JSON",
        json.dumps(turn_config, indent=2),
        "",
        "RUNTIME_CONFIG_JSON",
        json.dumps(
            {
                "runtime_provider": runtime_provider,
                "transport_mode": runtime_config.get("transport_mode") or "backend",
                "base_url": runtime_config.get("base_url") or "",
            },
            indent=2,
        ),
        "",
        "CONVERSATION",
    ]

    for message in conversation:
        if not isinstance(message, dict):
            continue
        role = str(message.get("role") or "user")
        content = str(message.get("content") or "").strip()
        if not content:
            continue
        sections.append(f"{role.upper()}: {content}")

    user_prompt = str(provider_payload.get("user_prompt") or "").strip()
    if user_prompt:
        sections.extend(["", "LATEST_USER_PROMPT", user_prompt])
    return "\n".join(sections)


def _build_google_interactions_request(provider_payload: Dict[str, Any], runtime_config: Dict[str, Any]) -> Dict[str, Any]:
    model = str(runtime_config.get("configured_model") or provider_payload.get("provider", {}).get("model") or "gemini-3.5-flash")
    return {
        "model": model,
        "system_instruction": str(provider_payload.get("system_prompt") or ""),
        "input": _build_google_interactions_input(provider_payload, runtime_config),
        "response_format": {
            "type": "text",
            "mime_type": "application/json",
            "schema": _build_unicorn_internal_response_schema(),
        },
    }


def _build_local_openai_compat_chat_request(provider_payload: Dict[str, Any], runtime_config: Dict[str, Any]) -> Dict[str, Any]:
    model = str(runtime_config.get("configured_model") or provider_payload.get("provider", {}).get("model") or "deepseek-ai/DeepSeek-R1-Distill-Qwen-14B")
    messages: List[Dict[str, str]] = []
    system_prompt = str(provider_payload.get("system_prompt") or "").strip()
    if system_prompt:
        messages.append({
            "role": "system",
            "content": system_prompt,
        })
    developer_context = _build_openai_developer_context_message(provider_payload, {
        **runtime_config,
        "runtime_provider": runtime_config.get("runtime_provider") or "local_openai_compat",
    })
    if developer_context:
        context_text = ""
        content = developer_context.get("content")
        if isinstance(content, list):
            text_parts = [
                str(entry.get("text") or "")
                for entry in content
                if isinstance(entry, dict) and isinstance(entry.get("text"), str)
            ]
            context_text = "\n".join(part for part in text_parts if part.strip())
        elif isinstance(content, str):
            context_text = content
        if context_text.strip():
            messages.append({
                "role": "system",
                "content": context_text,
            })
    messages.append({
        "role": "system",
        "content": _build_provider_contract_instruction_text(provider_payload, runtime_config),
    })
    for message in provider_payload.get("conversation", []):
        if not isinstance(message, dict):
            continue
        role = str(message.get("role") or "user")
        if role not in {"user", "assistant", "system"}:
            role = "user"
        content = str(message.get("content") or "").strip()
        if not content:
            continue
        messages.append({
            "role": role,
            "content": content,
        })
    user_prompt = str(provider_payload.get("user_prompt") or "").strip()
    if user_prompt:
        messages.append({
            "role": "user",
            "content": user_prompt,
        })
    return {
        "model": model,
        "messages": messages,
        "temperature": 0.0,
    }


def _extract_google_output_text(api_response: Dict[str, Any]) -> str:
    output_text = api_response.get("output_text")
    if isinstance(output_text, str) and output_text.strip():
        return output_text
    steps = api_response.get("steps")
    if isinstance(steps, list):
        text_parts: List[str] = []
        for step in steps:
            if not isinstance(step, dict):
                continue
            content = step.get("content")
            if not isinstance(content, list):
                continue
            for item in content:
                if not isinstance(item, dict):
                    continue
                text = item.get("text")
                if isinstance(text, str) and text.strip():
                    text_parts.append(text)
        if text_parts:
            return "\n".join(text_parts)
    return ""


def _extract_openai_output_text(api_response: Dict[str, Any]) -> str:
    output = api_response.get("output")
    if not isinstance(output, list):
        return ""
    for item in output:
        if not isinstance(item, dict) or item.get("type") != "message":
            continue
        content = item.get("content")
        if not isinstance(content, list):
            continue
        text_parts = [
            entry.get("text")
            for entry in content
            if isinstance(entry, dict)
            and entry.get("type") == "output_text"
            and isinstance(entry.get("text"), str)
        ]
        if text_parts:
            return "\n".join(text_parts)
    return ""


def _extract_local_openai_compat_output_text(api_response: Dict[str, Any]) -> str:
    choices = api_response.get("choices")
    if not isinstance(choices, list):
        return ""
    for choice in choices:
        if not isinstance(choice, dict):
            continue
        message = choice.get("message")
        if not isinstance(message, dict):
            continue
        content = message.get("content")
        if isinstance(content, str) and content.strip():
            return content
    return ""


def _strip_json_code_fences(text: str) -> str:
    stripped = str(text or "").strip()
    if stripped.startswith("```"):
        lines = stripped.splitlines()
        if lines:
            lines = lines[1:]
        if lines and lines[-1].strip() == "```":
            lines = lines[:-1]
        stripped = "\n".join(lines).strip()
    return stripped


def _strip_think_blocks(text: str) -> str:
    return re.sub(r"<think>.*?</think>", "", str(text or ""), flags=re.DOTALL | re.IGNORECASE).strip()


def _extract_last_fenced_json_block(text: str) -> str:
    matches = re.findall(r"```(?:json)?\s*(.*?)```", str(text or ""), flags=re.DOTALL | re.IGNORECASE)
    if not matches:
        return ""
    return str(matches[-1]).strip()


def _extract_first_top_level_json_object(text: str) -> str:
    source = str(text or "")
    start = source.find("{")
    if start < 0:
        return ""
    depth = 0
    in_string = False
    escape = False
    for index in range(start, len(source)):
        char = source[index]
        if in_string:
            if escape:
                escape = False
            elif char == "\\":
                escape = True
            elif char == "\"":
                in_string = False
            continue
        if char == "\"":
            in_string = True
            continue
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1].strip()
    return ""


def _extract_best_json_candidate(text: str) -> str:
    stripped = _strip_json_code_fences(str(text or ""))
    if stripped.startswith("{") and stripped.endswith("}"):
        return stripped
    fenced = _extract_last_fenced_json_block(text)
    if fenced:
        return fenced
    top_level = _extract_first_top_level_json_object(text)
    if top_level:
        return top_level
    return stripped


def _normalize_internal_provider_response(response: Dict[str, Any]) -> Dict[str, Any]:
    if not isinstance(response, dict):
        raise ValueError("Provider returned an invalid response payload.")
    response_type = str(response.get("type") or "")
    if not response_type:
        raise ValueError("Provider response is missing a type field.")
    if response_type == "assistant_message":
        return {
            "type": response_type,
            "content": str(response.get("content") or ""),
        }
    if response_type == "tool_call":
        args = response.get("args")
        return {
            "type": response_type,
            "tool_name": str(response.get("tool_name") or ""),
            "args": args if isinstance(args, dict) else {},
        }
    if response_type == "final_answer":
        tool_summary = response.get("tool_summary")
        return {
            "type": response_type,
            "content": str(response.get("content") or ""),
            "tool_summary": [str(item) for item in tool_summary] if isinstance(tool_summary, list) else [],
            "notes": "" if response.get("notes") is None else str(response.get("notes")),
        }
    if response_type == "error":
        return {
            "type": response_type,
            "code": "" if response.get("code") is None else str(response.get("code")),
            "message": str(response.get("message") or "Provider returned an error payload."),
        }
    raise ValueError(f"Provider returned unsupported response type: {response_type}")


def _normalize_local_openai_compat_provider_response(response: Dict[str, Any]) -> Dict[str, Any]:
    if not isinstance(response, dict):
        raise ValueError("Provider returned an invalid response payload.")
    if "type" in response:
        return _normalize_internal_provider_response(response)

    tool_name = response.get("tool")
    tool_args = response.get("input")
    if isinstance(tool_name, str) and tool_name.strip():
        return _normalize_internal_provider_response({
            "type": "tool_call",
            "tool_name": tool_name.strip(),
            "args": tool_args if isinstance(tool_args, dict) else {},
        })

    content = response.get("final") or response.get("answer") or response.get("content") or response.get("message")
    if isinstance(content, str) and content.strip():
        return _normalize_internal_provider_response({
            "type": "final_answer",
            "content": content.strip(),
            "tool_summary": [],
            "notes": "",
        })

    raise ValueError("Provider response did not match Unicorn's internal turn schema or the local compatibility shim.")


def _call_openai_provider_turn(provider_payload: Dict[str, Any], runtime_config: Dict[str, Any]) -> Dict[str, Any]:
    api_key = str(runtime_config.get("api_key") or "").strip()
    if not api_key:
        raise HTTPException(status_code=400, detail={"message": "No OpenAI API key was provided for backend-side provider transport."})
    request_body = _build_openai_responses_request(provider_payload, runtime_config)
    endpoint = _resolve_openai_responses_endpoint(str(runtime_config.get("base_url") or ""))
    request = urllib_request.Request(
        endpoint,
        data=json.dumps(request_body).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib_request.urlopen(request, timeout=45) as response:
            response_body = response.read().decode("utf-8")
            response_json = json.loads(response_body)
    except urllib_error.HTTPError as error:
        detail_message = f"OpenAI request failed with HTTP {error.code}"
        try:
            error_body = error.read().decode("utf-8")
            error_json = json.loads(error_body)
            detail_message = str(error_json.get("error", {}).get("message") or detail_message)
        except Exception:
            pass
        raise HTTPException(status_code=502, detail={"message": detail_message, "code": "openai_http_error"})
    except urllib_error.URLError as error:
        raise HTTPException(status_code=502, detail={"message": f"OpenAI request failed before reaching the API: {error.reason}", "code": "openai_network_error"})
    except TimeoutError:
        raise HTTPException(status_code=504, detail={"message": "OpenAI request timed out.", "code": "openai_timeout"})

    if isinstance(response_json, dict) and isinstance(response_json.get("error"), dict) and response_json["error"].get("message"):
        raise HTTPException(status_code=502, detail={"message": str(response_json["error"]["message"]), "code": "openai_api_error"})

    output_text = _extract_openai_output_text(response_json)
    if not output_text:
        raise HTTPException(status_code=502, detail={"message": "OpenAI Responses API returned no assistant JSON output.", "code": "openai_empty_output"})
    try:
        parsed = json.loads(output_text)
    except json.JSONDecodeError as error:
        raise HTTPException(status_code=502, detail={"message": f"OpenAI Responses API returned non-JSON output: {error}", "code": "openai_invalid_json"})
    try:
        return _normalize_internal_provider_response(parsed)
    except ValueError as error:
        raise HTTPException(status_code=502, detail={"message": str(error), "code": "openai_invalid_provider_shape"})


def _call_google_provider_turn(provider_payload: Dict[str, Any], runtime_config: Dict[str, Any]) -> Dict[str, Any]:
    api_key = str(runtime_config.get("api_key") or "").strip()
    if not api_key:
        raise HTTPException(status_code=400, detail={"message": "No Google API key was provided for backend-side provider transport."})
    request_body = _build_google_interactions_request(provider_payload, runtime_config)
    endpoint = _resolve_google_interactions_endpoint(str(runtime_config.get("base_url") or ""))
    request = urllib_request.Request(
        endpoint,
        data=json.dumps(request_body).encode("utf-8"),
        headers={
            "x-goog-api-key": api_key,
            "Content-Type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib_request.urlopen(request, timeout=45) as response:
            response_body = response.read().decode("utf-8")
            response_json = json.loads(response_body)
    except urllib_error.HTTPError as error:
        detail_message = f"Google Gemini request failed with HTTP {error.code}"
        try:
            error_body = error.read().decode("utf-8")
            error_json = json.loads(error_body)
            detail_message = str(error_json.get("error", {}).get("message") or detail_message)
        except Exception:
            pass
        raise HTTPException(status_code=502, detail={"message": detail_message, "code": "google_http_error"})
    except urllib_error.URLError as error:
        raise HTTPException(status_code=502, detail={"message": f"Google Gemini request failed before reaching the API: {error.reason}", "code": "google_network_error"})
    except TimeoutError:
        raise HTTPException(status_code=504, detail={"message": "Google Gemini request timed out.", "code": "google_timeout"})

    if isinstance(response_json, dict) and isinstance(response_json.get("error"), dict) and response_json["error"].get("message"):
        raise HTTPException(status_code=502, detail={"message": str(response_json["error"]["message"]), "code": "google_api_error"})

    output_text = _extract_google_output_text(response_json)
    if not output_text:
        raise HTTPException(status_code=502, detail={"message": "Google Gemini Interactions API returned no assistant JSON output.", "code": "google_empty_output"})
    try:
        parsed = json.loads(output_text)
    except json.JSONDecodeError as error:
        raise HTTPException(status_code=502, detail={"message": f"Google Gemini Interactions API returned non-JSON output: {error}", "code": "google_invalid_json"})
    try:
        return _normalize_internal_provider_response(parsed)
    except ValueError as error:
        raise HTTPException(status_code=502, detail={"message": str(error), "code": "google_invalid_provider_shape"})


def _call_local_openai_compat_provider_turn(provider_payload: Dict[str, Any], runtime_config: Dict[str, Any]) -> Dict[str, Any]:
    api_key = str(runtime_config.get("api_key") or "").strip()
    request_body = _build_local_openai_compat_chat_request(provider_payload, runtime_config)
    endpoint = _resolve_local_openai_compat_chat_endpoint(str(runtime_config.get("base_url") or ""))
    headers = {
        "Content-Type": "application/json",
    }
    if api_key:
        headers["Authorization"] = f"Bearer {api_key}"
    request = urllib_request.Request(
        endpoint,
        data=json.dumps(request_body).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    try:
        with urllib_request.urlopen(request, timeout=90) as response:
            response_body = response.read().decode("utf-8")
            response_json = json.loads(response_body)
    except urllib_error.HTTPError as error:
        detail_message = f"Local OpenAI-compatible request failed with HTTP {error.code}"
        try:
            error_body = error.read().decode("utf-8")
            error_json = json.loads(error_body)
            detail_message = str(error_json.get("error", {}).get("message") or error_json.get("message") or detail_message)
        except Exception:
            pass
        raise HTTPException(status_code=502, detail={"message": detail_message, "code": "local_openai_compat_http_error"})
    except urllib_error.URLError as error:
        raise HTTPException(status_code=502, detail={"message": f"Local OpenAI-compatible request failed before reaching the API: {error.reason}", "code": "local_openai_compat_network_error"})
    except TimeoutError:
        raise HTTPException(status_code=504, detail={"message": "Local OpenAI-compatible request timed out.", "code": "local_openai_compat_timeout"})

    if isinstance(response_json, dict) and isinstance(response_json.get("error"), dict) and response_json["error"].get("message"):
        raise HTTPException(status_code=502, detail={"message": str(response_json["error"]["message"]), "code": "local_openai_compat_api_error"})

    output_text = _extract_local_openai_compat_output_text(response_json)
    raw_log_path = _dump_agent_provider_raw_response(
        "local_openai_compat",
        output_text,
        provider_payload=provider_payload,
        runtime_config=runtime_config,
        endpoint=endpoint,
    )
    if not output_text:
        raise HTTPException(
            status_code=502,
            detail={
                "message": "Local OpenAI-compatible endpoint returned no assistant JSON output.",
                "code": "local_openai_compat_empty_output",
                "raw_response_log": str(raw_log_path),
            },
        )
    sanitized_output_text = _strip_think_blocks(output_text)
    json_candidate_text = _extract_best_json_candidate(sanitized_output_text)
    try:
        parsed = json.loads(json_candidate_text)
    except json.JSONDecodeError as error:
        raise HTTPException(
            status_code=502,
            detail={
                "message": f"Local OpenAI-compatible endpoint returned non-JSON output: {error}",
                "code": "local_openai_compat_invalid_json",
                "raw_response_log": str(raw_log_path),
            },
        )
    try:
        return _normalize_local_openai_compat_provider_response(parsed)
    except ValueError as error:
        raise HTTPException(
            status_code=502,
            detail={
                "message": str(error),
                "code": "local_openai_compat_invalid_provider_shape",
                "raw_response_log": str(raw_log_path),
            },
        )


def _node_passes_filter(node: TreeNodeModel, tree: TreeModel, min_reads: int) -> bool:
    return node is tree.root or node.total >= max(0, min_reads)


def _filtered_child_count(node: TreeNodeModel, min_reads: int) -> int:
    threshold = max(0, min_reads)
    return sum(1 for child in node.children if child.total >= threshold)


def _build_lineage(node: TreeNodeModel, tree: TreeModel) -> List[Dict[str, Any]]:
    lineage: List[Dict[str, Any]] = []
    current: Optional[TreeNodeModel] = node
    while current is not None:
        lineage.append(
            {
                "taxid": current.taxid,
                "name": current.name,
                "rank": current.rank,
            }
        )
        if current is tree.root or current.parent is None:
            break
        current = tree.root.find_taxid(current.parent)
    lineage.reverse()
    return lineage


def _dataset_breakdown(selection: SelectionModel, node: TreeNodeModel) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for index, dataset in enumerate(selection.datasets):
        rows.append(
            {
                "dataset": dataset.fileinfo.name,
                "direct": node.direct_by_source[index] if index < len(node.direct_by_source) else 0,
                "subtree": node.total_by_source[index] if index < len(node.total_by_source) else 0,
            }
        )
    return rows


def _resolve_node_in_context(
    tree: TreeModel,
    taxid: int,
    min_reads: int,
    request_context: Dict[str, Any],
) -> TreeNodeModel:
    node = tree.root.find_taxid(taxid)
    if node is None:
        raise HTTPException(
            status_code=404,
            detail=_error_detail(
                "Requested node is not present in the active tree.",
                code="node_not_in_active_tree",
                request_context=request_context,
                taxid=taxid,
            ),
        )
    if not _node_passes_filter(node, tree, min_reads):
        raise HTTPException(
            status_code=409,
            detail=_error_detail(
                "Requested node exists in the active tree but is filtered out by the current min_reads threshold.",
                code="node_filtered_out",
                request_context=request_context,
                taxid=taxid,
                node_subtree_reads=node.total,
                min_reads=min_reads,
            ),
        )
    return node


def _table_rows_for_subtree(
    node: TreeNodeModel,
    min_reads: int,
    sort_by: str,
    limit: int,
) -> List[Dict[str, Any]]:
    threshold = max(0, min_reads)
    rows: List[Dict[str, Any]] = []
    stack = [node]
    while stack:
        current = stack.pop()
        if current is not node and current.total < threshold:
            continue
        if current.direct > 0:
            rows.append(
                {
                    "taxid": current.taxid,
                    "name": current.name,
                    "rank": current.rank,
                    "depth": current.depth,
                    "direct": current.direct,
                    "subtree": current.total,
                    "child_count": _filtered_child_count(current, min_reads),
                }
            )
        stack.extend(reversed(current.children))

    sort_key = "direct" if sort_by == "direct" else "subtree"
    rows.sort(key=lambda row: (-int(row[sort_key]), -int(row["direct"]), str(row["name"])))
    return rows[:limit]


def _top_children_rows(
    node: TreeNodeModel,
    min_reads: int,
    limit: int,
) -> List[Dict[str, Any]]:
    threshold = max(0, min_reads)
    rows = [
        {
            "taxid": child.taxid,
            "name": child.name,
            "rank": child.rank,
            "depth": child.depth,
            "direct": child.direct,
            "subtree": child.total,
            "child_count": _filtered_child_count(child, min_reads),
        }
        for child in node.children
        if child.total >= threshold
    ]
    rows.sort(key=lambda row: (-int(row["subtree"]), -int(row["direct"]), str(row["name"])))
    return rows[:limit]


def _normalize_taxids(values: Optional[List[int]]) -> List[int]:
    if not values:
        return []
    out: List[int] = []
    seen: set[int] = set()
    for value in values:
        try:
            taxid = int(value)
        except (TypeError, ValueError):
            continue
        if taxid in seen:
            continue
        seen.add(taxid)
        out.append(taxid)
    return out


def _selected_count_matrix_report(
    selection: "SelectionModel",
    selected_nodes: List["TreeNodeModel"],
) -> Dict[str, Any]:
    matrix_rows = []
    for node in selected_nodes:
        matrix_rows.append(
            {
                "taxid": node.taxid,
                "name": node.name,
                "rank": node.rank,
                "direct": node.direct,
                "subtree": node.total,
                "datasets": [
                    {
                        "dataset": dataset.fileinfo.name,
                        "direct": node.direct_by_source[index] if index < len(node.direct_by_source) else 0,
                        "subtree": node.total_by_source[index] if index < len(node.total_by_source) else 0,
                    }
                    for index, dataset in enumerate(selection.datasets)
                ],
            }
        )

    return {
        "summary": {
            "selected_taxids": [int(node.taxid) for node in selected_nodes],
            "selected_node_count": len(selected_nodes),
            "dataset_names": [dataset.fileinfo.name for dataset in selection.datasets],
            "total_direct": sum(int(node.direct) for node in selected_nodes),
            "total_subtree": sum(int(node.total) for node in selected_nodes),
        },
        "matrix": {
            "rows": matrix_rows,
            "dataset_names": [dataset.fileinfo.name for dataset in selection.datasets],
            "row_count": len(matrix_rows),
        },
    }


def _collect_expandable_taxids_in_context(
    node: TreeNodeModel,
    tree: TreeModel,
    min_reads: int,
    expanded_taxids: set[int],
) -> None:
    eligible_children = [
        child for child in node.children
        if _node_passes_filter(child, tree, min_reads)
    ]
    if eligible_children:
        expanded_taxids.add(node.taxid)
    for child in eligible_children:
        _collect_expandable_taxids_in_context(child, tree, min_reads, expanded_taxids)


def _visible_tree_response(
    selection: SelectionModel,
    taxonomy: TaxonomyModel,
    tree: TreeModel,
    min_reads: int,
    expanded_taxids: set[int],
) -> Dict[str, Any]:
    visible_tree, active_expanded_taxids = STORE.build_visible_tree_payload(
        tree,
        expanded_taxids=expanded_taxids,
        min_reads=min_reads,
    )
    return {
        "ok": True,
        "datasets": [STORE.dataset_summary_payload(dataset) for dataset in selection.datasets],
        "taxonomy": taxonomy.to_status_payload(),
        "tree": visible_tree,
        "missing_taxids": tree.missing_taxids,
        "expanded_taxids": active_expanded_taxids,
        "min_reads": min_reads,
        "total_reads": selection.total_reads,
        "direct_taxa": len(selection.direct_counts),
        "request_context": _response_context(selection, taxonomy, min_reads, active_expanded_taxids),
        "metadata": STORE.metadata_summary_payload(),
        "cache": STORE.cache_status(),
    }


@app.get("/root-view")
def root_view(
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
    min_reads: int = Query(default=0, ge=0),
    expanded: Optional[List[int]] = Query(default=None),
) -> Dict[str, Any]:
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    requested_expanded_taxids = set(_normalize_taxids(expanded))
    return _visible_tree_response(selection, taxonomy, tree, min_reads, requested_expanded_taxids)


@app.get("/expand-node")
def expand_node(
    taxid: int = Query(...),
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
    min_reads: int = Query(default=0, ge=0),
    expanded: Optional[List[int]] = Query(default=None),
) -> Dict[str, Any]:
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(selection, taxonomy, min_reads, sorted(set(expanded or [])))
    if not tree.root.has_taxid(taxid):
        raise HTTPException(
            status_code=404,
            detail=_error_detail(
                "Requested node is not present in the active tree.",
                code="node_not_in_active_tree",
                request_context=request_context,
                taxid=taxid,
            ),
        )
    requested_expanded_taxids = set(expanded or [])
    requested_expanded_taxids.add(taxid)
    return _visible_tree_response(selection, taxonomy, tree, min_reads, requested_expanded_taxids)


@app.get("/node-tooltip")
def node_tooltip(
    taxid: int = Query(...),
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
    min_reads: int = Query(default=0, ge=0),
) -> Dict[str, Any]:
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(selection, taxonomy, min_reads, [])
    node = _resolve_node_in_context(tree, taxid, min_reads, request_context)
    return {
        "ok": True,
        "node": {
            "taxid": node.taxid,
            "name": node.name,
            "rank": node.rank,
            "parent": node.parent,
            "depth": node.depth,
            "direct": node.direct,
            "subtree": node.total,
            "child_count": _filtered_child_count(node, min_reads),
            "lineage": _build_lineage(node, tree),
            "datasets": _dataset_breakdown(selection, node),
        },
        "request_context": request_context,
    }


@app.get("/table-view")
def table_view(
    scope: str = Query(default="root"),
    taxid: Optional[int] = Query(default=None),
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
    min_reads: int = Query(default=0, ge=0),
    sort: str = Query(default="direct"),
    limit: int = Query(default=40, ge=1, le=1000),
) -> Dict[str, Any]:
    if scope not in {"root", "node"}:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "scope must be either 'root' or 'node'.",
                code="invalid_scope",
                scope=scope,
            ),
        )
    if sort not in {"direct", "subtree"}:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "sort must be either 'direct' or 'subtree'.",
                code="invalid_sort",
                sort=sort,
            ),
        )

    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(selection, taxonomy, min_reads, [])
    target = tree.root
    if scope == "node":
        if taxid is None:
            raise HTTPException(
                status_code=400,
                detail=_error_detail(
                    "taxid is required when scope='node'.",
                    code="missing_taxid",
                    request_context=request_context,
                    scope=scope,
                ),
            )
        target = _resolve_node_in_context(tree, taxid, min_reads, request_context)

    rows = _table_rows_for_subtree(target, min_reads=min_reads, sort_by=sort, limit=limit)
    return {
        "ok": True,
        "scope": scope,
        "target": {
            "taxid": target.taxid,
            "name": target.name,
            "rank": target.rank,
            "direct": target.direct,
            "subtree": target.total,
            "child_count": _filtered_child_count(target, min_reads),
        },
        "sort": sort,
        "limit": limit,
        "row_count": len(rows),
        "rows": rows,
        "request_context": request_context,
    }


@app.get("/subtree-report")
def subtree_report(
    taxid: Optional[int] = Query(default=None),
    taxids: Optional[List[int]] = Query(default=None),
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
    min_reads: int = Query(default=0, ge=0),
) -> Dict[str, Any]:
    return _subtree_report_payload(taxid, taxids, files, nodes_file, names_file, min_reads)


@app.post("/subtree-report")
def subtree_report_post(payload: Dict[str, Any] = Body(...)) -> Dict[str, Any]:
    return _subtree_report_payload(
        payload.get("taxid"),
        payload.get("taxids"),
        payload.get("files"),
        payload.get("nodes_file"),
        payload.get("names_file"),
        int(payload.get("min_reads", 0) or 0),
    )


@app.get("/rank-report")
def rank_report(
    taxids: Optional[List[int]] = Query(default=None),
    files: Optional[List[str]] = Query(default=None),
    nodes_file: Optional[str] = Query(default=None),
    names_file: Optional[str] = Query(default=None),
    min_reads: int = Query(default=0, ge=0),
) -> Dict[str, Any]:
    return _rank_report_payload(taxids, files, nodes_file, names_file, min_reads)


@app.post("/rank-report")
def rank_report_post(payload: Dict[str, Any] = Body(...)) -> Dict[str, Any]:
    return _rank_report_payload(
        payload.get("taxids"),
        payload.get("files"),
        payload.get("nodes_file"),
        payload.get("names_file"),
        int(payload.get("min_reads", 0) or 0),
    )


def _subtree_report_payload(
    taxid: Optional[int],
    taxids: Optional[List[int]],
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    min_reads: int,
) -> Dict[str, Any]:
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(selection, taxonomy, min_reads, [])
    requested_taxids = _normalize_taxids(taxids)
    if taxid is not None:
        try:
            single_taxid = int(taxid)
        except (TypeError, ValueError):
            single_taxid = None
        if single_taxid is not None and single_taxid not in requested_taxids:
            requested_taxids.append(single_taxid)
    if not requested_taxids:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "At least one taxid is required for a count matrix report.",
                code="missing_taxids",
                request_context=request_context,
            ),
        )
    selected_nodes = [
        _resolve_node_in_context(tree, selected_taxid, min_reads, request_context)
        for selected_taxid in requested_taxids
    ]
    report = _selected_count_matrix_report(selection, selected_nodes)

    return {
        "ok": True,
        "report": report,
        "request_context": request_context,
    }


def _rank_report_payload(
    taxids: Optional[List[int]],
    files: Optional[List[str]],
    nodes_file: Optional[str],
    names_file: Optional[str],
    min_reads: int,
) -> Dict[str, Any]:
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(selection, taxonomy, min_reads, [])
    requested_taxids = _normalize_taxids(taxids)
    if not requested_taxids:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "At least one taxid is required for a rank report.",
                code="missing_taxids",
                request_context=request_context,
            ),
        )

    selected_nodes = [
        _resolve_node_in_context(tree, taxid, min_reads, request_context)
        for taxid in requested_taxids
    ]

    by_rank: Dict[str, Dict[str, Any]] = {}
    for node in selected_nodes:
        rank = node.rank or "no rank"
        slot = by_rank.setdefault(
            rank,
            {
                "rank": rank,
                "direct": 0,
                "node_count": 0,
                "datasets": [
                    {"dataset": dataset.fileinfo.name, "direct": 0}
                    for dataset in selection.datasets
                ],
            },
        )
        slot["direct"] += node.direct
        slot["node_count"] += 1
        for index, entry in enumerate(slot["datasets"]):
            entry["direct"] += node.direct_by_source[index] if index < len(node.direct_by_source) else 0

    rows = sorted(
        by_rank.values(),
        key=lambda row: (-int(row["direct"]), str(row["rank"])),
    )

    return {
        "ok": True,
        "report": {
            "summary": {
                "selected_node_count": len(selected_nodes),
                "selected_taxids": requested_taxids,
                "total_direct": sum(node.direct for node in selected_nodes),
                "dataset_names": list(selection.dataset_names),
            },
            "rows": rows,
        },
        "request_context": request_context,
    }


@app.post("/compute/barplot")
def compute_barplot(payload: Dict[str, Any] = Body(...)) -> Dict[str, Any]:
    taxids = _normalize_taxids(payload.get("taxids") if isinstance(payload, dict) else None)
    if not taxids:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "At least one taxid is required for a barplot compute request.",
                code="missing_taxids",
            ),
        )
    files = payload.get("files") if isinstance(payload.get("files"), list) else None
    nodes_file = str(payload.get("nodes_file") or "") or None
    names_file = str(payload.get("names_file") or "") or None
    min_reads_value = payload.get("min_reads", 0)
    try:
        min_reads = max(0, int(min_reads_value))
    except (TypeError, ValueError):
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "min_reads must be an integer.",
                code="invalid_min_reads",
                min_reads=min_reads_value,
            ),
        )
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(selection, taxonomy, min_reads, [])
    selected_nodes = [
        _resolve_node_in_context(tree, taxid, min_reads, request_context)
        for taxid in taxids
    ]
    report = _selected_count_matrix_report(selection, selected_nodes)
    try:
        spec = build_count_matrix_barplot_spec(
            report,
            count_mode=payload.get("count_mode"),
            dataset_colors=payload.get("dataset_colors"),
            dataset_display=payload.get("dataset_display"),
        )
    except ValueError as error:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                str(error),
                code="invalid_barplot_request",
                request_context=request_context,
            ),
        )
    return {
        "ok": True,
        "spec": spec,
        "request_context": request_context,
    }


@app.post("/uncollapse-to-tips")
def uncollapse_to_tips(payload: Dict[str, Any] = Body(...)) -> Dict[str, Any]:
    taxids = _normalize_taxids(payload.get("taxids") if isinstance(payload, dict) else None)
    if not taxids:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "At least one taxid is required for an uncollapse-to-tips request.",
                code="missing_taxids",
            ),
        )
    files = payload.get("files") if isinstance(payload.get("files"), list) else None
    nodes_file = str(payload.get("nodes_file") or "") or None
    names_file = str(payload.get("names_file") or "") or None
    min_reads_value = payload.get("min_reads", 0)
    try:
        min_reads = max(0, int(min_reads_value))
    except (TypeError, ValueError):
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "min_reads must be an integer.",
                code="invalid_min_reads",
                min_reads=min_reads_value,
            ),
        )
    requested_expanded_taxids = set(
        _normalize_taxids(payload.get("expanded_taxids") if isinstance(payload, dict) else None)
    )
    LOGGER.info(
        "uncollapse-to-tips start selected_taxids=%s requested_expanded_taxids=%s",
        len(taxids),
        len(requested_expanded_taxids),
    )
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(
        selection,
        taxonomy,
        min_reads,
        sorted(requested_expanded_taxids),
    )
    selected_nodes = [
        _resolve_node_in_context(tree, taxid, min_reads, request_context)
        for taxid in taxids
    ]
    for node in selected_nodes:
        _collect_expandable_taxids_in_context(node, tree, min_reads, requested_expanded_taxids)
    response = _visible_tree_response(selection, taxonomy, tree, min_reads, requested_expanded_taxids)
    LOGGER.info(
        "uncollapse-to-tips complete selected_taxids=%s active_expanded_taxids=%s",
        len(taxids),
        len(response.get("expanded_taxids") or []),
    )
    return response


@app.post("/compute/pcoa")
def compute_pcoa(payload: Dict[str, Any] = Body(...)) -> Dict[str, Any]:
    taxids = _normalize_taxids(payload.get("taxids") if isinstance(payload, dict) else None)
    if not taxids:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "At least one taxid is required for a PCoA compute request.",
                code="missing_taxids",
            ),
        )
    files = payload.get("files") if isinstance(payload.get("files"), list) else None
    nodes_file = str(payload.get("nodes_file") or "") or None
    names_file = str(payload.get("names_file") or "") or None
    min_reads_value = payload.get("min_reads", 0)
    try:
        min_reads = max(0, int(min_reads_value))
    except (TypeError, ValueError):
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                "min_reads must be an integer.",
                code="invalid_min_reads",
                min_reads=min_reads_value,
            ),
        )
    selection, taxonomy, tree = _resolve_selection_and_tree(files, nodes_file, names_file)
    request_context = _response_context(selection, taxonomy, min_reads, [])
    selected_nodes = [
        _resolve_node_in_context(tree, taxid, min_reads, request_context)
        for taxid in taxids
    ]
    report = _selected_count_matrix_report(selection, selected_nodes)
    try:
        spec = build_count_matrix_pcoa_spec(
            report,
            count_mode=payload.get("count_mode"),
            distance_metric=payload.get("distance_metric"),
            dataset_colors=payload.get("dataset_colors"),
            dataset_display=payload.get("dataset_display"),
        )
    except NotImplementedError as error:
        raise HTTPException(
            status_code=501,
            detail=_error_detail(
                str(error),
                code="unimplemented_pcoa_request",
                request_context=request_context,
            ),
        )
    except ValueError as error:
        raise HTTPException(
            status_code=400,
            detail=_error_detail(
                str(error),
                code="invalid_pcoa_request",
                request_context=request_context,
            ),
        )
    return {
        "ok": True,
        "spec": spec,
        "request_context": request_context,
    }


@app.post("/agent/provider-turn")
def agent_provider_turn(payload: Dict[str, Any] = Body(...)) -> Dict[str, Any]:
    provider_payload = payload.get("provider_payload")
    runtime_config = payload.get("runtime_config")
    if not isinstance(provider_payload, dict):
        raise HTTPException(status_code=400, detail={"message": "provider_payload is required and must be an object."})
    if not isinstance(runtime_config, dict):
        raise HTTPException(status_code=400, detail={"message": "runtime_config is required and must be an object."})
    provider_name = str(runtime_config.get("runtime_provider") or runtime_config.get("configured_provider") or "mock")
    log_context = {
        "provider_name": provider_name,
        "runtime_config": _redact_provider_runtime_config(runtime_config),
        "provider_payload": provider_payload,
    }
    _append_agent_provider_log("provider_turn_request", log_context)
    try:
        if provider_name == "openai":
            response = _call_openai_provider_turn(provider_payload, runtime_config)
        elif provider_name == "google":
            response = _call_google_provider_turn(provider_payload, runtime_config)
        elif provider_name == "local_openai_compat":
            response = _call_local_openai_compat_provider_turn(provider_payload, runtime_config)
        else:
            raise HTTPException(status_code=400, detail={"message": f"Unsupported backend provider runtime: {provider_name}"})
    except HTTPException as error:
        _append_agent_provider_log(
            "provider_turn_error",
            {
                **log_context,
                "error": error.detail,
                "status_code": error.status_code,
            },
        )
        raise
    _append_agent_provider_log(
        "provider_turn_response",
        {
            **log_context,
            "response": response,
        },
    )
    return response


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host=HOST, port=PORT)
