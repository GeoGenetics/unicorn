"""Shared provider adapter boundary and provider-neutral parsing helpers."""

from __future__ import annotations

import json
import socket
from abc import ABC, abstractmethod
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

from unicorn_agent.contracts import (
    ContractValidationError,
    validate_provider_adapter_input,
    validate_normalized_provider_response,
)


class ProviderAdapterError(ValueError):
    """Stable provider-boundary failure without raw response contents."""

    def __init__(self, *, code: str, message: str) -> None:
        self.code = code
        super().__init__(message)


@dataclass(frozen=True)
class ProviderTransportResponse:
    """Provider-native response plus transport metadata needed for tracing."""

    status_code: int
    body: dict[str, Any]

    def __post_init__(self) -> None:
        if not 100 <= self.status_code <= 599:
            raise ValueError("Provider status_code must be between 100 and 599.")
        if not isinstance(self.body, dict):
            raise TypeError("Provider response body must be an object.")


@dataclass(frozen=True)
class ProviderOutputInspection:
    """Raw model output and the exact sanitized input used for parsing."""

    raw_text: str
    sanitized_text: str


class ProviderAdapter(ABC):
    """Common provider adapter interface owned by the backend orchestrator."""

    @abstractmethod
    def map_request(self, provider_input: Mapping[str, Any]) -> dict[str, Any]:
        """Map validated Unicorn input into a provider-native request."""

    @abstractmethod
    def send_request(
        self,
        native_request: Mapping[str, Any],
        *,
        api_key: str | None = None,
    ) -> ProviderTransportResponse:
        """Perform provider transport using only transient credentials."""

    @abstractmethod
    def inspect_response(
        self,
        raw_response: Mapping[str, Any],
    ) -> ProviderOutputInspection:
        """Extract raw model output and sanitized parser input."""

    @abstractmethod
    def parse_response(
        self,
        sanitized_text: str,
    ) -> dict[str, Any]:
        """Normalize one provider-native response."""


_LEGACY_TOOL_IDS = {
    "get_graph_context": "graph.context",
    "list_selected_datasets": "datasets.selected",
    "get_selected_nodes": "nodes.selected",
    "find_visible_nodes": "nodes.find_visible",
    "get_node_details": "node.details",
    "get_table_view": "table.view",
}

_CANONICAL_TOOL_IDS = [
    "graph.context",
    "datasets.selected",
    "nodes.selected",
    "nodes.find_visible",
    "node.details",
    "table.view",
]


def serialize_provider_input(provider_input: Mapping[str, Any]) -> str:
    """Serialize the validated canonical turn once, without duplicated history."""

    validate_provider_adapter_input(provider_input)
    payload = {
        "iteration": provider_input["iteration"],
        "graph_context": provider_input["graph_context"],
        "tools": provider_input["tools"],
        "tool_results": provider_input["tool_results"],
        "conversation": provider_input["conversation"],
        "user_prompt": provider_input["user_prompt"],
    }
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=True)


def provider_instructions(instructions: str) -> str:
    """Append the provider-neutral JSON-only response rules."""

    return "\n".join(
        [
            instructions.strip(),
            "",
            "Return exactly one JSON object and no markdown or prose outside it.",
            "Allowed response forms:",
            '{"type":"final_answer","content":"grounded answer"}',
            (
                '{"type":"tool_call","tool_id":"advertised.tool",'
                '"arguments":{}}'
            ),
            '{"type":"error","code":"stable_code","message":"explanation"}',
            "Use only tool IDs present in the supplied tools array.",
            "Never claim a tool action occurred until its result is supplied.",
        ]
    )


def provider_response_schema() -> dict[str, Any]:
    """Return a strict-output-compatible schema shared by hosted providers."""

    nullable_tool_id: list[Any] = [*_CANONICAL_TOOL_IDS, None]
    return {
        "type": "object",
        "additionalProperties": False,
        "required": [
            "type",
            "content",
            "tool_id",
            "arguments",
            "code",
            "message",
        ],
        "properties": {
            "type": {
                "type": "string",
                "enum": [
                    "assistant_message",
                    "tool_call",
                    "final_answer",
                    "error",
                ],
            },
            "content": {
                "type": ["string", "null"],
            },
            "tool_id": {
                "type": ["string", "null"],
                "enum": nullable_tool_id,
            },
            "arguments": {
                "type": ["object", "null"],
                "additionalProperties": False,
                "required": [
                    "taxid",
                    "taxids",
                    "query",
                    "scope",
                    "sort",
                    "limit",
                ],
                "properties": {
                    "taxid": {
                        "type": ["integer", "null"],
                        "minimum": 1,
                    },
                    "taxids": {
                        "type": ["array", "null"],
                        "items": {
                            "type": "integer",
                            "minimum": 1,
                        },
                    },
                    "query": {
                        "type": ["string", "null"],
                    },
                    "scope": {
                        "type": ["string", "null"],
                        "enum": ["root", "node", None],
                    },
                    "sort": {
                        "type": ["string", "null"],
                        "enum": ["direct", "subtree", None],
                    },
                    "limit": {
                        "type": ["integer", "null"],
                        "minimum": 1,
                    },
                },
            },
            "code": {
                "type": ["string", "null"],
            },
            "message": {
                "type": ["string", "null"],
            },
        },
    }


