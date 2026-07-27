"""Own the validated Unicorn Agent turn loop and iteration state machine."""

from __future__ import annotations

import copy
import json
from collections.abc import Callable, Mapping
from typing import Any, Protocol

from unicorn_agent.context import ContextBuildError
from unicorn_agent.contracts import (
    ContractValidationError,
    validate_browser_turn_request,
    validate_browser_turn_response,
    validate_normalized_provider_response,
    validate_provider_adapter_input,
    validate_tool_result,
)
from unicorn_agent.providers.base import ProviderAdapterError
from unicorn_agent.registry import ToolRegistry, ToolRegistryError


DEFAULT_INSTRUCTIONS = (
    "Answer only from the supplied Unicorn graph context and Unicorn tool "
    "results. Use only the advertised read-only tools. Return exactly one "
    "response matching the Unicorn provider response contract."
)

_TERMINAL_TOOL_ERRORS = {
    "unknown_tool",
    "invalid_tool_arguments",
    "mutation_not_allowed",
}


class ProviderBoundary(Protocol):
    def map_request(
        self,
        provider_input: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def send_request(
        self,
        native_request: Mapping[str, Any],
        *,
        api_key: str | None = None,
    ) -> dict[str, Any]: ...

    def parse_response(
        self,
        raw_response: Mapping[str, Any],
    ) -> dict[str, Any]: ...


class TraceBoundary(Protocol):
    def start_trace(self, turn_id: str) -> str: ...


class AgentOrchestrator:
    """Run one complete, backend-owned provider and tool turn."""

    def __init__(
        self,
        *,
        provider: ProviderBoundary,
        registry: ToolRegistry,
        context_builder: Callable[[Mapping[str, Any]], dict[str, Any]],
        trace_store: TraceBoundary,
        max_tool_calls: int = 4,
        instructions: str = DEFAULT_INSTRUCTIONS,
    ) -> None:
        if not 1 <= max_tool_calls <= 4:
            raise ValueError("max_tool_calls must be between 1 and 4.")
        if not instructions.strip():
            raise ValueError("instructions must not be empty.")
        self._provider = provider
        self._registry = registry
        self._context_builder = context_builder
        self._trace_store = trace_store
        self._max_tool_calls = max_tool_calls
        self._instructions = instructions

    def run(
        self,
        request: Mapping[str, Any],
        *,
        api_key: str | None = None,
    ) -> dict[str, Any]:
        validate_browser_turn_request(request)
        turn_id = request["turn_id"]
        trace_id = self._trace_store.start_trace(turn_id)

        try:
            graph_context = self._context_builder(request)
        except ContextBuildError as error:
            return self._failed_result(
                turn_id=turn_id,
                trace_id=trace_id,
                tools_used=[],
                code=error.code,
                message=str(error),
            )
        except ContractValidationError as error:
            return self._failed_result(
                turn_id=turn_id,
                trace_id=trace_id,
                tools_used=[],
                code=error.code,
                message=str(error),
            )

        tool_results: list[dict[str, Any]] = []
        tools_used: list[dict[str, Any]] = []
        completed_calls: set[str] = set()

        for iteration in range(self._max_tool_calls + 1):
            try:
                provider_input = self._provider_input(
                    request=request,
                    graph_context=graph_context,
                    tool_results=tool_results,
                    iteration=iteration,
                )
                normalized = self._execute_provider(
                    provider_input,
                    api_key=api_key,
                )
            except (ContractValidationError, ProviderAdapterError) as error:
                return self._failed_result(
                    turn_id=turn_id,
                    trace_id=trace_id,
                    tools_used=tools_used,
                    code=getattr(error, "code", "provider_response_invalid"),
                    message=str(error),
                )
            except Exception:
                return self._failed_result(
                    turn_id=turn_id,
                    trace_id=trace_id,
                    tools_used=tools_used,
                    code="provider_request_failed",
                    message="Provider request failed.",
                )

            response_type = normalized["type"]
            if response_type in {"assistant_message", "final_answer"}:
                return self._completed_result(
                    turn_id=turn_id,
                    trace_id=trace_id,
                    tools_used=tools_used,
                    answer=normalized["content"],
                )
            if response_type == "error":
                return self._failed_result(
                    turn_id=turn_id,
                    trace_id=trace_id,
                    tools_used=tools_used,
                    code=normalized["code"],
                    message=normalized["message"],
                )

            if iteration >= self._max_tool_calls:
                return self._iteration_limit_result(
                    turn_id=turn_id,
                    trace_id=trace_id,
                    tools_used=tools_used,
                )

            tool_id = normalized["tool_id"]
            arguments = normalized["arguments"]
            call_signature = _tool_call_signature(tool_id, arguments)
            if call_signature in completed_calls:
                return self._failed_result(
                    turn_id=turn_id,
                    trace_id=trace_id,
                    tools_used=tools_used,
                    code="repeated_tool_call",
                    message=(
                        "Provider repeated the same Unicorn tool call without "
                        "using its existing result."
                    ),
                )

            try:
                result = self._registry.dispatch(tool_id, arguments)
            except ToolRegistryError as error:
                if error.code in _TERMINAL_TOOL_ERRORS:
                    return self._failed_result(
                        turn_id=turn_id,
                        trace_id=trace_id,
                        tools_used=tools_used,
                        code=error.code,
                        message=str(error),
                    )
                tool_result = _failed_tool_result(
                    tool_id=tool_id,
                    arguments=arguments,
                    code=error.code,
                    message=str(error),
                )
            else:
                tool_result = _successful_tool_result(
                    tool_id=tool_id,
                    arguments=arguments,
                    result=result,
                )

            validate_tool_result(tool_result)
            completed_calls.add(call_signature)
            tool_results.append(tool_result)
            tools_used.append(
                {
                    "tool_id": tool_id,
                    "ok": tool_result["ok"],
                }
            )

        raise RuntimeError("Unreachable Agent iteration state.")

    def _provider_input(
        self,
        *,
        request: Mapping[str, Any],
        graph_context: Mapping[str, Any],
        tool_results: list[dict[str, Any]],
        iteration: int,
    ) -> dict[str, Any]:
        provider_input = {
            "schema_version": "unicorn_provider_input_v1",
            "turn_id": request["turn_id"],
            "iteration": iteration,
            "model": request["provider"]["model"],
            "instructions": self._instructions,
            "graph_context": copy.deepcopy(dict(graph_context)),
            "tools": self._registry.provider_tools(),
            "tool_results": copy.deepcopy(tool_results),
            "conversation": copy.deepcopy(request["conversation"]),
            "user_prompt": request["prompt"],
        }
        validate_provider_adapter_input(provider_input)
        return provider_input

    def _execute_provider(
        self,
        provider_input: dict[str, Any],
        *,
        api_key: str | None,
    ) -> dict[str, Any]:
        native_request = self._provider.map_request(provider_input)
        raw_response = self._provider.send_request(
            native_request,
            api_key=api_key,
        )
        normalized = self._provider.parse_response(raw_response)
        validate_normalized_provider_response(normalized)
        return normalized

    @staticmethod
    def _completed_result(
        *,
        turn_id: str,
        trace_id: str,
        tools_used: list[dict[str, Any]],
        answer: str,
    ) -> dict[str, Any]:
        return _validated_browser_result(
            {
                "schema_version": "unicorn_agent_result_v1",
                "turn_id": turn_id,
                "status": "completed",
                "answer": answer,
                "tools_used": copy.deepcopy(tools_used),
                "error": None,
                "trace_id": trace_id,
            }
        )

    @staticmethod
    def _failed_result(
        *,
        turn_id: str,
        trace_id: str,
        tools_used: list[dict[str, Any]],
        code: str,
        message: str,
    ) -> dict[str, Any]:
        return _validated_browser_result(
            {
                "schema_version": "unicorn_agent_result_v1",
                "turn_id": turn_id,
                "status": "failed",
                "answer": None,
                "tools_used": copy.deepcopy(tools_used),
                "error": {
                    "code": code,
                    "message": message,
                },
                "trace_id": trace_id,
            }
        )

    @staticmethod
    def _iteration_limit_result(
        *,
        turn_id: str,
        trace_id: str,
        tools_used: list[dict[str, Any]],
    ) -> dict[str, Any]:
        return _validated_browser_result(
            {
                "schema_version": "unicorn_agent_result_v1",
                "turn_id": turn_id,
                "status": "iteration_limit",
                "answer": None,
                "tools_used": copy.deepcopy(tools_used),
                "error": {
                    "code": "iteration_limit",
                    "message": "Provider exceeded the Unicorn tool-call limit.",
                },
                "trace_id": trace_id,
            }
        )


def _tool_call_signature(tool_id: str, arguments: Mapping[str, Any]) -> str:
    return json.dumps(
        {
            "tool_id": tool_id,
            "arguments": arguments,
        },
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    )


def _successful_tool_result(
    *,
    tool_id: str,
    arguments: Mapping[str, Any],
    result: Mapping[str, Any],
) -> dict[str, Any]:
    return {
        "schema_version": "unicorn_tool_result_v1",
        "tool_id": tool_id,
        "arguments": copy.deepcopy(dict(arguments)),
        "ok": True,
        "result": copy.deepcopy(dict(result)),
        "error": None,
    }


def _failed_tool_result(
    *,
    tool_id: str,
    arguments: Mapping[str, Any],
    code: str,
    message: str,
) -> dict[str, Any]:
    return {
        "schema_version": "unicorn_tool_result_v1",
        "tool_id": tool_id,
        "arguments": copy.deepcopy(dict(arguments)),
        "ok": False,
        "result": None,
        "error": {
            "code": code,
            "message": message,
        },
    }


def _validated_browser_result(result: dict[str, Any]) -> dict[str, Any]:
    validate_browser_turn_response(result)
    return result
