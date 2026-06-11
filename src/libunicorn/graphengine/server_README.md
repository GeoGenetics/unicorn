# Unicorn Graph Engine Prototype Server

This is the smallest possible remote service for the graph engine prototype.

## What it does

- `GET /ping`
- `POST /upload`

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
