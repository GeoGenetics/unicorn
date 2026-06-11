import os
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict

from fastapi import FastAPI, File, UploadFile
from fastapi.middleware.cors import CORSMiddleware


HOST = os.environ.get("UNICORN_GRAPHENGINE_HOST", "127.0.0.1")
PORT = int(os.environ.get("UNICORN_GRAPHENGINE_PORT", "8000"))
UPLOAD_DIR = Path(os.environ.get("UNICORN_GRAPHENGINE_UPLOAD_DIR", "uploads")).resolve()
UPLOAD_DIR.mkdir(parents=True, exist_ok=True)


app = FastAPI(title="Unicorn Graph Engine Prototype API")

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


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host=HOST, port=PORT)
