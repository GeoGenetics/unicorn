"""Pure Google Gemini request mapping and response parsing."""

from __future__ import annotations

import json
from collections.abc import Mapping
from typing import Any

from unicorn_agent.contracts import validate_provider_adapter_input
from unicorn_agent.providers.base import ProviderAdapterError, parse_normalized_output


def map_request(provider_input: Mapping[str, Any]) -> dict[str, Any]:
    validate_provider_adapter_input(provider_input)
    return {
        "model": provider_input["model"],
        "system_instruction": provider_input["instructions"],
        "input": _serialize_provider_input(provider_input),
        "response_format": {
            "type": "text",
            "mime_type": "application/json",
            "schema": {
                "type": "object",
            },
        },
    }


def extract_raw_text(raw_response: Mapping[str, Any]) -> str:
    output_text = raw_response.get("output_text")
    if isinstance(output_text, str):
        return output_text
    raise ProviderAdapterError(
        code="google_invalid_response",
        message="Google response did not contain output text.",
    )


def sanitize_output_text(raw_text: str) -> str:
    return raw_text.strip()


def parse_response(raw_response: Mapping[str, Any]) -> dict[str, Any]:
    raw_text = extract_raw_text(raw_response)
    return parse_normalized_output(
        sanitize_output_text(raw_text),
        provider="google",
    )


def _serialize_provider_input(provider_input: Mapping[str, Any]) -> str:
    payload = {
        "graph_context": provider_input["graph_context"],
        "tools": provider_input["tools"],
        "tool_results": provider_input["tool_results"],
        "conversation": provider_input["conversation"],
        "user_prompt": provider_input["user_prompt"],
    }
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=True)
