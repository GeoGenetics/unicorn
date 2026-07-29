"""Own the validated Unicorn Agent turn loop and iteration state machine."""

from __future__ import annotations

import copy
import json
from collections.abc import Callable, Mapping
from time import monotonic
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
from unicorn_agent.providers.base import (
    ProviderAdapterError,
    ProviderOutputInspection,
    ProviderTransportResponse,
)
from unicorn_agent.registry import ToolRegistry, ToolRegistryError
from unicorn_agent.tracing import payload_size_bytes


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
    ) -> ProviderTransportResponse: ...

    def inspect_response(
        self,
        raw_response: Mapping[str, Any],
    ) -> ProviderOutputInspection: ...

    def parse_response(self, sanitized_text: str) -> dict[str, Any]: ...


class TraceBoundary(Protocol):
    def start_trace(
        self,
        turn_id: str,
        *,
        secrets: tuple[str | None, ...] = (),
    ) -> str: ...

    def record(
        self,
        trace_id: str,
        *,
        iteration: int,
        event: str,
        data: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def finish_trace(self, trace_id: str) -> None: ...


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
        trace_id = self._trace_store.start_trace(
            turn_id,
            secrets=(api_key,),
        )
        self._record(
            trace_id,
            iteration=0,
            event="turn_received",
            data={
                "request": request,
                "payload_bytes": payload_size_bytes(request),
            },
        )
        self._record(
            trace_id,
            iteration=0,
            event="turn_validated",
            data={
                "schema_version": request["schema_version"],
            },
        )

        try:
            graph_context = self._context_builder(request)
        except ContextBuildError as error:
            return self._failed_turn(
                turn_id=turn_id,
                trace_id=trace_id,
                iteration=0,
                tools_used=[],
                code=error.code,
                message=str(error),
            )
        except ContractValidationError as error:
            return self._failed_turn(
                turn_id=turn_id,
                trace_id=trace_id,
                iteration=0,
                tools_used=[],
                code=error.code,
                message=str(error),
            )
        except Exception:
            return self._failed_turn(
                turn_id=turn_id,
                trace_id=trace_id,
                iteration=0,
                tools_used=[],
                code="context_build_failed",
                message="Backend graph context construction failed.",
            )
        self._record(
            trace_id,
            iteration=0,
            event="context_built",
            data={
                "graph_context": graph_context,
                "payload_bytes": payload_size_bytes(graph_context),
            },
        )

        tool_results: list[dict[str, Any]] = []
        tools_used: list[dict[str, Any]] = []
        completed_calls: set[str] = set()

        for iteration in range(self._max_tool_calls + 1):
            self._record(
                trace_id,
                iteration=iteration,
                event="iteration_started",
                data={
                    "available_tool_results": len(tool_results),
                },
            )
            if tool_results:
                reinjected = tool_results[-1]
                self._record(
                    trace_id,
                    iteration=iteration,
                    event="tool_result_reinjected",
                    data={
                        "tool_result": reinjected,
                        "payload_bytes": payload_size_bytes(reinjected),
                    },
                )
            try:
                provider_input = self._provider_input(
                    request=request,
                    graph_context=graph_context,
                    tool_results=tool_results,
                    iteration=iteration,
                )
                self._record(
                    trace_id,
                    iteration=iteration,
                    event="provider_input_created",
                    data={
                        "provider_input": provider_input,
                        "payload_bytes": payload_size_bytes(provider_input),
                    },
                )
                normalized = self._execute_provider(
                    provider_input,
                    provider_config=request["provider"],
                    api_key=api_key,
                    trace_id=trace_id,
                    iteration=iteration,
                )
            except (ContractValidationError, ProviderAdapterError) as error:
                self._iteration_completed(
                    trace_id,
                    iteration=iteration,
                    status="failed",
                    error_code=getattr(
                        error,
                        "code",
                        "provider_response_invalid",
                    ),
                )
                return self._failed_turn(
                    turn_id=turn_id,
                    trace_id=trace_id,
                    iteration=iteration,
                    tools_used=tools_used,
                    code=getattr(error, "code", "provider_response_invalid"),
                    message=str(error),
                )
            except Exception:
                self._iteration_completed(
                    trace_id,
                    iteration=iteration,
                    status="failed",
                    error_code="provider_request_failed",
                )
                return self._failed_turn(
                    turn_id=turn_id,
                    trace_id=trace_id,
                    iteration=iteration,
                    tools_used=tools_used,
                    code="provider_request_failed",
                    message="Provider request failed.",
                )

            response_type = normalized["type"]
            if response_type in {"assistant_message", "final_answer"}:
                self._iteration_completed(
                    trace_id,
                    iteration=iteration,
                    status="completed",
                    response_type=response_type,
                )
                return self._completed_turn(
                    turn_id=turn_id,
                    trace_id=trace_id,
                    iteration=iteration,
                    tools_used=tools_used,
                    answer=normalized["content"],
                )
            if response_type == "error":
                self._iteration_completed(
                    trace_id,
                    iteration=iteration,
                    status="failed",
                    response_type=response_type,
                    error_code=normalized["code"],
                )
                return self._failed_turn(
                    turn_id=turn_id,
                    trace_id=trace_id,
                    iteration=iteration,
                    tools_used=tools_used,
                    code=normalized["code"],
                    message=normalized["message"],
                )

            if iteration >= self._max_tool_calls:
                return self._iteration_limit_turn(
                    turn_id=turn_id,
                    trace_id=trace_id,
                    iteration=iteration,
                    tools_used=tools_used,
                    tool_id=normalized["tool_id"],
                )

            tool_id = normalized["tool_id"]
            arguments = normalized["arguments"]
            call_signature = _tool_call_signature(tool_id, arguments)
            if call_signature in completed_calls:
                self._iteration_completed(
                    trace_id,
                    iteration=iteration,
                    status="failed",
                    response_type="tool_call",
                    error_code="repeated_tool_call",
                )
                return self._failed_turn(
                    turn_id=turn_id,
                    trace_id=trace_id,
                    iteration=iteration,
                    tools_used=tools_used,
                    code="repeated_tool_call",
                    message=(
                        "Provider repeated the same Unicorn tool call without "
                        "using its existing result."
                    ),
                )

            try:
                self._registry.validate_arguments(tool_id, arguments)
            except ToolRegistryError as error:
                self._iteration_completed(
                    trace_id,
                    iteration=iteration,
                    status="failed",
                    response_type="tool_call",
                    error_code=error.code,
                )
                return self._failed_turn(
                    turn_id=turn_id,
                    trace_id=trace_id,
                    iteration=iteration,
                    tools_used=tools_used,
                    code=error.code,
                    message=str(error),
                )

            self._record(
                trace_id,
                iteration=iteration,
                event="tool_call_validated",
                data={
                    "tool_id": tool_id,
                    "arguments": arguments,
                    "payload_bytes": payload_size_bytes(arguments),
                },
            )
            self._record(
                trace_id,
                iteration=iteration,
                event="tool_execution_started",
                data={
                    "tool_id": tool_id,
                    "arguments": arguments,
                },
            )

            try:
                result = self._registry.dispatch(tool_id, arguments)
            except ToolRegistryError as error:
                if error.code in _TERMINAL_TOOL_ERRORS:
                    self._iteration_completed(
                        trace_id,
                        iteration=iteration,
                        status="failed",
                        response_type="tool_call",
                        error_code=error.code,
                    )
                    return self._failed_turn(
                        turn_id=turn_id,
                        trace_id=trace_id,
                        iteration=iteration,
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
            self._record(
                trace_id,
                iteration=iteration,
                event="tool_execution_completed",
                data={
                    "tool_result": tool_result,
                },
            )
            completed_calls.add(call_signature)
            tool_results.append(tool_result)
            tools_used.append(
                {
                    "tool_id": tool_id,
                    "ok": tool_result["ok"],
                }
            )
            self._iteration_completed(
                trace_id,
                iteration=iteration,
                status="completed",
                response_type="tool_call",
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
        provider_config: Mapping[str, Any],
        api_key: str | None,
        trace_id: str,
        iteration: int,
    ) -> dict[str, Any]:
        native_request = self._provider.map_request(provider_input)
        self._record(
            trace_id,
            iteration=iteration,
            event="provider_request_mapped",
            data={
                "provider": provider_config["name"],
                "native_request": native_request,
                "payload_bytes": payload_size_bytes(native_request),
            },
        )
        self._record(
            trace_id,
            iteration=iteration,
            event="provider_http_started",
            data={
                "provider": provider_config["name"],
                "model": provider_config["model"],
                "base_url": provider_config["base_url"],
                "request_bytes": payload_size_bytes(native_request),
            },
        )
        started_at = monotonic()
        try:
            transport = self._provider.send_request(
                native_request,
                api_key=api_key,
            )
        except ProviderAdapterError as error:
            duration_ms = max(0.0, (monotonic() - started_at) * 1000.0)
            self._record(
                trace_id,
                iteration=iteration,
                event="provider_http_failed",
                data={
                    "provider": provider_config["name"],
                    "duration_ms": duration_ms,
                    "error": {
                        "code": error.code,
                        "message": str(error),
                    },
                },
            )
            raise
        except Exception:
            duration_ms = max(0.0, (monotonic() - started_at) * 1000.0)
            self._record(
                trace_id,
                iteration=iteration,
                event="provider_http_failed",
                data={
                    "provider": provider_config["name"],
                    "duration_ms": duration_ms,
                    "error": {
                        "code": "provider_request_failed",
                        "message": "Provider request failed.",
                    },
                },
            )
            raise
        duration_ms = max(0.0, (monotonic() - started_at) * 1000.0)
        if not isinstance(transport, ProviderTransportResponse):
            raise ProviderAdapterError(
                code="provider_transport_invalid",
                message="Provider returned an invalid transport response.",
            )
        self._record(
            trace_id,
            iteration=iteration,
            event="provider_http_completed",
            data={
                "provider": provider_config["name"],
                "status_code": transport.status_code,
                "duration_ms": duration_ms,
                "response_bytes": payload_size_bytes(transport.body),
                "raw_response": transport.body,
            },
        )
        output = self._provider.inspect_response(transport.body)
        if not isinstance(output, ProviderOutputInspection):
            raise ProviderAdapterError(
                code="provider_output_invalid",
                message="Provider returned invalid output inspection data.",
            )
        self._record(
            trace_id,
            iteration=iteration,
            event="provider_output_extracted",
            data={
                "raw_output": output.raw_text,
                "raw_output_bytes": len(output.raw_text.encode("utf-8")),
                "sanitized_input": output.sanitized_text,
                "sanitized_input_bytes": len(
                    output.sanitized_text.encode("utf-8")
                ),
            },
        )
        normalized = self._provider.parse_response(output.sanitized_text)
        validate_normalized_provider_response(normalized)
        self._record(
            trace_id,
            iteration=iteration,
            event="provider_response_normalized",
            data={
                "normalized_response": normalized,
                "payload_bytes": payload_size_bytes(normalized),
            },
        )
        return normalized

    def _iteration_completed(
        self,
        trace_id: str,
        *,
        iteration: int,
        status: str,
        response_type: str | None = None,
        error_code: str | None = None,
    ) -> None:
        data: dict[str, Any] = {
            "status": status,
        }
        if response_type is not None:
            data["response_type"] = response_type
        if error_code is not None:
            data["error_code"] = error_code
        self._record(
            trace_id,
            iteration=iteration,
            event="iteration_completed",
            data=data,
        )

    def _record(
        self,
        trace_id: str,
        *,
        iteration: int,
        event: str,
        data: Mapping[str, Any],
    ) -> None:
        self._trace_store.record(
            trace_id,
            iteration=iteration,
            event=event,
            data=data,
        )

    def _completed_turn(
        self,
        *,
        turn_id: str,
        trace_id: str,
        iteration: int,
        tools_used: list[dict[str, Any]],
        answer: str,
    ) -> dict[str, Any]:
        result = self._completed_result(
            turn_id=turn_id,
            trace_id=trace_id,
            tools_used=tools_used,
            answer=answer,
        )
        self._record(
            trace_id,
            iteration=iteration,
            event="turn_completed",
            data={
                "result": result,
                "payload_bytes": payload_size_bytes(result),
            },
        )
        self._trace_store.finish_trace(trace_id)
        return result

    def _failed_turn(
        self,
        *,
        turn_id: str,
        trace_id: str,
        iteration: int,
        tools_used: list[dict[str, Any]],
        code: str,
        message: str,
    ) -> dict[str, Any]:
        result = self._failed_result(
            turn_id=turn_id,
            trace_id=trace_id,
            tools_used=tools_used,
            code=code,
            message=message,
        )
        self._record(
            trace_id,
            iteration=iteration,
            event="turn_failed",
            data={
                "result": result,
                "payload_bytes": payload_size_bytes(result),
            },
        )
        self._trace_store.finish_trace(trace_id)
        return result

    def _iteration_limit_turn(
        self,
        *,
        turn_id: str,
        trace_id: str,
        iteration: int,
        tools_used: list[dict[str, Any]],
        tool_id: str,
    ) -> dict[str, Any]:
        self._iteration_completed(
            trace_id,
            iteration=iteration,
            status="iteration_limit",
            response_type="tool_call",
            error_code="iteration_limit",
        )
        self._record(
            trace_id,
            iteration=iteration,
            event="iteration_limit_reached",
            data={
                "max_tool_calls": self._max_tool_calls,
                "rejected_tool_id": tool_id,
            },
        )
        result = self._iteration_limit_result(
            turn_id=turn_id,
            trace_id=trace_id,
            tools_used=tools_used,
        )
        self._record(
            trace_id,
            iteration=iteration,
            event="turn_failed",
            data={
                "result": result,
                "payload_bytes": payload_size_bytes(result),
            },
        )
        self._trace_store.finish_trace(trace_id)
        return result

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
