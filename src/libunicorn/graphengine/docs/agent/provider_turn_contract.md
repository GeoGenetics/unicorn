Unicorn Graph Engine Agent Turn Contract
========================================

Status
------

This is the live V1 Agent flow.

Machine-readable contract authority:

- `unicorn_agent/schemas/browser_turn_request.schema.json`
- `unicorn_agent/schemas/provider_adapter_input.schema.json`
- `unicorn_agent/schemas/normalized_provider_response.schema.json`
- `unicorn_agent/schemas/tool_result.schema.json`
- `unicorn_agent/schemas/browser_turn_response.schema.json`
- `unicorn_agent/schemas/trace_event.schema.json`

Runtime authority:

- `unicorn_agent/runtime.py`
- `unicorn_agent/orchestrator.py`
- `unicorn_agent/providers/`
- `unicorn_agent/routes.py`

If this guide and a JSON schema disagree, the JSON schema is authoritative.


Operating Invariants
--------------------

- Graphengine requires a backend connection.
- The backend rebuilds and verifies graph context from backend state.
- The browser sends one request to `POST /agent/turn`.
- The backend owns provider transport, tool execution, and the turn loop.
- Provider adapters never execute Unicorn tools.
- The Agent surface is read-only.
- Providers return JSON that normalizes into one Unicorn response shape.
- Provider credentials are transport-only and never enter contract payloads or
  traces.
- Every accepted turn receives one ordered persistent trace.


1. Browser Turn Request
-----------------------

The browser sends `unicorn_agent_turn_v1`:

```json
{
  "schema_version": "unicorn_agent_turn_v1",
  "turn_id": "turn_example_001",
  "session_id": "session_example_001",
  "prompt": "Tell me about Viridiplantae",
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
}
```

The browser describes requested scope; it does not provide trusted graph
facts. The backend verifies:

- dataset filenames against the backend store
- taxonomy filenames against backend-loaded taxonomy
- selected and focused taxids against the threshold-visible tree
- `min_reads` and count-mode interpretation

Conversation is a bounded user/assistant dialogue slice. It is not graph
evidence.


2. Credential Transport
-----------------------

An API key is sent only in:

```http
X-Unicorn-Provider-API-Key: <secret>
```

The header is optional for an unauthenticated local OpenAI-compatible server.
The key must not appear in:

- browser request JSON
- graph context
- provider adapter input
- provider-native payload traces
- tool arguments or results
- browser responses
- persistent logs


3. Backend Context Construction
-------------------------------

`BackendContextBuilder` transforms verified graph scope into the compact
provider context documented in `docs/agent/compact_context.md`.

The initial context is deliberately small. Detailed graph, dataset, metadata,
node, or table data must be obtained through the read-only tool registry.


4. Provider Adapter Input
-------------------------

For each iteration, the orchestrator creates
`unicorn_provider_input_v1`:

```json
{
  "schema_version": "unicorn_provider_input_v1",
  "turn_id": "turn_example_001",
  "iteration": 0,
  "model": "provider-model",
  "instructions": "Unicorn-owned grounding instructions",
  "graph_context": {},
  "tools": [],
  "tool_results": [],
  "conversation": [],
  "user_prompt": "Tell me about Viridiplantae"
}
```

`tools` contains the exact read-only definitions available for the turn.
`tool_results` is empty on iteration zero and contains validated Unicorn tool
results on later iterations.

The adapter boundary is:

```text
map_request(canonical_input) -> provider-native request
send_request(provider-native request) -> transport response
inspect_response(raw provider response) -> raw and sanitized output
parse_response(sanitized output) -> normalized response
```

Supported adapters:

- `local_openai_compat`: OpenAI-compatible `/v1/chat/completions`
- `google`: Gemini Interactions API
- `openai`: OpenAI Responses API

Provider-specific envelopes, endpoint rules, refusals, fenced JSON, reasoning
blocks, and compatibility forms remain inside the selected adapter.


5. Normalized Provider Response
-------------------------------

Every adapter returns exactly one `unicorn_provider_response_v1` object.

Final answer:

```json
{
  "schema_version": "unicorn_provider_response_v1",
  "type": "final_answer",
  "content": "Grounded answer."
}
```

