"""Runtime validation for the Unicorn Agent V1 contracts.

The machine-readable JSON Schemas in ``unicorn_agent/schemas`` are the only
authoritative contract definitions. This module loads and validates against
them without copying their field shapes into Python.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator, FormatChecker
from jsonschema.exceptions import ValidationError, best_match
from referencing import Registry, Resource


CONTRACT_DIR = Path(__file__).resolve().parent / "schemas"

_CONTRACT_FILES = {
    "browser_turn_request": "browser_turn_request.schema.json",
    "provider_adapter_input": "provider_adapter_input.schema.json",
    "normalized_provider_response": "normalized_provider_response.schema.json",
    "tool_result": "tool_result.schema.json",
    "trace_event": "trace_event.schema.json",
    "browser_turn_response": "browser_turn_response.schema.json",
}


@dataclass(frozen=True)
class ContractErrorDetail:
    """Secret-safe description of one contract validation failure."""

    code: str
    message: str
    contract: str
    path: tuple[str | int, ...]
    rule: str

    def to_dict(self) -> dict[str, Any]:
        return {
            "code": self.code,
            "message": self.message,
            "contract": self.contract,
            "path": list(self.path),
            "rule": self.rule,
        }


class ContractValidationError(ValueError):
    """Raised when data does not satisfy a frozen Agent contract."""

    def __init__(
        self,
        *,
        contract: str,
        path: tuple[str | int, ...],
        rule: str,
    ) -> None:
        location = _format_path(path)
        detail = ContractErrorDetail(
            code="contract_validation_error",
            message=f"Invalid {contract} payload at {location}: failed {rule}.",
            contract=contract,
            path=path,
            rule=rule,
        )
        self.detail = detail
        self.code = detail.code
        self.contract = detail.contract
        self.path = detail.path
        self.rule = detail.rule
        super().__init__(detail.message)

    def to_dict(self) -> dict[str, Any]:
        return self.detail.to_dict()


def _format_path(path: tuple[str | int, ...]) -> str:
    if not path:
        return "$"

    parts = ["$"]
    for item in path:
        if isinstance(item, int):
            parts.append(f"[{item}]")
        else:
            parts.append(f".{item}")
    return "".join(parts)


@lru_cache(maxsize=1)
def _load_schemas() -> dict[str, dict[str, Any]]:
    schemas: dict[str, dict[str, Any]] = {}
    for contract, filename in _CONTRACT_FILES.items():
        path = CONTRACT_DIR / filename
        with path.open("r", encoding="utf-8") as handle:
            schema = json.load(handle)
        Draft202012Validator.check_schema(schema)
        schemas[contract] = schema
    return schemas


@lru_cache(maxsize=1)
def _build_registry() -> Registry:
    registry = Registry()
    for schema in _load_schemas().values():
        registry = registry.with_resource(
            schema["$id"],
            Resource.from_contents(schema),
        )
    return registry


@lru_cache(maxsize=None)
def _get_validator(contract: str) -> Draft202012Validator:
    try:
        schema = _load_schemas()[contract]
    except KeyError as error:
        raise ValueError(f"Unknown Agent contract: {contract}") from error

    return Draft202012Validator(
        schema,
        registry=_build_registry(),
        format_checker=FormatChecker(),
    )


def validate_contract(contract: str, payload: Any) -> Any:
    """Validate and return a payload without transforming or copying it."""

    validator = _get_validator(contract)
    error = best_match(validator.iter_errors(payload))
    if error is not None:
        raise _contract_error(contract, error) from None
    return payload


def _contract_error(
    contract: str,
    error: ValidationError,
) -> ContractValidationError:
    rule = str(error.validator or "schema")
    return ContractValidationError(
        contract=contract,
        path=tuple(error.absolute_path),
        rule=rule,
    )


def validate_browser_turn_request(payload: Any) -> Any:
    return validate_contract("browser_turn_request", payload)


def validate_provider_adapter_input(payload: Any) -> Any:
    return validate_contract("provider_adapter_input", payload)


def validate_normalized_provider_response(payload: Any) -> Any:
    return validate_contract("normalized_provider_response", payload)


def validate_tool_result(payload: Any) -> Any:
    return validate_contract("tool_result", payload)


def validate_trace_event(payload: Any) -> Any:
    return validate_contract("trace_event", payload)


def validate_browser_turn_response(payload: Any) -> Any:
    return validate_contract("browser_turn_response", payload)


@lru_cache(maxsize=1)
def _get_graph_context_validator() -> Draft202012Validator:
    provider_schema = _load_schemas()["provider_adapter_input"]
    schema = {
        "$schema": provider_schema["$schema"],
        "$ref": f"{provider_schema['$id']}#/$defs/graph_context",
    }
    return Draft202012Validator(
        schema,
        registry=_build_registry(),
        format_checker=FormatChecker(),
    )


def validate_graph_context(payload: Any) -> Any:
    """Validate the compact context embedded in provider adapter input."""

    error = best_match(_get_graph_context_validator().iter_errors(payload))
    if error is not None:
        raise _contract_error("graph_context", error) from None
    return payload