def post_json_transport(
    *,
    endpoint: str,
    payload: Mapping[str, Any],
    headers: Mapping[str, str],
    provider: str,
    timeout_seconds: float,
) -> ProviderTransportResponse:
    """POST one JSON object and preserve the provider-native JSON envelope."""

    request = Request(
        endpoint,
        data=json.dumps(
            payload,
            separators=(",", ":"),
            ensure_ascii=True,
        ).encode("utf-8"),
        headers=dict(headers),
        method="POST",
    )

    try:
        with urlopen(request, timeout=timeout_seconds) as response:
            status_code = int(response.getcode())
            body_text = response.read().decode("utf-8", errors="replace")
    except HTTPError as error:
        status_code = int(error.code)
        body_text = error.read().decode("utf-8", errors="replace")
    except (TimeoutError, socket.timeout) as error:
        raise ProviderAdapterError(
            code=f"{provider}_timeout",
            message=f"{provider} request timed out.",
        ) from error
    except URLError as error:
        if isinstance(error.reason, (TimeoutError, socket.timeout)):
            raise ProviderAdapterError(
                code=f"{provider}_timeout",
                message=f"{provider} request timed out.",
            ) from error
        raise ProviderAdapterError(
            code=f"{provider}_network_error",
            message=(
                f"{provider} request failed before reaching the API: "
                f"{error.reason}"
            ),
        ) from error

    try:
        body = json.loads(body_text)
    except json.JSONDecodeError as error:
        if status_code >= 400:
            body = {
                "error": {
                    "message": body_text or f"HTTP {status_code}",
                }
            }
        else:
            raise ProviderAdapterError(
                code=f"{provider}_invalid_http_response",
                message=f"{provider} returned a non-JSON HTTP response.",
            ) from error

    if not isinstance(body, dict):
        raise ProviderAdapterError(
            code=f"{provider}_invalid_http_response",
            message=f"{provider} returned a JSON value that is not an object.",
        )
    return ProviderTransportResponse(
        status_code=status_code,
        body=body,
    )


def validate_http_endpoint(endpoint: str, *, provider: str) -> str:
    """Reject malformed or credential-bearing provider endpoint URLs."""

    parsed = urlsplit(endpoint)
    if (
        parsed.scheme not in {"http", "https"}
        or not parsed.netloc
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
    ):
        raise ProviderAdapterError(
            code=f"{provider}_invalid_base_url",
            message=(
                f"{provider} base URL must be an HTTP(S) URL without "
                "credentials, query parameters, or fragments."
            ),
        )
    return endpoint


def raise_provider_api_error(
    raw_response: Mapping[str, Any],
    *,
    provider: str,
) -> None:
    """Raise one stable adapter error for a provider-native error envelope."""

    error = raw_response.get("error")
    if isinstance(error, Mapping):
        message = error.get("message")
    elif isinstance(error, str):
        message = error
    elif raw_response.get("object") == "error":
        message = raw_response.get("message")
    else:
        return
    if not isinstance(message, str) or not message.strip():
        message = f"{provider} returned an API error."
    raise ProviderAdapterError(
        code=f"{provider}_api_error",
        message=message.strip(),
    )


def parse_normalized_output(text: str, *, provider: str) -> dict[str, Any]:
    """Parse provider text and normalize historical JSON compatibility forms."""

    try:
        payload = json.loads(text)
    except (TypeError, json.JSONDecodeError) as error:
        raise ProviderAdapterError(
            code=f"{provider}_invalid_json",
            message=f"{provider} returned non-JSON output.",
        ) from error

    if not isinstance(payload, dict):
        raise ProviderAdapterError(
            code=f"{provider}_invalid_response",
            message=f"{provider} returned a JSON value that is not an object.",
        )

    normalized = _normalize_response_object(payload)
    try:
        validate_normalized_provider_response(normalized)
    except ContractValidationError as error:
        raise ProviderAdapterError(
            code=f"{provider}_invalid_response",
            message=f"{provider} returned an invalid Unicorn response: {error}",
        ) from error
    return normalized


def _normalize_response_object(payload: Mapping[str, Any]) -> dict[str, Any]:
    response_type = payload.get("type")
    if response_type is None and "tool" in payload:
        response_type = "tool_call"

    if response_type in {"assistant_message", "final_answer"}:
        return {
            "schema_version": "unicorn_provider_response_v1",
            "type": response_type,
            "content": payload.get("content"),
        }

    if response_type == "error":
        return {
            "schema_version": "unicorn_provider_response_v1",
            "type": "error",
            "code": payload.get("code"),
            "message": payload.get("message"),
        }

    if response_type == "tool_call":
        legacy_tool_id = payload.get("tool_name", payload.get("tool"))
        tool_id = payload.get("tool_id", legacy_tool_id)
        if isinstance(tool_id, str):
            tool_id = _LEGACY_TOOL_IDS.get(tool_id, tool_id)
        arguments = payload.get(
            "arguments",
            payload.get("args", payload.get("input", {})),
        )
        if isinstance(arguments, Mapping):
            arguments = {
                str(key): value
                for key, value in arguments.items()
                if value is not None
            }
        return {
            "schema_version": "unicorn_provider_response_v1",
            "type": "tool_call",
            "tool_id": tool_id,
            "arguments": arguments,
        }

    return {
        "schema_version": "unicorn_provider_response_v1",
        "type": response_type,
    }
