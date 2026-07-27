"""Pure local OpenAI-compatible request mapping and response parsing."""

from __future__ import annotations

import json
import re
from collections.abc import Mapping
from typing import Any

from unicorn_agent.contracts import validate_provider_adapter_input
from unicorn_agent.providers.base import ProviderAdapterError, parse_normalized_output


_THINK_BLOCK = re.compile(r"<think>.*?</think>", re.DOTALL | re.IGNORECASE)
_JSON_FENCE = re.compile(
    r"```(?:json)?[ \t]*\r?\n?(.*?)```",
    re.DOTALL | re.IGNORECASE,
)


def map_request(provider_input: Mapping[str, Any]) -> dict[str, Any]:
    validate_provider_adapter_input(provider_input)
    messages = [
        {
            "role": "system",
            "content": provider_input["instructions"],
        }
    ]
    messages.extend(
        {
            "role": item["role"],
            "content": item["content"],
        }
        for item in provider_input["conversation"]
    )
    messages.append(
        {
            "role": "user",
            "content": _serialize_provider_input(provider_input),
        }
    )
    return {
        "model": provider_input["model"],
        "messages": messages,
        "temperature": 0.0,
        "response_format": {
            "type": "json_object",
        },
    }


def extract_raw_text(raw_response: Mapping[str, Any]) -> str:
    try:
        content = raw_response["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        content = None
    if isinstance(content, str):
        return content
    raise ProviderAdapterError(
        code="local_openai_compat_invalid_response",
        message="Local OpenAI-compatible response did not contain output text.",
    )


def sanitize_output_text(raw_text: str) -> str:
    text = _THINK_BLOCK.sub("", raw_text).strip()

    fenced_blocks = _JSON_FENCE.findall(text)
    if fenced_blocks:
        return fenced_blocks[-1].strip()

    extracted = _extract_first_json_object(text)
    return extracted if extracted is not None else text


def parse_response(raw_response: Mapping[str, Any]) -> dict[str, Any]:
    raw_text = extract_raw_text(raw_response)
    return parse_normalized_output(
        sanitize_output_text(raw_text),
        provider="local_openai_compat",
    )


def _extract_first_json_object(text: str) -> str | None:
    decoder = json.JSONDecoder()
    for index, character in enumerate(text):
        if character != "{":
            continue
        try:
            value, end = decoder.raw_decode(text[index:])
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            return text[index : index + end]
    return None


def _serialize_provider_input(provider_input: Mapping[str, Any]) -> str:
    payload = {
        "graph_context": provider_input["graph_context"],
        "tools": provider_input["tools"],
        "tool_results": provider_input["tool_results"],
        "conversation": provider_input["conversation"],
        "user_prompt": provider_input["user_prompt"],
    }
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=True)
