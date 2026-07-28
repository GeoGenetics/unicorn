"""Provider adapters for the backend-owned Unicorn Agent runtime.

Adapters map provider-neutral inputs to native requests and native responses
back to the frozen normalized response contract. They never execute tools.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from unicorn_agent.providers.base import ProviderAdapter, ProviderAdapterError
from unicorn_agent.providers.google import GoogleAdapter
from unicorn_agent.providers.openai import OpenAIAdapter
from unicorn_agent.providers.openai_compat import (
    LocalOpenAICompatibleAdapter,
)


def create_provider_adapter(
    provider_config: Mapping[str, Any],
) -> ProviderAdapter:
    """Construct one request-scoped adapter from validated provider config."""

    provider_name = provider_config.get("name")
    base_url = provider_config.get("base_url")
    if not isinstance(base_url, str):
        raise ProviderAdapterError(
            code="provider_config_invalid",
            message="Provider base_url must be a string.",
        )

    if provider_name == "local_openai_compat":
        return LocalOpenAICompatibleAdapter(base_url=base_url)
    if provider_name == "google":
        return GoogleAdapter(base_url=base_url)
    if provider_name == "openai":
        return OpenAIAdapter(base_url=base_url)
    raise ProviderAdapterError(
        code="provider_not_supported",
        message=f"Unsupported provider target: {provider_name!r}.",
    )


__all__ = [
    "GoogleAdapter",
    "LocalOpenAICompatibleAdapter",
    "OpenAIAdapter",
    "create_provider_adapter",
]
