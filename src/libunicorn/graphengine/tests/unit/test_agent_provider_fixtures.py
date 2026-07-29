from __future__ import annotations

import copy
from typing import Any

import pytest

from unicorn_agent.providers import create_provider_adapter

from .helpers import (
    assert_contains_no_secret,
    provider_fixtures,
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


PROVIDERS = [
    "google",
    "local_openai_compat",
    "openai",
]


def provider_adapter(provider: str):
    return create_provider_adapter(
        {
            "name": provider,
            "base_url": "",
        }
    )


@pytest.mark.parametrize("provider", PROVIDERS)
def test_provider_request_mapping_is_provider_native_and_secret_free(
    provider: str,
) -> None:
    adapter = provider_adapter(provider)
    provider_input = valid_provider_input()

    native_request = adapter.map_request(provider_input)

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
        assert native_request["max_tokens"] == 2048
        assert native_request["response_format"]["type"] == "json_schema"
        assert (
            native_request["response_format"]["json_schema"]["name"]
            == "unicorn_provider_response"
        )
        assert (
            native_request["response_format"]["json_schema"]["schema"][
                "additionalProperties"
            ]
            is False
        )
        assert (
            native_request["response_format"]["json_schema"]["schema"][
                "required"
            ]
            == ["type"]
        )


@pytest.mark.parametrize("provider", PROVIDERS)
def test_post_tool_provider_mapping_requires_result_reuse(
    provider: str,
) -> None:
    adapter = provider_adapter(provider)
    provider_input = valid_provider_input()
    provider_input["iteration"] = 1
    provider_input["tool_results"] = [
        {
            "schema_version": "unicorn_tool_result_v1",
            "tool_id": "node.details",
            "arguments": {
                "taxid": 33090,
            },
            "ok": True,
            "result": {
                "taxid": 33090,
                "name": "Viridiplantae",
                "rank": "kingdom",
            },
            "error": None,
        }
    ]

    native_request = adapter.map_request(provider_input)
    if provider == "openai":
        instructions = native_request["instructions"]
    elif provider == "google":
        instructions = native_request["system_instruction"]
    else:
        instructions = native_request["messages"][0]["content"]

    assert "post-tool iteration" in instructions
    assert "Never repeat a tool call" in instructions
    assert "return a concise final_answer immediately" in instructions
    assert "do not restart node lookup" in instructions
    assert "Conversation is non-authoritative" in instructions
    assert "Answer the current user_prompt" in instructions


@pytest.mark.parametrize("provider", PROVIDERS)
def test_grounded_name_lookup_maps_an_explicit_node_details_next_step(
    provider: str,
) -> None:
    adapter = provider_adapter(provider)
    provider_input = valid_provider_input()
    provider_input["iteration"] = 1
    provider_input["tool_results"] = [
        {
            "schema_version": "unicorn_tool_result_v1",
            "tool_id": "nodes.find_visible",
            "arguments": {
                "query": "Triticinae",
                "limit": 100,
            },
            "ok": True,
            "result": {
                "query": "Triticinae",
                "count": 1,
                "matches": [
                    {
                        "taxid": 1648030,
                        "name": "Triticinae",
                    }
                ],
            },
            "error": None,
        }
    ]

    native_request = adapter.map_request(provider_input)
    if provider == "openai":
        instructions = native_request["instructions"]
    elif provider == "google":
        instructions = native_request["system_instruction"]
    else:
        instructions = native_request["messages"][0]["content"]

    assert "grounded one match at taxid 1648030" in instructions
    assert "Do not call nodes.find_visible again" in instructions
    assert '{"taxid":1648030}' in instructions
    assert "metadata.compare_selected" in instructions
    assert '"taxids":[1648030]' in instructions


def test_empty_name_lookup_maps_to_controlled_final_answer() -> None:
    adapter = provider_adapter("local_openai_compat")
    provider_input = valid_provider_input()
    provider_input["iteration"] = 1
    provider_input["tool_results"] = [
        {
            "schema_version": "unicorn_tool_result_v1",
            "tool_id": "nodes.find_visible",
            "arguments": {
                "query": "unknown plant",
            },
            "ok": True,
            "result": {
                "query": "unknown plant",
                "count": 0,
                "matches": [],
            },
            "error": None,
        }
    ]

    native_request = adapter.map_request(provider_input)
    instructions = native_request["messages"][0]["content"]

    assert "returned zero matches" in instructions
    assert "Do not repeat the same lookup" in instructions
    assert "check its spelling" in instructions


def test_selected_nodes_map_one_multi_node_details_next_step() -> None:
    adapter = provider_adapter("local_openai_compat")
    provider_input = valid_provider_input()
    provider_input["iteration"] = 1
    provider_input["tool_results"] = [
        {
            "schema_version": "unicorn_tool_result_v1",
            "tool_id": "nodes.selected",
            "arguments": {},
            "ok": True,
            "result": {
                "count": 2,
                "nodes": [
                    {
                        "taxid": 4565,
                        "name": "Triticum aestivum",
                    },
                    {
                        "taxid": 4571,
                        "name": "Triticum turgidum",
                    },
                ],
            },
            "error": None,
        }
    ]

    native_request = adapter.map_request(provider_input)
    instructions = native_request["messages"][0]["content"]

    assert "Do not call nodes.selected again" in instructions
    assert '{"taxids":[4565,4571]}' in instructions


def test_metadata_summary_maps_field_or_comparison_decision() -> None:
    adapter = provider_adapter("local_openai_compat")
    provider_input = valid_provider_input()
    provider_input["iteration"] = 1
    provider_input["tool_results"] = [
        {
            "schema_version": "unicorn_tool_result_v1",
            "tool_id": "metadata.summary",
            "arguments": {},
            "ok": True,
            "result": {
                "loaded": True,
                "fields": ["group", "host", "site"],
                "field_count": 3,
            },
            "error": None,
        }
    ]

    native_request = adapter.map_request(provider_input)
    instructions = native_request["messages"][0]["content"]

    assert "available metadata fields" in instructions
    assert "final_answer" in instructions
    assert "metadata.compare_selected" in instructions


@pytest.mark.parametrize("provider", PROVIDERS)
def test_metadata_field_discovery_requires_summary_before_answer(
    provider: str,
) -> None:
    adapter = provider_adapter(provider)
    provider_input = valid_provider_input()
    provider_input["user_prompt"] = "What variables do we have in the metadata?"

    native_request = adapter.map_request(provider_input)
    if provider == "openai":
        instructions = native_request["instructions"]
    elif provider == "google":
        instructions = native_request["system_instruction"]
    else:
        instructions = native_request["messages"][0]["content"]

    assert "does not mean the backend metadata table" in instructions
    assert "do not return a final_answer" in instructions
    assert (
        '{"type":"tool_call","tool_id":"metadata.summary","arguments":{}}'
        in instructions
    )


def test_metadata_comparison_maps_directly_to_grounded_final_answer() -> None:
    adapter = provider_adapter("local_openai_compat")
    provider_input = valid_provider_input()
    provider_input["iteration"] = 1
    provider_input["tool_results"] = [
        {
            "schema_version": "unicorn_tool_result_v1",
            "tool_id": "metadata.compare_selected",
            "arguments": {
                "field": "group",
            },
            "ok": True,
            "result": {
                "field": "group",
                "comparison_basis": "descriptive_raw_counts",
                "groups": [],
            },
            "error": None,
        }
    ]

    native_request = adapter.map_request(provider_input)
    instructions = native_request["messages"][0]["content"]

    assert "descriptive metadata-to-node count comparison" in instructions
    assert "Do not call another tool" in instructions
    assert "non-causal and non-normalized caveat" in instructions


@pytest.mark.parametrize("provider,path,fixture", SUCCESS_FIXTURES)
def test_provider_fixture_extracts_and_normalizes_successfully(
    provider: str,
    path: Any,
    fixture: dict[str, Any],
) -> None:
    adapter = provider_adapter(provider)
    raw_response = copy.deepcopy(fixture["raw_provider_response"])

    inspection = adapter.inspect_response(raw_response)
    assert inspection.sanitized_text == fixture["expected_extracted_text"]

    normalized = adapter.parse_response(inspection.sanitized_text)
    assert normalized == fixture["expected_normalized_response"]


@pytest.mark.parametrize("provider,path,fixture", ERROR_FIXTURES)
def test_malformed_provider_fixture_fails_with_stable_error(
    provider: str,
    path: Any,
    fixture: dict[str, Any],
) -> None:
    adapter = provider_adapter(provider)
    expected_error = fixture["expected_error"]
    inspection = adapter.inspect_response(
        copy.deepcopy(fixture["raw_provider_response"])
    )

    with pytest.raises(Exception) as caught:
        adapter.parse_response(inspection.sanitized_text)

    assert getattr(caught.value, "code", None) == expected_error["code"]
    assert expected_error["message_contains"] in str(caught.value)


def test_local_think_content_is_preserved_raw_and_removed_before_parsing() -> None:
    adapter = provider_adapter("local_openai_compat")
    fixture = next(
        fixture
        for provider, path, fixture in provider_fixtures()
        if provider == "local_openai_compat" and path.stem == "think_and_tool_call"
    )

    inspection = adapter.inspect_response(fixture["raw_provider_response"])
    assert "<think>" in inspection.raw_text
    assert "</think>" in inspection.raw_text

    assert "<think>" not in inspection.sanitized_text
    assert "</think>" not in inspection.sanitized_text
    assert inspection.sanitized_text == fixture["expected_extracted_text"]


def test_local_fenced_json_is_extracted_without_markdown_fences() -> None:
    adapter = provider_adapter("local_openai_compat")
    fixture = next(
        fixture
        for provider, path, fixture in provider_fixtures()
        if provider == "local_openai_compat" and path.stem == "fenced_json"
    )

    inspection = adapter.inspect_response(fixture["raw_provider_response"])
    assert inspection.raw_text.startswith("```json")

    assert "```" not in inspection.sanitized_text
    assert inspection.sanitized_text == fixture["expected_extracted_text"]


def test_local_length_finish_reason_is_reported_as_truncated() -> None:
    adapter = provider_adapter("local_openai_compat")
    raw_response = {
        "choices": [
            {
                "finish_reason": "length",
                "message": {
                    "content": (
                        '{"type":"tool_call","tool_id":"nodes.selected",'
                        '"arguments":{}'
                    ),
                },
            }
        ]
    }

    with pytest.raises(Exception) as caught:
        adapter.inspect_response(raw_response)

    assert (
        getattr(caught.value, "code", None)
        == "local_openai_compat_truncated_response"
    )
    assert "output-token limit" in str(caught.value)
