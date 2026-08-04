# Unicorn Graph Engine Backend Contract

This file is the single live contract for the graph engine backend as it exists
today.

The goal is not to preserve prototype history. The goal is to define the
current backend-owned semantics that the UI and future agentic workflow depend
on.


## Operating invariant

Graphengine now has one intended operating model:

- the UI requires a backend connection
- the backend is the authoritative source for datasets, taxonomy, tree slices,
  and analysis payloads
- there is no separate intended in-browser local dataset/tree/report mode
- if a user is working on one machine, they should run the backend locally and
  connect the UI to `localhost`

"Local" is therefore a deployment detail, not a second semantic mode.


## Role of the backend

In graphengine operation, the backend is the source of truth for:

- uploaded `.bdamage.txt` datasets
- dataset selection and aggregate counts
- taxonomy loading from `nodes.dmp` and optional `names.dmp`
- induced tree construction for the active dataset selection
- visible-tree slicing under the active expansion state and `min_reads` filter
- node tooltip payloads
- ranked table payloads
- subtree reports
- rank reports

The browser still renders the graph and owns transient UI state, but the data
contract comes from the backend.

## Runtime storage

Runtime uploads, taxonomy files, metadata, and Agent traces do not belong in
tracked source. The default runtime root is:

```text
<repository>/var/graphengine/
```

Its default layout is:

```text
var/graphengine/
├── uploads/
└── logs/
    └── agent_trace/
```

Configuration precedence:

- `UNICORN_GRAPHENGINE_RUNTIME_DIR` relocates the complete runtime tree.
- `UNICORN_GRAPHENGINE_UPLOAD_DIR` overrides only `uploads/`.
- `UNICORN_GRAPHENGINE_AGENT_TRACE_DIR` overrides only Agent trace storage.
- `UNICORN_GRAPHENGINE_NODES_FILE` and
  `UNICORN_GRAPHENGINE_NAMES_FILE` select taxonomy filenames or explicit
  paths.

The upload directory is created by `unicorn_backend.app.create_app()`.
`server_app.py` only re-exports the production application for compatibility.
Tests inject temporary runtime configuration when constructing isolated apps.


## Shared request context

The newer graph endpoints all operate under the same logical context:

- `files`
  - repeated query parameter
  - selected dataset filenames
  - if omitted, the backend uses all available uploaded datasets
- `nodes_file`
  - taxonomy nodes filename
- `names_file`
  - taxonomy names filename
- `min_reads`
  - subtree-read threshold used for filtering visible children and node access

Tree-slice endpoints also use:

- `expanded`
  - repeated query parameter of taxids requested as expanded in remote tree mode

The backend echoes the effective context in `request_context` where relevant:

- `dataset_names`
- `nodes_file`
- `names_file`
- `min_reads`
- `expanded_taxids`


## Visible-tree semantics

These semantics are the current contract for backend-served tree mode:

- the root is always returned
- a node is visible when it survives the active `min_reads` filter and lies on
  the requested expanded frontier
- `child_count` is the number of children that survive the current filter, not
  the raw taxonomy child count
- collapsed nodes still carry subtree totals
- dataset or taxonomy changes invalidate prior expansion state on the client
- changing `min_reads` keeps the client expansion request, but the backend may
  prune children that no longer pass the filter
- the backend returns `expanded_taxids` as the effective expanded state the
  client should treat as authoritative


## Endpoint surface

### Utility and compatibility endpoints

- `GET /ping`
  - service health plus upload/taxonomy/cache summary
- `POST /upload`
  - stores one uploaded dataset file
- `GET /datasets`
  - lists available uploaded datasets
- `GET /taxonomy/status`
  - loads or reports backend taxonomy state
- `GET /model/status`
  - backend-oriented summary of current dataset selection, taxonomy, tree, and
    cache state
- `GET /render-data`
  - compatibility endpoint for the older client path that still wants parsed
    dataset payloads
- `GET /tree-model`
  - returns the full induced tree model for the current selection

These endpoints still exist, but they are not the main contract for the current
backend-driven tree flow.

### Remote graph endpoints

#### `GET /root-view`

Purpose:
- returns the visible tree slice for the current selection, taxonomy, filter,
  and expansion state

Query parameters:
- `files`
- `nodes_file`
- `names_file`
- `min_reads`
- `expanded`

Response fields:
- `ok`
- `datasets`
- `taxonomy`
- `tree`
- `missing_taxids`
- `expanded_taxids`
- `min_reads`
- `total_reads`
- `direct_taxa`
- `request_context`
- `cache`

Notes:
- this is the base entry point for remote tree rendering
- with no effective expansions, this is the root plus its visible frontier

#### `GET /expand-node`

Purpose:
- returns the updated visible tree slice after requesting one node expansion

Query parameters:
- `taxid` required
- `files`
- `nodes_file`
- `names_file`
- `min_reads`
- `expanded`

Response fields:
- same envelope as `/root-view`

Notes:
- the backend adds the requested `taxid` to the incoming expanded set
- the response `expanded_taxids` is the canonical post-expansion state

#### `GET /node-tooltip`

