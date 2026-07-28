"""Google Gemini Interactions API provider adapter."""

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


_DEFAULT_ENDPOINT = "https://generativelanguage.googleapis.com/v1/interactions"


class GoogleAdapter(ProviderAdapter):
    """Map Unicorn turns onto the Gemini Interactions API."""

    def __init__(self, *, base_url: str = "") -> None:
        self._base_url = base_url

    def map_request(
        self,
        provider_input: Mapping[str, Any],
    ) -> dict[str, Any]:
        validate_provider_adapter_input(provider_input)
        return {
            "model": provider_input["model"],
            "system_instruction": provider_instructions(
                provider_input["instructions"],
                iteration=provider_input["iteration"],
                has_tool_results=bool(provider_input["tool_results"]),
            ),
            "input": serialize_provider_input(provider_input),
            "response_format": {
                "type": "text",
                "mime_type": "application/json",
                "schema": provider_response_schema(),
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
                code="google_api_key_required",
                message="Google provider transport requires an API key.",
            )
        return post_json_transport(
            endpoint=_resolve_endpoint(self._base_url),
            payload=native_request,
            headers={
                "Content-Type": "application/json",
                "x-goog-api-key": api_key,
            },
            provider="google",
            timeout_seconds=60.0,
        )

    def inspect_response(
        self,
        raw_response: Mapping[str, Any],
    ) -> ProviderOutputInspection:
        raise_provider_api_error(raw_response, provider="google")
        raw_text = _extract_raw_text(raw_response)
        return ProviderOutputInspection(
            raw_text=raw_text,
            sanitized_text=raw_text.strip(),
        )

    def parse_response(self, sanitized_text: str) -> dict[str, Any]:
        return parse_normalized_output(
            sanitized_text,
            provider="google",
        )


def _resolve_endpoint(base_url: str) -> str:
    trimmed = str(base_url or "").strip()
    if not trimmed:
        return _DEFAULT_ENDPOINT

    endpoint = trimmed.rstrip("/")
    if endpoint.endswith(("/v1/interactions", "/v1beta/interactions")):
        pass
    elif endpoint.endswith(("/v1", "/v1beta")):
        endpoint = f"{endpoint}/interactions"
    else:
        endpoint = f"{endpoint}/v1/interactions"
    return validate_http_endpoint(endpoint, provider="google")


def _extract_raw_text(raw_response: Mapping[str, Any]) -> str:
    output_text = raw_response.get("output_text")
    if isinstance(output_text, str) and output_text.strip():
        return output_text

    steps = raw_response.get("steps")
    if isinstance(steps, list):
        text_parts: list[str] = []
        for step in steps:
            if not isinstance(step, Mapping):
                continue
            if step.get("type") not in {None, "model_output"}:
                continue
            content = step.get("content")
            if not isinstance(content, list):
                continue
            for item in content:
                if not isinstance(item, Mapping):
                    continue
                text = item.get("text")
                if isinstance(text, str) and text.strip():
                    text_parts.append(text)
        if text_parts:
            return "\n".join(text_parts)

    raise ProviderAdapterError(
        code="google_invalid_response",
        message="Google response did not contain model output text.",
    )
