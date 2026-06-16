import os
from dataclasses import dataclass
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


app = FastAPI(title="Unicorn Graph Engine Prototype API")


def _utc_iso(timestamp: float) -> str:
    return datetime.fromtimestamp(timestamp, timezone.utc).isoformat()


def _is_bdamage_file(path: Path) -> bool:
    return path.is_file() and path.name.endswith(".bdamage.txt")


def _list_bdamage_files() -> List[Path]:
    return sorted(path for path in UPLOAD_DIR.iterdir() if _is_bdamage_file(path))


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


@dataclass(frozen=True)
class DatasetFileInfo:
    name: str
    size: int
    modified_at: float

    @classmethod
    def from_path(cls, path: Path) -> "DatasetFileInfo":
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


@dataclass
class DatasetModel:
    fileinfo: DatasetFileInfo
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


class GraphEngineStore:
    def __init__(self) -> None:
        self._dataset_cache: Dict[str, DatasetModel] = {}
        self._selection_cache: Dict[Tuple[str, ...], SelectionModel] = {}
        self._lock = RLock()

    def _parse_dataset(self, path: Path) -> DatasetModel:
        fileinfo = DatasetFileInfo.from_path(path)
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

    def list_files(self) -> List[Path]:
        with self._lock:
            files = _list_bdamage_files()
            self._prune_stale_caches({path.name for path in files})
            return files

    def get_or_load_dataset(self, path: Path) -> DatasetModel:
        with self._lock:
            current_info = DatasetFileInfo.from_path(path)
            cached = self._dataset_cache.get(path.name)
            if cached and cached.fileinfo.fingerprint() == current_info.fingerprint():
                return cached
            loaded = self._parse_dataset(path)
            self._dataset_cache[path.name] = loaded
            stale_selections = [key for key in self._selection_cache if path.name in key]
            for key in stale_selections:
                del self._selection_cache[key]
            return loaded

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
                    current_info = DatasetFileInfo.from_path(path)
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

    def cache_status(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "dataset_cache_entries": len(self._dataset_cache),
                "selection_cache_entries": len(self._selection_cache),
                "cached_datasets": sorted(self._dataset_cache.keys()),
                "cached_selections": [list(key) for key in sorted(self._selection_cache.keys())],
            }


STORE = GraphEngineStore()


# Keep CORS permissive for the prototype so the static app can talk to the API
# through localhost during tunnel testing.
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
    return {
        "ok": True,
        "service": "unicorn-graphengine-prototype",
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "upload_dir": str(UPLOAD_DIR),
        "datasets": len(files),
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
    datasets = [DatasetFileInfo.from_path(path).to_payload() for path in STORE.list_files()]
    return {
        "ok": True,
        "datasets": datasets,
    }


@app.get("/model/status")
def model_status(files: Optional[List[str]] = Query(default=None)) -> Dict[str, Any]:
    available = STORE.list_files()
    requested = _normalize_requested_files(files)
    selected_names = requested if requested else sorted(path.name for path in available)
    selection = STORE.build_selection(selected_names) if selected_names else None
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


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host=HOST, port=PORT)