Purpose:
- returns one node detail payload under the active tree context

Query parameters:
- `taxid` required
- `files`
- `nodes_file`
- `names_file`
- `min_reads`

Response fields:
- `ok`
- `node`
- `request_context`

`node` payload:
- `taxid`
- `name`
- `rank`
- `parent`
- `depth`
- `direct`
- `subtree`
- `child_count`
- `lineage`
- `datasets`

Notes:
- `datasets` is a per-dataset direct/subtree breakdown
- this is the remote source for graph hover and detailed node inspection

#### `POST /damage/node`

Purpose:
- returns one backend-authoritative damage profile comparison for a taxid
  across selected datasets

JSON request:
- `taxid` required
- `files` required non-empty dataset filename list
- `nodes_file`
- `names_file`

Response fields:
- `ok`
- `taxid`
- `name`
- `direct_count_scope`
- `subtree_count_scope`
- `damage_scope`
- `datasets`
- `request_context`

Per-dataset fields:
- `dataset`
- `direct_count`
- `subtree_count`
- `profile_present`
- `profile_status`
  - `valid`, `invalid`, or `missing`
- `fit_valid`
- `missing_fields`
- `observed`
- `fit`

Notes:
- `direct_count` is exact-LCA assignment to the row taxid
- `subtree_count`, observed damage, and fitted damage apply to the row taxid
  plus represented descendant evidence
- profiles and fitted parameters remain separate per dataset
- missing profiles are represented explicitly and never converted to zeros
- non-finite producer values are returned as JSON `null`
- raw `direct_mmm_base64` values are validated during ingestion but are not
  retained or returned by this endpoint
- dataset summaries expose only compact damage capability counts; tree,
  report, and render payloads never include the full profile map

#### `POST /damage/selected`

Purpose:
- returns a compact backend-authoritative damage comparison for multiple
  selected taxids
- resolves the active datasets and taxonomy once for the complete selection

JSON request:
- `taxids` required unique list containing 1 to 250 positive taxids
- `files` required non-empty dataset filename list
- `nodes_file`
- `names_file`

Response fields:
- `ok`
- `direct_count_scope`
- `subtree_count_scope`
- `damage_scope`
- `nodes`
- `request_context`

Each `nodes` entry uses the same `taxid`, `name`, scope, and per-dataset
profile fields returned by `POST /damage/node`. Repeated request context is
omitted from individual nodes.

Notes:
- the endpoint is intended for compact selected-node tables and normalized
  export
- it does not aggregate datasets, metadata groups, or fitted parameters
- selections larger than 250 taxids must be narrowed before requesting damage
- requests larger than 5,000 taxon-by-dataset profiles are rejected with
  `damage_selection_too_large`

#### `GET /table-view`

Purpose:
- returns ranked rows for the current root scope or one subtree scope

Query parameters:
- `scope`
  - `root` or `node`
- `taxid`
  - required when `scope=node`
- `files`
- `nodes_file`
- `names_file`
- `min_reads`
- `sort`
  - `direct` or `subtree`
- `limit`
  - `1..1000`

Response fields:
- `ok`
- `scope`
- `target`
- `sort`
- `limit`
- `row_count`
- `rows`
- `request_context`

`target` payload:
- `taxid`
- `name`
- `rank`
- `direct`
- `subtree`
- `child_count`

Notes:
- rows are already ranked backend-side
- the target node must exist in the active induced tree and pass `min_reads`

#### `GET /subtree-report`

Purpose:
- returns a subtree-focused analysis payload for one taxon

Query parameters:
- `taxid` required
- `files`
- `nodes_file`
- `names_file`
- `min_reads`
- `descendant_limit`
- `matrix_limit`

Response fields:
- `ok`
- `report`
- `request_context`

`report` payload:
- `target`
- `per_dataset_summary`
- `matrix`

Notes:
- `per_dataset_summary.rows` gives direct-read counts for the target by dataset
- `matrix.rows` gives top child rows plus direct counts split by dataset

#### `GET /rank-report`

Purpose:
- returns a rank-collapsed summary across one or more requested taxids

Query parameters:
- `taxids`
  - repeated query parameter
- `files`
- `nodes_file`
- `names_file`
- `min_reads`

Response fields:
- `ok`
- `report`
- `request_context`

`report` payload:
- `summary`
- `rows`

`summary` fields:
- `selected_taxids`
- `selected_node_count`
- `dataset_names`
- `total_direct`


## Error contract

These endpoints return structured FastAPI error payloads in `detail`:

- `message`
- `code`
- optional `request_context`
- optional endpoint-specific fields such as `taxid`, `scope`, `sort`,
  `node_subtree_reads`, or `min_reads`

Current error codes include:

- `node_not_in_active_tree`
- `node_filtered_out`
- `invalid_scope`
- `invalid_sort`
- `missing_taxid`
- `missing_taxids`

Important distinction:

- `404 node_not_in_active_tree`
  - the requested node is absent from the active induced tree
- `409 node_filtered_out`
  - the node exists in the active induced tree, but the current `min_reads`
    threshold filters it out


## Client expectations

