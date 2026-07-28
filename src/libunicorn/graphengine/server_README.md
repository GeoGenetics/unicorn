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


## Agent backend status

The previous Agent routes, browser-local registry, and browser tool loop remain
removed. The backend replacement now exposes:

```text
POST /agent/turn
```

The route accepts `unicorn_agent_turn_v1` and returns
`unicorn_agent_result_v1`. Provider credentials travel only in:

```http
X-Unicorn-Provider-API-Key: <secret>
```

Supported backend adapters are:

- local OpenAI-compatible/vLLM
- Google Gemini Interactions
- OpenAI Responses

Every turn writes an ordered, secret-free trace under:

```text
logs/agent_trace/YYYY-MM-DD/<turn_id>.jsonl
```

The preserved browser Agent panel remains disabled until the Phase 11 client is
implemented. The backend route can be exercised directly in the meantime.
`/agent/provider-turn` and `/agent/client-payload` are not part of the current
contract.
