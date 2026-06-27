# Graphengine Regression Checklist

Run this checklist after any non-trivial graphengine change.


## Setup

1. Start the graphengine backend.
2. Open the UI through a local web server, not by opening `index.html` directly.
3. Connect to the backend.
4. Ensure at least one `.bdamage.txt` dataset is available.
5. Ensure `nodes.dmp` is available.
6. Prefer having `names.dmp` available as well.


## Core Render Flow

1. Render a backend-backed tree.
2. Confirm the top summary updates.
3. Confirm the SVG tree is visible and interactive.
4. Confirm the client does not freeze.


## Taxonomy Readiness

1. Verify rendering is blocked if taxonomy is missing.
2. Verify the UI explains which taxonomy file is missing.
3. Verify `names.dmp` missing state is communicated clearly if applicable.


## Selection Flow

1. Select one node with the platform selection gesture.
2. Select multiple nodes.
3. Confirm selected count in the summary updates correctly.
4. Clear selection and confirm the count returns to zero.


## Rank Selection

1. Open the rank dropdown.
2. Confirm it shows meaningful available ranks.
3. Select a rank and run `Select To Rank`.
4. Confirm the resulting selection count is plausible.


## Expansion Flow

1. Collapse a node.
2. Expand a node again.
3. Use `Uncollapse` on a selected node.
4. Use `Uncollapse Tips` on a selected node.
5. Confirm the tree remains responsive after each action.


## Dataset Flow

1. Toggle dataset visibility from the legend.
2. Confirm the tree redraws.
3. Select or unselect backend datasets.
4. Confirm the tree refreshes or clears appropriately.


## Table And Report Flow

1. Open the top table.
2. Confirm rows load.
3. Select multiple nodes and open `Count Matrix`.
4. Confirm the matrix reflects more than one selected node when expected.
5. Select multiple nodes and open `Rank Report`.
6. Confirm the report opens without collapsing the selection unexpectedly.


## Filter Flow

1. Change `min_reads`.
2. Change the `min_reads` scale mode.
3. Change the `min_reads` max.
4. Confirm the tree redraws and reports remain coherent.


## Count View Flow

1. Switch between direct, cumulative, and combined count views.
2. Confirm node labels and report views stay consistent.
3. If a report is open, confirm it rerenders or stays valid.


## Agent Safety Smoke Test

1. Open the agent panel.
2. Confirm it remains usable after a tree render.
3. If provider functionality is in scope, confirm the request payload still builds.


## Failure Signals

If any item fails, capture:

1. the exact UI action
2. the exact status message
3. the client log entry if present
4. whether the issue affects:
   render
   selection
   reports
   backend context
   agent state


## Minimum Standard

Do not consider a graphengine change complete until:

1. the intended feature works
2. the core render flow passes
3. selection flow passes
4. table and report flow passes
5. no new contradictory UI state appears