The frontend should treat these as the current backend-required rules:

- do not recompute backend-derived tree visibility locally
- treat `expanded_taxids` from `/root-view` and `/expand-node` as authoritative
- carry the same dataset/taxonomy/filter context across tree, tooltip, table,
  subtree report, and rank report requests


## Agent backend

The Agent runtime is backend-owned and read-only.

The browser Agent panel collects:

- provider, model, optional base URL, and transient API key
- the current prompt
- a bounded conversation slice
- selected datasets and taxonomy filenames
- `min_reads`, count mode, selected taxids, and focused taxid

It sends exactly one request:

```text
POST /agent/turn
```

The backend validates scope, rebuilds compact graph context, selects a provider
adapter, runs the tool loop, and returns one terminal response.

### Turn request

Minimal local-provider example:

```bash
curl -sS \
  -H 'Content-Type: application/json' \
  -X POST http://localhost:8000/agent/turn \
  --data '{
    "schema_version": "unicorn_agent_turn_v1",
    "turn_id": "turn_manual_001",
    "session_id": "session_manual_001",
    "prompt": "What min_reads threshold am I using?",
    "provider": {
      "name": "local_openai_compat",
      "model": "deepseek-ai/DeepSeek-R1-Distill-Qwen-14B",
      "base_url": "http://localhost:8542"
    },
    "graph_scope": {
      "datasets": ["sample_a.bdamage.txt"],
      "nodes_file": "nodes.dmp",
      "names_file": "names.dmp",
      "min_reads": 1000,
      "count_mode": "subtree",
      "selected_taxids": [],
      "focused_taxid": null
    },
    "conversation": []
  }' | jq .
```

The dataset and taxonomy filenames must exist in the backend upload scope.

Hosted providers receive the key through the transport-only header:

```http
X-Unicorn-Provider-API-Key: <secret>
```

Example:

```bash
curl -sS \
  -H 'Content-Type: application/json' \
  -H "X-Unicorn-Provider-API-Key: ${PROVIDER_API_KEY}" \
  -X POST http://localhost:8000/agent/turn \
  --data @turn.json | jq .
```

The API key is excluded from request JSON, graph context, provider-input
traces, tool data, responses, and logs.

### Provider targets

Supported provider names:

- `local_openai_compat`
  - OpenAI-compatible `/v1/chat/completions`
  - API key optional
- `google`
  - Gemini Interactions API
  - API key required
- `openai`
  - OpenAI Responses API
  - API key required

`base_url` is interpreted by the backend machine. For example,
`http://localhost:8542` means port 8542 on the machine running Graphengine's
backend, not on the browser machine.

### Turn response

The route returns `unicorn_agent_result_v1`.

Terminal statuses:

- `completed`
- `failed`
- `iteration_limit`

The response contains:

- `turn_id`
- user-visible `answer`, or structured `error`
- concise `tools_used`
- `trace_id`

### Trace storage

Every accepted turn writes ordered JSONL under:

```text
<repository>/var/graphengine/logs/agent_trace/YYYY-MM-DD/<turn_id>.jsonl
```

The default trace root is derived from the repository runtime directory, not
the server's current working directory.

Override it with:

```bash
export UNICORN_GRAPHENGINE_AGENT_TRACE_DIR=/path/to/agent_trace
```

Each line is one event. List the complete flow with:

```bash
jq -r '.event' \
  /path/to/unicorn/var/graphengine/logs/agent_trace/YYYY-MM-DD/<turn_id>.jsonl
```

Inspect sequence, iteration, event, and elapsed time:

```bash
jq -c \
  '{sequence, iteration, event, elapsed_ms}' \
  /path/to/unicorn/var/graphengine/logs/agent_trace/YYYY-MM-DD/<turn_id>.jsonl
```

Inspect provider boundaries:

```bash
jq -c \
  'select(.event | startswith("provider_")) | {sequence, event, data}' \
  /path/to/unicorn/var/graphengine/logs/agent_trace/YYYY-MM-DD/<turn_id>.jsonl
```

Inspect tool calls and results:

```bash
jq -c \
  'select(.event | startswith("tool_")) | {sequence, event, data}' \
  /path/to/unicorn/var/graphengine/logs/agent_trace/YYYY-MM-DD/<turn_id>.jsonl
```

A connection failure should end with:

```text
provider_http_started
provider_http_failed
iteration_completed
turn_failed
```

Provider responses preserve both:

- raw provider output
- sanitized parser input

This makes reasoning-block removal, fenced JSON extraction, normalization, and
tool-loop decisions reconstructable from one trace.

### Trace retrieval

View one trace inline:

```text
GET /agent/traces/{trace_id}
```

Download it as an attachment:

```text
GET /agent/traces/{trace_id}?download=true
```

The Agent panel exposes both operations for the latest turn.

### Contract and runtime references

Machine-readable contracts:

```text
unicorn_agent/schemas/
```

Human-readable flow:

```text
docs/agent/provider_turn_contract.md
```

Compact context:

```text
docs/agent/compact_context.md
```

Tool registry:

```text
docs/agent/tool_registry.md
```
