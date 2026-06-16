# Unicorn Graph Engine Prototype Server

This is the backend service for the graph engine prototype.

It now keeps parsed `.bdamage.txt` datasets in memory on the server side, so the
API can serve cached dataset models instead of reparsing files for every client
request.

## What it does

- `GET /ping`
- `POST /upload`
- `GET /datasets`
- `GET /render-data`
- `GET /model/status`

## Current server-side model

The backend currently owns:

- uploaded `.bdamage.txt` files
- parsed per-dataset direct taxon counts
- per-dataset taxon-name mappings found in the files
- cached selections across one or more datasets

The backend does not yet own taxonomy loading or subtree aggregation. That is
the next step in the server-authoritative roadmap.

## Endpoint notes

- `GET /datasets`
  Returns metadata for available uploaded `.bdamage.txt` files.

- `GET /render-data?files=a&files=b`
  Returns the parsed dataset payloads needed by the current client. This stays
  compatible with the existing frontend while using the server cache internally.

- `GET /model/status?files=a&files=b`
  Returns a backend-facing summary of the in-memory selection model, including
  total reads, total taxon rows, aggregated direct taxa, and cache status.

## Files

- `server_app.py`: FastAPI application
- `server_requirements.txt`: Python dependencies

## Remote server setup

Copy or clone this repository onto the remote machine, then from the directory
containing `server_app.py`:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r server_requirements.txt
python3 server_app.py
```

By default, the service listens on:

- host: `127.0.0.1`
- port: `8000`

That is intentional: it keeps the service private to the remote machine, so you
reach it through an SSH tunnel from your local computer.

## Where files exist on the remote server

Yes: by default, things exist relative to **where you start the server**.

More precisely:

- the API code exists wherever you put `server_app.py`
- uploaded files are stored in `./uploads` relative to the server process
  working directory, unless overridden

Example:

```bash
cd /home/you/unicorn/src/libunicorn/graphengine
python3 server_app.py
```

Then uploads will go to:

```bash
/home/you/unicorn/src/libunicorn/graphengine/uploads
```

## Making the upload directory explicit

If you want uploads somewhere else, set:

```bash
export UNICORN_GRAPHENGINE_UPLOAD_DIR=/scratch/you/unicorn-uploads
python3 server_app.py
```

Then uploads will be written there instead.

## Tunnel from your local machine

On your local machine:

```bash
ssh -L 8000:localhost:8000 youruser@remote-server
```

Then your browser app can test:

```text
http://localhost:8000/ping
```

And for the current in-memory model:

```text
http://localhost:8000/model/status
```

## Good first remote layout

I would suggest one of these:

1. Keep everything under your repo clone

```text
/home/you/unicorn/src/libunicorn/graphengine
```

2. Keep code in the repo, but uploads on a larger filesystem

```text
code:    /home/you/unicorn/src/libunicorn/graphengine
uploads: /scratch/you/unicorn-uploads
```

Option 2 is usually better once files get large.
