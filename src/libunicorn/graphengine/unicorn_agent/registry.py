"""Define and dispatch the backend-owned Unicorn tool registry."""

from __future__ import annotations

import copy
import re
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from threading import RLock
from typing import Any

from jsonschema import Draft202012Validator, FormatChecker
from jsonschema.exceptions import SchemaError, best_match


ToolHandler = Callable[[dict[str, Any]], Mapping[str, Any]]
_TOOL_ID = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$")


@dataclass(frozen=True)
class ToolDefinition:
    tool_id: str
    description: str
    arguments_schema: dict[str, Any]
    handler: ToolHandler = field(repr=False, compare=False)
    mutation: bool = False

    def provider_payload(self) -> dict[str, Any]:
        return {
            "tool_id": self.tool_id,
            "description": self.description,
            "arguments_schema": copy.deepcopy(self.arguments_schema),
        }


@dataclass(frozen=True)
class ToolErrorDetail:
    code: str
    message: str
    tool_id: str | None
    path: tuple[str | int, ...] = ()
    rule: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "code": self.code,
            "message": self.message,
            "tool_id": self.tool_id,
            "path": list(self.path),
            "rule": self.rule,
        }


class ToolRegistryError(ValueError):
    """Structured registry failure that excludes argument values."""

    def __init__(
        self,
        *,
        code: str,
        message: str,
        tool_id: str | None = None,
        path: tuple[str | int, ...] = (),
        rule: str | None = None,
    ) -> None:
        self.detail = ToolErrorDetail(
            code=code,
            message=message,
            tool_id=tool_id,
            path=path,
            rule=rule,
        )
        self.code = code
        self.tool_id = tool_id
        self.path = path
        self.rule = rule
        super().__init__(message)

    def to_dict(self) -> dict[str, Any]:
        return self.detail.to_dict()


class ToolRegistry:
    """Register, advertise, validate, and dispatch Unicorn tools."""

    def __init__(self) -> None:
        self._definitions: dict[str, ToolDefinition] = {}
        self._validators: dict[str, Draft202012Validator] = {}
        self._lock = RLock()

    def register(
        self,
        *,
        tool_id: str,
        description: str,
        arguments_schema: Mapping[str, Any],
        handler: ToolHandler,
        mutation: bool = False,
    ) -> ToolDefinition:
        _validate_registration(
            tool_id=tool_id,
            description=description,
            arguments_schema=arguments_schema,
            handler=handler,
        )
        schema = copy.deepcopy(dict(arguments_schema))
        try:
            Draft202012Validator.check_schema(schema)
        except SchemaError as error:
            raise ToolRegistryError(
                code="invalid_tool_schema",
                message=f"Tool {tool_id} has an invalid argument schema.",
                tool_id=tool_id,
                rule="schema",
            ) from error

        definition = ToolDefinition(
            tool_id=tool_id,
            description=description.strip(),
            arguments_schema=schema,
            handler=handler,
            mutation=mutation,
        )
        validator = Draft202012Validator(
            schema,
            format_checker=FormatChecker(),
        )

        with self._lock:
            if tool_id in self._definitions:
                raise ToolRegistryError(
                    code="duplicate_tool",
                    message=f"Tool {tool_id} is already registered.",
                    tool_id=tool_id,
                )
            self._definitions[tool_id] = definition
            self._validators[tool_id] = validator
        return definition

    def get(self, tool_id: str) -> ToolDefinition:
        with self._lock:
            definition = self._definitions.get(tool_id)
        if definition is None:
            raise ToolRegistryError(
                code="unknown_tool",
                message=f"Tool {tool_id} is not registered.",
                tool_id=tool_id,
            )
        return definition

    def provider_tools(self) -> list[dict[str, Any]]:
        with self._lock:
            definitions = sorted(
                (
                    definition
                    for definition in self._definitions.values()
                    if not definition.mutation
                ),
                key=lambda definition: definition.tool_id,
            )
        return [definition.provider_payload() for definition in definitions]

    def validate_arguments(
        self,
        tool_id: str,
        arguments: Any,
    ) -> dict[str, Any]:
        self.get(tool_id)
        with self._lock:
            validator = self._validators[tool_id]

        error = best_match(validator.iter_errors(arguments))
        if error is not None:
            raise ToolRegistryError(
                code="invalid_tool_arguments",
                message=f"Arguments for tool {tool_id} failed validation.",
                tool_id=tool_id,
                path=tuple(error.absolute_path),
                rule=str(error.validator or "schema"),
            ) from None
        return copy.deepcopy(arguments)

    def dispatch(
        self,
        tool_id: str,
        arguments: Any,
        *,
        allow_mutation: bool = False,
    ) -> dict[str, Any]:
        definition = self.get(tool_id)
        if definition.mutation and not allow_mutation:
            raise ToolRegistryError(
                code="mutation_not_allowed",
                message=f"Mutation tool {tool_id} is disabled.",
                tool_id=tool_id,
            )

        validated_arguments = self.validate_arguments(tool_id, arguments)
        try:
            result = definition.handler(validated_arguments)
        except ToolRegistryError:
            raise
        except Exception as error:
            raise ToolRegistryError(
                code="tool_execution_failed",
                message=f"Tool {tool_id} failed during execution.",
                tool_id=tool_id,
            ) from error

        if not isinstance(result, Mapping):
            raise ToolRegistryError(
                code="invalid_tool_result",
                message=f"Tool {tool_id} returned a non-object result.",
                tool_id=tool_id,
            )
        return copy.deepcopy(dict(result))

    def __contains__(self, tool_id: object) -> bool:
        with self._lock:
            return tool_id in self._definitions

    def __len__(self) -> int:
        with self._lock:
            return len(self._definitions)


def _validate_registration(
    *,
    tool_id: str,
    description: str,
    arguments_schema: Mapping[str, Any],
    handler: ToolHandler,
) -> None:
    if not isinstance(tool_id, str) or not _TOOL_ID.fullmatch(tool_id):
        raise ToolRegistryError(
            code="invalid_tool_id",
            message="Tool IDs must use a stable dotted lowercase form.",
            tool_id=tool_id if isinstance(tool_id, str) else None,
        )
    if not isinstance(description, str) or not description.strip():
        raise ToolRegistryError(
            code="invalid_tool_description",
            message=f"Tool {tool_id} requires a description.",
            tool_id=tool_id,
        )
    if not isinstance(arguments_schema, Mapping):
        raise ToolRegistryError(
            code="invalid_tool_schema",
            message=f"Tool {tool_id} requires an object argument schema.",
            tool_id=tool_id,
        )
    if not callable(handler):
        raise ToolRegistryError(
            code="invalid_tool_handler",
            message=f"Tool {tool_id} requires a callable handler.",
            tool_id=tool_id,
        )
