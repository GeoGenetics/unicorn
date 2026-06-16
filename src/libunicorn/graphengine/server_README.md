# Unicorn Graph Engine Prototype Server

This is the backend service for the graph engine prototype.

It now keeps parsed `.bdamage.txt` datasets in memory on the server side, so the
API can serve cached dataset models instead of reparsing files for every client
request.

It also now supports loading taxonomy on the backend from `nodes.dmp` and
optionally `names.dmp`, and can build the induced tree server-side.

## What it does

- `GET /ping`
- `POST /upload`
- `GET /datasets`
- `GET /render-data`
- `GET /model/status`
- `GET /taxonomy/status`
- `GET /tree-model`

## Phase 1 tree semantics

For the new remote tree mode, the backend is the authority for what the client
is allowed to render.

"API contract" here simply means:

- what parameters the client sends to the backend
- what JSON fields the backend returns
- what those fields mean

For the visible tree, the current agreement is:

- the root node is always returned
- a node is returned if it survives the active filters and lies on the visible
  expanded frontier
- `child_count` means the number of children that survive the current filters,
  not the raw taxonomy child count
- collapsed nodes still keep their subtree totals
- in remote mode, the returned tree is the source of truth for expanded state

Filter and selection behavior:

- changing selected dataset files resets expanded state
- changing taxonomy files resets expanded state
- changing `min_reads` keeps the current expansion request, but the backend may
  prune children that no longer pass the filter

This is important because it defines what the client is allowed to cache later.
We do not want to cache behavior that is still ambiguous.

## Current server-side model

The backend currently owns:

- uploaded `.bdamage.txt` files
- parsed per-dataset direct taxon counts
- per-dataset taxon-name mappings found in the files
- cached selections across one or more datasets
- parsed taxonomy nodes
- parsed taxonomy names
- cached server-side induced tree models

The current frontend is still on the older rendering path: even in remote mode,
it still fetches full dataset payloads and builds the visible tree in the
browser. The new backend tree model is in place for the next client transition.

## Endpoint notes

- `GET /datasets`
  Returns metadata for available uploaded `.bdamage.txt` files.

- `GET /render-data?files=a&files=b`
  Returns the parsed dataset payloads needed by the current client. This stays
  compatible with the existing frontend while using the server cache internally.

- `GET /model/status?files=a&files=b`
  Returns a backend-facing summary of the in-memory selection model, including
  total reads, total taxon rows, aggregated direct taxa, taxonomy status, tree
  summary, and cache status.

- `GET /taxonomy/status`
  Returns backend taxonomy loading status, including file metadata and numbers
  of parsed nodes and names.

- `GET /tree-model?files=a&files=b`
  Returns the induced taxonomy tree built on the backend for the requested
  datasets.

- `GET /root-view?files=a&files=b`
  Returns the currently visible tree slice for the requested dataset selection
  and filters. By default this is the root and its direct visible children.

- `GET /expand-node?taxid=123&files=a&files=b`
  Returns the updated visible tree slice after expanding the requested node
  under the current selection and filters.

- `GET /node-tooltip?taxid=123&files=a&files=b`
  Returns the tooltip payload for one node under the same dataset selection,
  taxonomy files, and `min_reads` filter as the tree view. This includes node
  identity, direct and subtree counts, filtered child count, lineage, and
  per-dataset direct/subtree breakdown.

- `GET /table-view?scope=root&files=a&files=b`
  Returns table rows for the current root or for one requested subtree under the
  same dataset selection, taxonomy files, and `min_reads` filter as the tree
  view. Rows can currently be sorted by `direct` or `subtree`.

## What these new endpoints are for

- `/node-tooltip`
  This answers: "for this one node, what should the hover panel show under the
  current tree context?"

- `/table-view`
  This answers: "for this root or subtree, what ranked rows should the count
  table show under the current tree context?"

They are meant to use the same selection and filter context as the visible tree
API, so the client does not have to recompute those derived views locally.

## Error behavior

The newer endpoints now return explicit error payloads with a short `code` and
human-readable `message`.

Examples:

- `node_not_in_active_tree`
  The requested taxid does not exist in the current induced tree.

- `node_filtered_out`
  The requested taxid exists in the current induced tree, but does not pass the
  current `min_reads` threshold.

- `invalid_scope`
  The `/table-view` scope is not one of the supported values.

- `invalid_sort`
  The `/table-view` sort mode is not one of the supported values.

- `missing_taxid`
  `/table-view` was asked for `scope=node` without providing a `taxid`.

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

And for the backend-built tree model:

```text
http://localhost:8000/tree-model
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
