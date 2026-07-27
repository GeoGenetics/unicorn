"""Pure OpenAI Responses API request mapping and response parsing."""

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
        "instructions": provider_input["instructions"],
        "input": [
            {
                "role": "user",
                "content": [
                    {
                        "type": "input_text",
                        "text": _serialize_provider_input(provider_input),
                    }
                ],
            }
        ],
        "text": {
            "format": {
                "type": "json_schema",
                "name": "unicorn_provider_response",
                "strict": False,
                "schema": {
                    "type": "object",
                },
            }
        },
    }


def extract_raw_text(raw_response: Mapping[str, Any]) -> str:
    try:
        output = raw_response["output"]
        for item in output:
            for content in item.get("content", []):
                text = content.get("text")
                if isinstance(text, str):
                    return text
    except (KeyError, TypeError):
        pass
    raise ProviderAdapterError(
        code="openai_invalid_response",
        message="OpenAI response did not contain output text.",
    )


def sanitize_output_text(raw_text: str) -> str:
    return raw_text.strip()


def parse_response(raw_response: Mapping[str, Any]) -> dict[str, Any]:
    raw_text = extract_raw_text(raw_response)
    return parse_normalized_output(
        sanitize_output_text(raw_text),
        provider="openai",
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
