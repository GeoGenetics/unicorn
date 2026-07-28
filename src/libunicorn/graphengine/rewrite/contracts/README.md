# Unicorn Agent V1 Contracts

Status: frozen for the V1 rewrite

This directory defines every provider-neutral boundary in the rewritten
Unicorn Agent workflow. Runtime code must validate or produce these shapes
without adding undocumented fields.

## Contract Inventory

| Boundary | Schema version | Schema |
| --- | --- | --- |
| Browser to backend | `unicorn_agent_turn_v1` | `browser_turn_request.schema.json` |
| Orchestrator to provider adapter | `unicorn_provider_input_v1` | `provider_adapter_input.schema.json` |
| Provider adapter to orchestrator | `unicorn_provider_response_v1` | `normalized_provider_response.schema.json` |
| Tool executor to orchestrator | `unicorn_tool_result_v1` | `tool_result.schema.json` |
| Backend to browser | `unicorn_agent_result_v1` | `browser_turn_response.schema.json` |
| Runtime stage to trace store | `unicorn_agent_trace_v1` | `trace_event.schema.json` |

The JSON Schemas are the machine-readable authorities. This document defines
ownership, sequencing, secret handling, and cross-contract semantics.

## End-to-End Ownership

The browser:

- owns Agent panel interaction and transcript rendering
- creates one browser turn request when the user sends a prompt
- sends graph identifiers and UI scope, not raw graph records
- renders one browser turn response

The backend orchestrator:

- validates the browser request
- rebuilds graph context from backend-authoritative state
- owns the tool loop and its iteration counter
- validates every normalized provider response
- validates and executes every Unicorn tool call
- emits the final browser response
- records ordered trace events

A provider adapter:

- consumes one provider adapter input
- maps it to one provider-native request
- performs provider transport
- preserves the raw provider response in the trace
- extracts provider output
- returns one normalized provider response

A provider never executes Unicorn tools and never mutates graph state.

## Browser Turn Semantics

The browser sends exactly one `unicorn_agent_turn_v1` object per user action.

- `prompt` is the current user prompt.
- `conversation` contains prior user and assistant messages only.
- The current `prompt` must not be duplicated in `conversation`.
- `graph_scope` contains identifiers and active UI interpretation settings.
- The backend must validate dataset and taxid identifiers against its active
  graph state before using them.
- `graph_scope` is not an authoritative graph snapshot.
- Provider-native fields, tool definitions, and tool results do not belong in
  this request.

## Provider Iteration Semantics

`iteration` is the number of completed tool calls whose results are available
to the provider.

- The initial provider adapter input uses `iteration: 0` and no tool results.
- A valid `tool_call` is executed once by Unicorn.
- Its tool result is appended to `tool_results`.
- The next provider input increments `iteration` by one.
- Tool results remain ordered by execution.
- Exactly one normalized provider response is accepted per provider request.
- Exactly one tool call may be requested by a normalized response.

V1 permits at most four tool executions:

- provider inputs may use iterations `0` through `4`
- a `tool_call` returned at iteration `4` is rejected
- rejection completes the turn with status `iteration_limit`
- this permits at most five provider requests and four tool executions

The iteration limit is backend policy. It is not accepted from the browser.

## Tool Definition Semantics

Every provider-visible tool definition contains:

- a stable dotted `tool_id`
- a concise `description`
- explicit `when_to_use` guidance
- an `output_summary` describing the result without executing the tool
- a strict JSON `arguments_schema`
- `mutation: false`

The Python registry additionally owns the implementation handler and the
authoritative mutation flag. Mutation tools are never included in provider
input and are rejected by default during dispatch.

## Normalized Provider Response Semantics

The response type determines whether the loop continues:

- `tool_call` is nonterminal and is the only type that continues the loop.
- `assistant_message` is a terminal answer produced without a tool summary.
- `final_answer` is a terminal answer produced after zero or more tool calls.
- `error` is terminal and completes the turn as failed.

Adapters must not return provider-native tool-call structures. V1 uses JSON-only
provider output and Unicorn-controlled tool execution.

## Tool Result Semantics

A tool result records the exact normalized `tool_id` and `arguments` accepted
by the executor.

- Successful results use `ok: true`, an object in `result`, and `error: null`.
- Failed results use `ok: false`, `result: null`, and a structured error.
- Tool failures are data and may be reinjected into the next provider input.
- Tool results never contain credentials or provider transport details.

## Browser Result Semantics

The backend returns exactly one `unicorn_agent_result_v1` object:

- `completed` carries a user-visible answer and no error
- `failed` carries a structured error and no answer
- `iteration_limit` carries an iteration-limit error and no answer
- `tools_used` is a concise execution summary, not a tool-result dump
- `trace_id` identifies the ordered backend trace for the turn

## Secret Handling

The provider API key is not part of any JSON contract in this directory.

For V1, the browser sends it in the transport-only HTTP header:

```http
X-Unicorn-Provider-API-Key: <secret>
```

The header is optional for providers such as an unauthenticated local vLLM
server.

The API key must never enter:

- browser turn JSON
- provider adapter input
- graph context
- conversation
- tool arguments or results
- normalized provider responses
- browser turn responses
- traces
- fixtures
- application or error logs

The request handler passes the key directly to the selected adapter through a
transient in-memory transport argument. Header logging must redact this header.

The provider `base_url` is configuration, not a credential, but it must still
be validated before provider transport is implemented.

## Trace Semantics

Trace events are append-only and ordered by `sequence`.

- `sequence` begins at `1` and increases by one for the complete turn.
- `iteration` follows the provider iteration semantics above.
- `elapsed_ms` is measured from receipt of the browser turn.
- `data` contains only data relevant to the named event.
- Raw provider responses are stored in `provider_http_completed`.
- Raw extracted output and sanitized parser input are stored separately in
  `provider_output_extracted`.
- Normalized provider-neutral output belongs in
  `provider_response_normalized`.
- Trace data must not contain API keys or authorization headers.

The expected V1 event order is:

1. `turn_received`
2. `turn_validated`
3. `context_built`
4. `iteration_started`
5. `tool_result_reinjected`, when a previous tool result exists
6. `provider_input_created`
7. `provider_request_mapped`
8. `provider_http_started`
9. `provider_http_completed`
10. `provider_output_extracted`
11. `provider_response_normalized`
12. `tool_call_validated`, when requested
13. `tool_execution_started`, when requested
14. `tool_execution_completed`, when requested
15. `iteration_completed`
16. Repeat iteration events when a tool result is reinjected
17. `iteration_limit_reached`, when applicable
18. `turn_completed` or `turn_failed`

Events may be omitted only when their stage was never reached. Sequence values
must remain contiguous across iterations.

## Version Compatibility

- V1 producers emit exactly the version declared by their schema.
- V1 consumers reject missing or unknown `schema_version` values.
- Additional properties are rejected at provider-neutral boundaries.
- A breaking field or semantic change requires a new schema version.
- Provider-native request and response envelopes are adapter internals and do
  not change these provider-neutral schema versions.
