Unicorn Compact Provider Context
================================

Status and Authority
--------------------

This document describes the compact context produced by
`unicorn_agent/context.py`.

The machine-readable authority is the `graph_context` definition in:

```text
unicorn_agent/schemas/provider_adapter_input.schema.json
```

This is a live runtime shape, not a future proposal.


Purpose
-------

The compact context gives a provider enough orientation to:

- answer simple session questions directly
- understand the broad active graph scope
- choose an appropriate Unicorn tool for detailed questions

It does not attempt to answer detailed graph, metadata, dataset, or report
questions by itself.


Backend Construction
--------------------

The browser request carries a requested `graph_scope`. The backend:

1. validates the complete turn request
2. resolves dataset filenames through `GraphEngineStore`
3. loads and verifies taxonomy filenames
4. builds the active selection and tree
5. applies `min_reads`
6. verifies selected and focused taxids against threshold-visible nodes
7. emits the compact context below

Browser values describe user intent. Backend-derived context is authoritative.


Exact V1 Shape
--------------

```json
{
  "session": {
    "backend_connected": true
  },
  "datasets": {
    "selected_count": 3
  },
  "tree": {
    "visible_node_count": 537,
    "selected_node_count": 17,
    "visible_root": {
      "taxid": 1,
      "name": "root",
      "rank": "no rank"
    },
    "focused_node": null
  },
  "filters": {
    "min_reads": 2000,
    "count_mode": "subtree"
  },
  "metadata": {
    "active_field": null
  },
  "report_state": {
    "active": false,
    "mode": null
  }
}
```

All six top-level sections are required. Additional properties are rejected.


Field Semantics
---------------

`session.backend_connected`

- Confirms that context was constructed through the backend runtime.

`datasets.selected_count`

- Number of datasets successfully resolved in the active backend selection.
- Dataset names and per-dataset totals belong behind `datasets.selected`.

`tree.visible_node_count`

- Number of nodes surviving the current `min_reads` threshold.

`tree.selected_node_count`

- Number of requested selected taxids verified as visible.
- The selected taxid list and node details belong behind `nodes.selected`.

`tree.visible_root`

- Compact `taxid`, `name`, and `rank` summary.
- Null only when no dataset-backed tree is active.

`tree.focused_node`

- Compact verified focus summary, or null.

`filters.min_reads`

- Backend visibility threshold for the current graph scope.

`filters.count_mode`

- `direct` or `subtree`.
- Determines count interpretation for applicable tools.

`metadata.active_field`

- Reserved compact metadata-visualization field.
- The current V1 context builder emits null.
- Null does not mean metadata is unavailable.
- Field discovery belongs behind `metadata.summary`.

`report_state`

- Reserved compact report activity and mode summary.
- The current V1 context builder emits `active: false` and `mode: null`.
- Detailed report rows never belong in initial context.


Deliberately Excluded
---------------------

The default provider context does not contain:

- dataset filenames or complete dataset records
- raw visible-node arrays
- selected taxid arrays
- expanded or collapsed taxid arrays
- taxonomy tables
- raw metadata rows
- per-dataset metadata dictionaries
- node lineages and per-dataset node counts
- count matrices or ranked report rows
- provider credentials
- browser DOM or rendering state

Those details are available only through explicit read-only tools when
relevant.


Direct Answers and Tool Escalation
----------------------------------

The context can directly answer questions such as:

- What `min_reads` threshold is active?
- How many datasets are selected?
- How many nodes are visible?
- How many nodes are selected?
- What count mode is active?

The provider should use tools for questions such as:

- Which datasets are selected? -> `datasets.selected`
- Which nodes are selected? -> `nodes.selected`
- Tell me about a named node. -> `nodes.find_visible`, then `node.details`
- Which metadata fields exist? -> `metadata.summary`
- How do metadata groups compare across selected nodes? ->
  `metadata.compare_selected`
- Show ranked rows. -> `table.view`


Empty-Scope Behavior
--------------------

With no selected datasets, context remains valid and reports:

- `datasets.selected_count: 0`
- zero visible and selected nodes
- null root and focus
- the requested filter interpretation

Selected or focused taxids without active datasets are rejected rather than
represented as valid context.


Validation Failures
-------------------

Backend context construction returns structured failures for conditions such
as:

- unavailable datasets
- unavailable or mismatched taxonomy files
- invalid backend filenames
- selected or focused taxids outside the visible scope
- taxids supplied without active datasets
- backend graph construction failure

Context construction never silently accepts an unverified graph scope.