Assistant message:

```json
{
  "schema_version": "unicorn_provider_response_v1",
  "type": "assistant_message",
  "content": "Grounded answer."
}
```

Tool call:

```json
{
  "schema_version": "unicorn_provider_response_v1",
  "type": "tool_call",
  "tool_id": "node.details",
  "arguments": {
    "taxid": 33090
  }
}
```

Provider error:

```json
{
  "schema_version": "unicorn_provider_response_v1",
  "type": "error",
  "code": "provider_refused",
  "message": "Provider refused this request."
}
```

The orchestrator never interprets provider-native response objects directly.


6. Tool Execution and Reinjection
---------------------------------

For a `tool_call`, the orchestrator:

1. rejects an unknown tool ID
2. rejects repeated tool ID and argument combinations
3. validates arguments against the registry JSON schema
4. executes the backend handler
5. validates `unicorn_tool_result_v1`
6. records the result in the trace
7. reinjects the result into the next provider iteration

Successful tool result:

```json
{
  "schema_version": "unicorn_tool_result_v1",
  "tool_id": "node.details",
  "arguments": {
    "taxid": 33090
  },
  "ok": true,
  "result": {},
  "error": null
}
```

Failed tool result:

```json
{
  "schema_version": "unicorn_tool_result_v1",
  "tool_id": "node.details",
  "arguments": {
    "taxid": 999999999
  },
  "ok": false,
  "result": null,
  "error": {
    "code": "node_not_found",
    "message": "The requested taxid is not available."
  }
}
```

The loop permits at most four tool executions. It stops on:

- `assistant_message`
- `final_answer`
- provider `error`
- unknown or invalid tool call
- repeated completed tool call
- provider or transport failure
- iteration limit


7. Browser Turn Response
------------------------

The backend returns one `unicorn_agent_result_v1` object.

Completed:

```json
{
  "schema_version": "unicorn_agent_result_v1",
  "turn_id": "turn_example_001",
  "status": "completed",
  "answer": "Viridiplantae is grounded by backend data.",
  "tools_used": [
    {
      "tool_id": "node.details",
      "ok": true
    }
  ],
  "error": null,
  "trace_id": "trace_example_001"
}
```

Failed turns use `status: "failed"`, `answer: null`, and a structured
`error`. Iteration exhaustion uses `status: "iteration_limit"`.

`tools_used` is a concise execution summary. Full arguments and results belong
in the trace.


8. Trace Contract
-----------------

Each accepted turn writes:

```text
<repository>/var/graphengine/logs/agent_trace/YYYY-MM-DD/<turn_id>.jsonl
```

`UNICORN_GRAPHENGINE_RUNTIME_DIR` relocates the runtime tree.
`UNICORN_GRAPHENGINE_AGENT_TRACE_DIR` overrides trace storage directly.

Every line is one `unicorn_agent_trace_v1` event with:

- stable turn and trace IDs
- iteration and contiguous sequence numbers
- timestamp and elapsed milliseconds
- event-specific data

Important boundaries include:

```text
turn_received
turn_validated
context_built
iteration_started
provider_input_created
provider_request_mapped
provider_http_started
provider_http_failed
provider_http_completed
provider_output_extracted
provider_response_normalized
tool_call_validated
tool_execution_started
tool_execution_completed
tool_result_reinjected
iteration_completed
turn_completed
turn_failed
```

`provider_http_completed` preserves the provider-native response.
`provider_output_extracted` stores raw model output separately from the exact
sanitized parser input. A transport failure uses `provider_http_failed`.

Trace retrieval:

```text
GET /agent/traces/{trace_id}
GET /agent/traces/{trace_id}?download=true
```


9. Ownership Summary
--------------------

Browser:

- provider/model/base URL/API-key controls
- prompt and bounded transcript
- requested graph scope
- one `POST /agent/turn`
- rendering result and trace controls

Backend:

- request validation
- authoritative context construction
- registry and tools
- provider selection and transport
- response parsing and normalization
- iteration state machine
- trace persistence

Provider:

- chooses one normalized response
- may request advertised tools
- never executes tools or mutates Unicorn state
