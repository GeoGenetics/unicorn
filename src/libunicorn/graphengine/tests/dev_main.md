# Graphengine Development Protocol

This document defines the default way to change Unicorn graphengine without
introducing hard-to-localize regressions.


## Purpose

Graphengine currently concentrates several concerns in one runtime:

- backend connection and request context
- dataset selection
- taxonomy readiness
- tree rendering
- selection semantics
- table and report views
- the backend-owned Agent request and trace boundary

Because these concerns are tightly coupled, one small change can affect several
behaviors at once. The goal of this protocol is to keep changes narrow, easy to
review, and easy to test.


## Core Rule

Every graphengine change must be:

1. small in scope
2. validated against the regression checklist
3. committed only after the intended workflow still works


## Required Workflow For Any Change

1. Define the single behavior being changed.
   Examples:
   - "fix rank dropdown population"
   - "change subtree report request payload"
   - "improve taxonomy missing-file messaging"

2. Identify the source of truth before editing.
   For every change, explicitly ask:
   - what object owns this state?
   - where is it derived?
   - does another path derive the same thing differently?

3. Keep the diff narrow.
   A single commit should ideally do one of:
   - UI copy or messaging
   - request payload logic
   - selection behavior
   - render behavior
   - report behavior
   - provider or backend Agent behavior

4. Avoid mixing permanent fixes with temporary debugging.
   If debug instrumentation is needed:
   - add it clearly
   - use it
   - remove it before finalizing unless it is intentionally retained

5. Run the graphengine regression checklist before considering the change done.

6. Review the diff before commit.
   Ask:
   - did I touch more than one behavior?
   - did I introduce a fallback that hides a real bug?
   - did I create a second source of truth?


## State Discipline

For any new change, prefer one canonical representation per concern:

- selected taxa:
  use one authoritative taxid set

- active backend request context:
  use one authoritative request-context object

- available ranks:
  derive them from one agreed path

- active report:
  use one authoritative report state and one refresh path

- visible tree:
  treat the backend payload as the semantic tree

Do not introduce parallel derived states unless there is a strong reason and it
is documented in code.


## Change Size Rules

Good graphengine commits:

- "Fix count matrix to send selected taxids directly"
- "Show taxonomy missing-file message when backend uploads are incomplete"
- "Refresh visible report when count-view mode changes"

Risky graphengine commits:

- rank dropdown changes plus selection semantics plus report refresh
- rendering changes plus new fallback behavior plus new backend fetches
- UI fixes plus invisible state repair logic

If a change feels broad, split it before merging it.


## Safe Debugging Rules

When a bug is unclear:

1. instrument first
2. reproduce second
3. fix third
4. remove temporary instrumentation last

Preferred debug aids:

- explicit status messages
- client log entries
- small assertions
- narrow helper functions for inspection

Avoid:

- silent catch-and-continue behavior unless absolutely necessary
- synthetic fallback state that hides missing data
- adding new asynchronous fetches without request-staleness guards


## Assertions To Prefer

Add lightweight sanity checks when touching fragile logic.

Examples:

- if `state.selected.size > 1`, report actions should send more than one taxid
- if backend tree is loaded, selection controls should not show contradictory UI
- if a rank dropdown is populated, it should come from a verified rank source
- if report refresh fails, the failure should be explicit in the UI or log


## Review Questions

Before finishing a graphengine change, answer these questions:

1. What exact behavior changed?
2. What state is now authoritative?
3. What regression would be most likely from this diff?
4. Which checklist items prove the change is safe?
5. Did the diff add a hidden fallback that could mask future bugs?


## Merge Standard

A graphengine change is ready only when:

- the code parses
- the intended behavior works
- the regression checklist passes
- the diff is still understandable in one reading

If any of these are missing, the change is not done yet.


## When A Regression Happens

Do not react by piling on more fixes immediately.

Instead:

1. stop feature work
2. isolate the smallest offending diff
3. identify the state transition that drifted
4. restore one source of truth
5. add or improve the checklist item that would have caught it earlier


## Long-Term Goal

This protocol is a bridge until graphengine is split into cleaner modules.

Until then, discipline is our architecture:

- smaller diffs
- explicit ownership of state
- repeatable workflow checks
- fewer hidden fallbacks
