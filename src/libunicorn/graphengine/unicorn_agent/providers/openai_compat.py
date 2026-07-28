"""Local OpenAI-compatible/vLLM provider adapter."""

from __future__ import annotations

import json
import re
from collections.abc import Mapping
from typing import Any

from unicorn_agent.contracts import validate_provider_adapter_input
from unicorn_agent.providers.base import (
    ProviderAdapter,
    ProviderAdapterError,
    ProviderOutputInspection,
    ProviderTransportResponse,
    parse_normalized_output,
    post_json_transport,
    provider_instructions,
    raise_provider_api_error,
    serialize_provider_input,
    validate_http_endpoint,
)


_DEFAULT_ENDPOINT = "http://localhost:8542/v1/chat/completions"
_THINK_BLOCK = re.compile(r"<think>.*?</think>", re.DOTALL | re.IGNORECASE)
_JSON_FENCE = re.compile(
    r"```(?:json)?[ \t]*\r?\n?(.*?)```",
    re.DOTALL | re.IGNORECASE,
)


class LocalOpenAICompatibleAdapter(ProviderAdapter):
    """Map Unicorn turns onto an OpenAI-compatible chat-completion server."""

    def __init__(self, *, base_url: str = "") -> None:
        self._base_url = base_url

    def map_request(
        self,
        provider_input: Mapping[str, Any],
    ) -> dict[str, Any]:
        validate_provider_adapter_input(provider_input)
        return {
            "model": provider_input["model"],
            "messages": [
                {
                    "role": "system",
                    "content": provider_instructions(
                        provider_input["instructions"],
                    ),
                },
                {
                    "role": "user",
                    "content": serialize_provider_input(provider_input),
                },
            ],
            "temperature": 0.0,
            "response_format": {
                "type": "json_object",
            },
        }

    def send_request(
        self,
        native_request: Mapping[str, Any],
        *,
        api_key: str | None = None,
    ) -> ProviderTransportResponse:
        headers = {
            "Content-Type": "application/json",
        }
        if api_key:
            headers["Authorization"] = f"Bearer {api_key}"
        return post_json_transport(
            endpoint=_resolve_endpoint(self._base_url),
            payload=native_request,
            headers=headers,
            provider="local_openai_compat",
            timeout_seconds=180.0,
        )

    def inspect_response(
        self,
        raw_response: Mapping[str, Any],
    ) -> ProviderOutputInspection:
        raise_provider_api_error(
            raw_response,
            provider="local_openai_compat",
        )
        raw_text = _extract_raw_text(raw_response)
        return ProviderOutputInspection(
            raw_text=raw_text,
            sanitized_text=_sanitize_output_text(raw_text),
        )

    def parse_response(self, sanitized_text: str) -> dict[str, Any]:
        return parse_normalized_output(
            sanitized_text,
            provider="local_openai_compat",
        )


def _resolve_endpoint(base_url: str) -> str:
    trimmed = str(base_url or "").strip()
    if not trimmed:
        return _DEFAULT_ENDPOINT

    endpoint = trimmed.rstrip("/")
    if endpoint.endswith("/v1/chat/completions"):
        pass
    elif endpoint.endswith("/v1"):
        endpoint = f"{endpoint}/chat/completions"
    else:
        endpoint = f"{endpoint}/v1/chat/completions"
    return validate_http_endpoint(
        endpoint,
        provider="local_openai_compat",
    )


def _extract_raw_text(raw_response: Mapping[str, Any]) -> str:
    try:
        content = raw_response["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        content = None
    if isinstance(content, str) and content.strip():
        return content
    raise ProviderAdapterError(
        code="local_openai_compat_invalid_response",
        message="Local OpenAI-compatible response did not contain output text.",
    )


def _sanitize_output_text(raw_text: str) -> str:
    text = _THINK_BLOCK.sub("", raw_text).strip()

    fenced_blocks = _JSON_FENCE.findall(text)
    if fenced_blocks:
        return fenced_blocks[-1].strip()

    extracted = _extract_first_json_object(text)
    return extracted if extracted is not None else text


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
