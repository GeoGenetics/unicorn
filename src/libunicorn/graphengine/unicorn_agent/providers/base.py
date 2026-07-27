"""Shared provider adapter boundary and provider-neutral parsing helpers."""

from __future__ import annotations

import json
from abc import ABC, abstractmethod
from collections.abc import Mapping
from typing import Any

from unicorn_agent.contracts import (
    ContractValidationError,
    validate_normalized_provider_response,
)


class ProviderCredentials:
    """Ephemeral credentials that redact themselves from debug output."""

    __slots__ = ("_api_key",)

    def __init__(self, api_key: str | None = None) -> None:
        self._api_key = api_key

    @property
    def api_key(self) -> str | None:
        return self._api_key

    def __repr__(self) -> str:
        status = "<redacted>" if self._api_key else "None"
        return f"{type(self).__name__}(api_key={status})"


class ProviderAdapterError(ValueError):
    """Stable provider-boundary failure without raw response contents."""

    def __init__(self, *, code: str, message: str) -> None:
        self.code = code
        super().__init__(message)


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
    ) -> dict[str, Any]:
        """Perform provider transport using only transient credentials."""

    @abstractmethod
    def parse_response(
        self,
        raw_response: Mapping[str, Any],
    ) -> dict[str, Any]:
        """Normalize one provider-native response."""

    def execute(
        self,
        provider_input: Mapping[str, Any],
        credentials: ProviderCredentials,
    ) -> dict[str, Any]:
        native_request = self.map_request(provider_input)
        raw_response = self.send_request(
            native_request,
            api_key=credentials.api_key,
        )
        return self.parse_response(raw_response)


_LEGACY_TOOL_IDS = {
    "get_node_details": "node.details",
}


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
