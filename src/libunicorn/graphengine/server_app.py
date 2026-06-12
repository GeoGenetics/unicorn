import os
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional

from fastapi import FastAPI, File, HTTPException, Query, UploadFile
from fastapi.middleware.cors import CORSMiddleware


HOST = os.environ.get("UNICORN_GRAPHENGINE_HOST", "127.0.0.1")
PORT = int(os.environ.get("UNICORN_GRAPHENGINE_PORT", "8000"))
UPLOAD_DIR = Path(os.environ.get("UNICORN_GRAPHENGINE_UPLOAD_DIR", "uploads")).resolve()
UPLOAD_DIR.mkdir(parents=True, exist_ok=True)


app = FastAPI(title="Unicorn Graph Engine Prototype API")


def _is_bdamage_file(path: Path) -> bool:
    return path.is_file() and path.name.endswith(".bdamage.txt")


def _list_bdamage_files() -> List[Path]:
    return sorted(path for path in UPLOAD_DIR.iterdir() if _is_bdamage_file(path))


def _clean_name(value: str) -> str:
    return value.strip().strip('"')


def _parse_bdamage_file(path: Path) -> Dict[str, Any]:
    counts = []
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
            counts.append(
                {
                    "taxid": taxid,
                    "count": count,
                    "name": name or "NA",
                }
            )
            total_reads += count
            total_taxa += 1
    stat = path.stat()
    return {
        "id": path.name,
        "filename": path.name,
        "bytes": stat.st_size,
        "modified_at": datetime.fromtimestamp(stat.st_mtime, timezone.utc).isoformat(),
        "total_reads": total_reads,
        "total_taxa": total_taxa,
        "counts": counts,
    }


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
    return {
        "ok": True,
        "service": "unicorn-graphengine-prototype",
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "upload_dir": str(UPLOAD_DIR),
        "datasets": len(_list_bdamage_files()),
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
    datasets = []
    for path in _list_bdamage_files():
        stat = path.stat()
        datasets.append(
            {
                "id": path.name,
                "filename": path.name,
                "bytes": stat.st_size,
                "modified_at": datetime.fromtimestamp(stat.st_mtime, timezone.utc).isoformat(),
            }
        )
    return {
        "ok": True,
        "datasets": datasets,
    }


@app.get("/render-data")
def render_data(files: Optional[List[str]] = Query(default=None)) -> Dict[str, Any]:
    available = {path.name: path for path in _list_bdamage_files()}
    requested = _normalize_requested_files(files)
    selected_names = requested if requested else sorted(available.keys())
    if not selected_names:
        return {
            "ok": True,
            "datasets": [],
            "total_reads": 0,
            "total_taxa": 0,
        }

    missing = [name for name in selected_names if name not in available]
    if missing:
        raise HTTPException(
            status_code=404,
            detail={
                "message": "Requested dataset(s) not found in upload directory.",
                "missing": missing,
            },
        )

    datasets = [_parse_bdamage_file(available[name]) for name in selected_names]
    return {
        "ok": True,
        "datasets": datasets,
        "total_reads": sum(dataset["total_reads"] for dataset in datasets),
        "total_taxa": sum(dataset["total_taxa"] for dataset in datasets),
    }


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host=HOST, port=PORT)
