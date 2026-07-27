from __future__ import annotations

import copy
from typing import Any

import pytest

from .helpers import (
    PROVIDER_MODULES,
    assert_contains_no_secret,
    provider_fixtures,
    require_agent_module,
    valid_provider_input,
)


SUCCESS_FIXTURES = [
    pytest.param(provider, path, fixture, id=f"{provider}-{path.stem}")
    for provider, path, fixture in provider_fixtures()
    if fixture["expected_normalized_response"] is not None
]

ERROR_FIXTURES = [
    pytest.param(provider, path, fixture, id=f"{provider}-{path.stem}")
    for provider, path, fixture in provider_fixtures()
    if fixture["expected_error"] is not None
]


def expected_native_text(provider: str, raw_response: dict[str, Any]) -> str:
    if provider == "openai":
        return str(raw_response["output"][0]["content"][0]["text"])
    if provider == "google":
        return str(raw_response["output_text"])
    return str(raw_response["choices"][0]["message"]["content"])


@pytest.mark.parametrize("provider", sorted(PROVIDER_MODULES))
def test_provider_request_mapping_is_provider_native_and_secret_free(
    provider: str,
) -> None:
    provider_module = require_agent_module(PROVIDER_MODULES[provider])
    provider_input = valid_provider_input()

    native_request = provider_module.map_request(provider_input)

    assert native_request["model"] == provider_input["model"]
    assert_contains_no_secret(native_request, "fixture-provider-secret")

    if provider == "openai":
        assert "input" in native_request
        assert "text" in native_request
    elif provider == "google":
        assert "input" in native_request
        assert "response_format" in native_request
    else:
        assert "messages" in native_request
        assert native_request["temperature"] == 0.0


@pytest.mark.parametrize("provider,path,fixture", SUCCESS_FIXTURES)
def test_provider_fixture_extracts_and_normalizes_successfully(
    provider: str,
    path: Any,
    fixture: dict[str, Any],
) -> None:
    provider_module = require_agent_module(PROVIDER_MODULES[provider])
    raw_response = copy.deepcopy(fixture["raw_provider_response"])

    raw_text = provider_module.extract_raw_text(raw_response)
    assert raw_text == expected_native_text(provider, raw_response)

    extracted_text = provider_module.sanitize_output_text(raw_text)
    assert extracted_text == fixture["expected_extracted_text"]

    normalized = provider_module.parse_response(raw_response)
    assert normalized == fixture["expected_normalized_response"]


@pytest.mark.parametrize("provider,path,fixture", ERROR_FIXTURES)
def test_malformed_provider_fixture_fails_with_stable_error(
    provider: str,
    path: Any,
    fixture: dict[str, Any],
) -> None:
    provider_module = require_agent_module(PROVIDER_MODULES[provider])
    expected_error = fixture["expected_error"]

    with pytest.raises(Exception) as caught:
        provider_module.parse_response(copy.deepcopy(fixture["raw_provider_response"]))

    assert getattr(caught.value, "code", None) == expected_error["code"]
    assert expected_error["message_contains"] in str(caught.value)


def test_local_think_content_is_preserved_raw_and_removed_before_parsing() -> None:
    provider_module = require_agent_module(PROVIDER_MODULES["local_openai_compat"])
    fixture = next(
        fixture
        for provider, path, fixture in provider_fixtures()
        if provider == "local_openai_compat" and path.stem == "think_and_tool_call"
    )

    raw_text = provider_module.extract_raw_text(fixture["raw_provider_response"])
    assert "<think>" in raw_text
    assert "</think>" in raw_text

    sanitized = provider_module.sanitize_output_text(raw_text)
    assert "<think>" not in sanitized
    assert "</think>" not in sanitized
    assert sanitized == fixture["expected_extracted_text"]


def test_local_fenced_json_is_extracted_without_markdown_fences() -> None:
    provider_module = require_agent_module(PROVIDER_MODULES["local_openai_compat"])
    fixture = next(
        fixture
        for provider, path, fixture in provider_fixtures()
        if provider == "local_openai_compat" and path.stem == "fenced_json"
    )

    raw_text = provider_module.extract_raw_text(fixture["raw_provider_response"])
    assert raw_text.startswith("```json")

    sanitized = provider_module.sanitize_output_text(raw_text)
    assert "```" not in sanitized
    assert sanitized == fixture["expected_extracted_text"]

