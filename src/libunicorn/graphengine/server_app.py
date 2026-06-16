from __future__ import annotations

import os
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from threading import RLock
from typing import Any, Dict, List, Optional, Tuple

from fastapi import FastAPI, File, HTTPException, Query, UploadFile
from fastapi.middleware.cors import CORSMiddleware


HOST = os.environ.get("UNICORN_GRAPHENGINE_HOST", "127.0.0.1")
PORT = int(os.environ.get("UNICORN_GRAPHENGINE_PORT", "8000"))
UPLOAD_DIR = Path(os.environ.get("UNICORN_GRAPHENGINE_UPLOAD_DIR", "uploads")).resolve()
UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
NODES_FILENAME = os.environ.get("UNICORN_GRAPHENGINE_NODES_FILE", "nodes.dmp")
NAMES_FILENAME = os.environ.get("UNICORN_GRAPHENGINE_NAMES_FILE", "names.dmp")


app = FastAPI(title="Unicorn Graph Engine Prototype API")


def _utc_iso(timestamp: float) -> str:
    return datetime.fromtimestamp(timestamp, timezone.utc).isoformat()


def _clean_name(value: str) -> str:
    return value.strip().strip('"')


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
            "modified_at": _utc_iso(self.modified_at),
        }


@dataclass(frozen=True)
class TaxonomyNode:
    taxid: int
    parent: int
    rank: str


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

    def to_summary_payload(self) -> Dict[str, Any]:
        payload = self.fileinfo.to_payload()
        payload.update(
            {
                "total_reads": self.total_reads,
                "total_taxa": self.total_taxa,
            }
        )
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
        return nodes

    def _parse_names(self, path: Optional[Path]) -> Dict[int, str]:
        names: Dict[int, str] = {}
        if not path or not path.exists():
            return names
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
        return names

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


@app.get("/datasets")
def list_datasets() -> Dict[str, Any]:
    datasets = [FileInfo.from_path(path).to_payload() for path in STORE.list_files()]
    return {
        "ok": True,
        "datasets": datasets,
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
        "selection": selection.to_status_payload() if selection else {
            "dataset_count": 0,
            "datasets": [],
            "total_reads": 0,
            "total_taxa": 0,
            "direct_taxa": 0,
        },
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
        "datasets": [dataset.to_summary_payload() for dataset in selection.datasets],
        "taxonomy": taxonomy.to_status_payload(),
        "tree": tree.root.to_payload(),
        "missing_taxids": tree.missing_taxids,
        "cache": STORE.cache_status(),
    }


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host=HOST, port=PORT)
