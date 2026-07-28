"""Deterministic no-network provider used to prove the Agent turn loop."""

from __future__ import annotations

import copy
import json
from collections.abc import Mapping
from typing import Any

from unicorn_agent.contracts import validate_provider_adapter_input
from unicorn_agent.providers.base import (
    ProviderAdapter,
    ProviderAdapterError,
    ProviderOutputInspection,
    ProviderTransportResponse,
    parse_normalized_output,
)


_FAKE_PROTOCOL = "unicorn_agent_fake_v1"
_CONTEXT_TOOL_ID = "graph.context"


class DeterministicFakeProviderAdapter(ProviderAdapter):
    """Call graph.context once, then summarize its reinjected result."""

    def map_request(
        self,
        provider_input: Mapping[str, Any],
    ) -> dict[str, Any]:
        validate_provider_adapter_input(provider_input)
        return {
            "fake_protocol": _FAKE_PROTOCOL,
            "iteration": provider_input["iteration"],
            "tool_results": copy.deepcopy(provider_input["tool_results"]),
        }

    def send_request(
        self,
        native_request: Mapping[str, Any],
        *,
        api_key: str | None = None,
    ) -> ProviderTransportResponse:
        del api_key
        if native_request.get("fake_protocol") != _FAKE_PROTOCOL:
            raise ProviderAdapterError(
                code="fake_request_invalid",
                message="Fake provider received an invalid native request.",
            )

        iteration = native_request.get("iteration")
        tool_results = native_request.get("tool_results")
        if not isinstance(iteration, int) or not isinstance(tool_results, list):
            raise ProviderAdapterError(
                code="fake_request_invalid",
                message="Fake provider request is missing iteration state.",
            )

        if iteration == 0:
            response = {
                "schema_version": "unicorn_provider_response_v1",
                "type": "tool_call",
                "tool_id": _CONTEXT_TOOL_ID,
                "arguments": {},
            }
        elif iteration == 1:
            response = _final_response(tool_results)
        else:
            response = {
                "schema_version": "unicorn_provider_response_v1",
                "type": "error",
                "code": "fake_iteration_invalid",
                "message": "Fake provider received an unexpected iteration.",
            }

        return ProviderTransportResponse(
            status_code=200,
            body=response,
        )

    def inspect_response(
        self,
        raw_response: Mapping[str, Any],
    ) -> ProviderOutputInspection:
        text = json.dumps(
            raw_response,
            separators=(",", ":"),
            ensure_ascii=True,
        )
        return ProviderOutputInspection(
            raw_text=text,
            sanitized_text=text,
        )

    def parse_response(self, sanitized_text: str) -> dict[str, Any]:
        return parse_normalized_output(
            sanitized_text,
            provider="fake_provider",
        )


def _final_response(tool_results: list[Any]) -> dict[str, Any]:
    if len(tool_results) != 1:
        return {
            "schema_version": "unicorn_provider_response_v1",
            "type": "error",
            "code": "fake_tool_result_missing",
            "message": "Fake provider did not receive exactly one tool result.",
        }

    tool_result = tool_results[0]
    if (
        not isinstance(tool_result, Mapping)
        or tool_result.get("tool_id") != _CONTEXT_TOOL_ID
        or tool_result.get("ok") is not True
        or not isinstance(tool_result.get("result"), Mapping)
    ):
        return {
            "schema_version": "unicorn_provider_response_v1",
            "type": "error",
            "code": "fake_tool_result_invalid",
            "message": "Fake provider received an invalid graph.context result.",
        }

    context = tool_result["result"]
    datasets = context.get("datasets", {})
    tree = context.get("tree", {})
    selected_datasets = datasets.get("selected_count", 0)
    visible_nodes = tree.get("visible_node_count", 0)
    selected_nodes = tree.get("selected_node_count", 0)
    return {
        "schema_version": "unicorn_provider_response_v1",
        "type": "final_answer",
        "content": (
            "Deterministic fake provider inspected graph.context: "
            f"{selected_datasets} selected dataset(s), "
            f"{visible_nodes} visible node(s), and "
            f"{selected_nodes} selected node(s)."
        ),
    }
