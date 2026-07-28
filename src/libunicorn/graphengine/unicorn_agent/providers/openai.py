"""OpenAI Responses API provider adapter."""

from __future__ import annotations

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
    provider_response_schema,
    raise_provider_api_error,
    serialize_provider_input,
    validate_http_endpoint,
)


_DEFAULT_ENDPOINT = "https://api.openai.com/v1/responses"


class OpenAIAdapter(ProviderAdapter):
    """Map Unicorn turns onto the OpenAI Responses API."""

    def __init__(self, *, base_url: str = "") -> None:
        self._base_url = base_url

    def map_request(
        self,
        provider_input: Mapping[str, Any],
    ) -> dict[str, Any]:
        validate_provider_adapter_input(provider_input)
        return {
            "model": provider_input["model"],
            "instructions": provider_instructions(
                provider_input["instructions"],
                iteration=provider_input["iteration"],
                has_tool_results=bool(provider_input["tool_results"]),
            ),
            "input": [
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "input_text",
                            "text": serialize_provider_input(provider_input),
                        }
                    ],
                }
            ],
            "text": {
                "format": {
                    "type": "json_schema",
                    "name": "unicorn_provider_response",
                    "strict": True,
                    "schema": provider_response_schema(),
                }
            },
            "store": False,
        }

    def send_request(
        self,
        native_request: Mapping[str, Any],
        *,
        api_key: str | None = None,
    ) -> ProviderTransportResponse:
        if not api_key:
            raise ProviderAdapterError(
                code="openai_api_key_required",
                message="OpenAI provider transport requires an API key.",
            )
        return post_json_transport(
            endpoint=_resolve_endpoint(self._base_url),
            payload=native_request,
            headers={
                "Authorization": f"Bearer {api_key}",
                "Content-Type": "application/json",
            },
            provider="openai",
            timeout_seconds=60.0,
        )

    def inspect_response(
        self,
        raw_response: Mapping[str, Any],
    ) -> ProviderOutputInspection:
        raise_provider_api_error(raw_response, provider="openai")
        raw_text = _extract_raw_text(raw_response)
        return ProviderOutputInspection(
            raw_text=raw_text,
            sanitized_text=raw_text.strip(),
        )

    def parse_response(self, sanitized_text: str) -> dict[str, Any]:
        return parse_normalized_output(
            sanitized_text,
            provider="openai",
        )


def _resolve_endpoint(base_url: str) -> str:
    trimmed = str(base_url or "").strip()
    if not trimmed:
        return _DEFAULT_ENDPOINT

    endpoint = trimmed.rstrip("/")
    if endpoint.endswith("/v1/responses"):
        pass
    elif endpoint.endswith("/v1"):
        endpoint = f"{endpoint}/responses"
    else:
        endpoint = f"{endpoint}/v1/responses"
    return validate_http_endpoint(endpoint, provider="openai")


def _extract_raw_text(raw_response: Mapping[str, Any]) -> str:
    output_text = raw_response.get("output_text")
    if isinstance(output_text, str) and output_text.strip():
        return output_text

    output = raw_response.get("output")
    if isinstance(output, list):
        text_parts: list[str] = []
        refusals: list[str] = []
        for item in output:
            if not isinstance(item, Mapping) or item.get("type") != "message":
                continue
            content = item.get("content")
            if not isinstance(content, list):
                continue
            for entry in content:
                if not isinstance(entry, Mapping):
                    continue
                text = entry.get("text")
                if entry.get("type") == "output_text" and isinstance(text, str):
                    text_parts.append(text)
                refusal = entry.get("refusal")
                if entry.get("type") == "refusal" and isinstance(refusal, str):
                    refusals.append(refusal)
        if text_parts:
            return "\n".join(text_parts)
        if refusals:
            raise ProviderAdapterError(
                code="openai_refusal",
                message="\n".join(refusals),
            )

    raise ProviderAdapterError(
        code="openai_invalid_response",
        message="OpenAI response did not contain output text.",
    )
