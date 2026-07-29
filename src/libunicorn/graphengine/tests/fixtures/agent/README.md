# Unicorn Agent Provider Fixtures

These fixtures preserve the provider-specific response shapes understood by the
pre-rewrite Agent runtime. They are static parser evidence: tests using them
must not contact OpenAI, Google, or a local vLLM server.

## Fixture Contract

Every fixture contains:

- `description`: behavior represented by the fixture
- `provenance`: where the evidence came from and how faithfully it was captured
- `provider_request`: sanitized provider-native request body
- `raw_provider_response`: complete response envelope presented to the parser
- `expected_extracted_text`: text expected after provider-envelope extraction
  and any provider-specific cleanup
- `expected_normalized_response`: Unicorn's V1 normalized turn result, or `null`
- `expected_error`: expected parser failure, or `null`

All fixtures conform to `fixture.schema.json`.

## Provenance Levels

- `historical_log_reconstructed_envelope`: the normalized provider result is
  preserved in a historical Unicorn log, but the provider-native envelope was
  not retained. The envelope follows the old parser's accepted native shape.
- `conversation_capture_reconstructed_envelope`: model output text was captured
  during interactive debugging. The surrounding OpenAI-compatible chat
  completion envelope follows the old parser's accepted native shape.
- `contract_derived`: no live successful response survived. The fixture records
  an explicit behavior of the old request builder and parser and must not be
  described as a live provider capture.

Reconstructed fixtures are deliberately labeled. If a matching sanitized live
capture becomes available, replace the reconstructed envelope while preserving
the expected parser behavior.

Provider-native response envelopes preserve old-runtime evidence, while
successful `expected_normalized_response` objects use the frozen
`unicorn_provider_response_v1` contract under `unicorn_agent/schemas/`.

## Fixture Inventory

| Provider | Fixture | Purpose |
| --- | --- | --- |
| OpenAI | `openai/final_answer.json` | Responses API final answer |
| OpenAI | `openai/tool_call.json` | Responses API Unicorn tool call |
| OpenAI | `openai/malformed_output.json` | Non-JSON output failure |
| Google | `google/final_answer.json` | Interactions API final answer |
| Google | `google/tool_call.json` | Historical Unicorn tool call |
| Google | `google/malformed_output.json` | Non-JSON output failure |
| Local | `local_openai_compat/final_answer.json` | Chat completion final answer |
| Local | `local_openai_compat/think_and_tool_call.json` | Reasoning removal followed by tool call |
| Local | `local_openai_compat/fenced_json.json` | Fenced JSON extraction |
| Local | `local_openai_compat/compatibility_tool_input.json` | Legacy `tool`/`input` normalization |
| Local | `local_openai_compat/malformed_output.json` | Unrecoverable prose failure |

## Sanitization Rules

- Never add API keys or authorization headers.
- Use example dataset and model identifiers where originals are private.
- Preserve raw `<think>...</think>` text when it is the behavior under test.
- Keep response envelopes structurally complete for the parser path.
- Do not silently convert reconstructed evidence into claimed live captures.
