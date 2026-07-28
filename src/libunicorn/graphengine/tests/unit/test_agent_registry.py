from __future__ import annotations

from typing import Any

import pytest

from .helpers import require_agent_module


NODE_DETAILS_SCHEMA = {
    "type": "object",
    "additionalProperties": False,
    "required": ["taxid"],
    "properties": {
        "taxid": {
            "type": "integer",
            "minimum": 1,
        }
    },
}
NODE_DETAILS_USAGE = "Use after resolving a taxid."
NODE_DETAILS_OUTPUT = "One node identity and count summary."


def build_registry() -> tuple[Any, list[dict[str, Any]]]:
    registry_module = require_agent_module("unicorn_agent.registry")
    calls: list[dict[str, Any]] = []

    def node_details(arguments: dict[str, Any]) -> dict[str, Any]:
        calls.append(arguments)
        return {
            "taxid": arguments["taxid"],
            "name": "Fixture node",
        }

    registry = registry_module.ToolRegistry()
    registry.register(
        tool_id="node.details",
        description="Return details for one taxid.",
        when_to_use=NODE_DETAILS_USAGE,
        output_summary=NODE_DETAILS_OUTPUT,
        arguments_schema=NODE_DETAILS_SCHEMA,
        handler=node_details,
        mutation=False,
    )
    return registry, calls


def test_registry_advertises_stable_read_only_tool_definition() -> None:
    registry, _ = build_registry()
    definition = registry.get("node.details")

    assert registry.provider_tools() == [
        {
            "tool_id": "node.details",
            "description": "Return details for one taxid.",
            "when_to_use": NODE_DETAILS_USAGE,
            "output_summary": NODE_DETAILS_OUTPUT,
            "arguments_schema": NODE_DETAILS_SCHEMA,
            "mutation": False,
        }
    ]
    assert definition.when_to_use == NODE_DETAILS_USAGE
    assert definition.output_summary == NODE_DETAILS_OUTPUT
    assert callable(definition.handler)
    assert definition.mutation is False


def test_registry_validates_and_dispatches_tool_call() -> None:
    registry, calls = build_registry()

    result = registry.dispatch("node.details", {"taxid": 33090})

    assert result == {
        "taxid": 33090,
        "name": "Fixture node",
    }
    assert calls == [{"taxid": 33090}]


def test_registry_rejects_unknown_tool_without_execution() -> None:
    registry_module = require_agent_module("unicorn_agent.registry")
    registry, calls = build_registry()

    with pytest.raises(registry_module.ToolRegistryError) as caught:
        registry.dispatch("unknown.details", {"taxid": 33090})

    assert caught.value.code == "unknown_tool"
    assert calls == []


def test_registry_rejects_invalid_arguments_without_execution() -> None:
    registry_module = require_agent_module("unicorn_agent.registry")
    registry, calls = build_registry()

    with pytest.raises(registry_module.ToolRegistryError) as caught:
        registry.dispatch("node.details", {"taxid": "Viridiplantae"})

    assert caught.value.code == "invalid_tool_arguments"
    assert caught.value.path == ("taxid",)
    assert calls == []


def test_registry_rejects_duplicate_tool_id() -> None:
    registry_module = require_agent_module("unicorn_agent.registry")
    registry, _ = build_registry()

    with pytest.raises(registry_module.ToolRegistryError) as caught:
        registry.register(
            tool_id="node.details",
            description="Duplicate.",
            when_to_use=NODE_DETAILS_USAGE,
            output_summary=NODE_DETAILS_OUTPUT,
            arguments_schema=NODE_DETAILS_SCHEMA,
            handler=lambda arguments: arguments,
        )

    assert caught.value.code == "duplicate_tool"


@pytest.mark.parametrize(
    ("field", "code"),
    [
        ("when_to_use", "invalid_tool_usage"),
        ("output_summary", "invalid_tool_output_summary"),
    ],
)
def test_registry_rejects_missing_tool_guidance(
    field: str,
    code: str,
) -> None:
    registry_module = require_agent_module("unicorn_agent.registry")
    arguments = {
        "tool_id": "node.details",
        "description": "Return details for one taxid.",
        "when_to_use": NODE_DETAILS_USAGE,
        "output_summary": NODE_DETAILS_OUTPUT,
        "arguments_schema": NODE_DETAILS_SCHEMA,
        "handler": lambda values: values,
    }
    arguments[field] = " "

    with pytest.raises(registry_module.ToolRegistryError) as caught:
        registry_module.ToolRegistry().register(**arguments)

    assert caught.value.code == code


def test_registry_hides_and_blocks_mutation_tools_by_default() -> None:
    registry_module = require_agent_module("unicorn_agent.registry")
    registry, _ = build_registry()
    registry.register(
        tool_id="node.select",
        description="Select one taxid.",
        when_to_use="Use to mutate selection.",
        output_summary="Updated selection.",
        arguments_schema=NODE_DETAILS_SCHEMA,
        handler=lambda arguments: arguments,
        mutation=True,
    )

    assert [tool["tool_id"] for tool in registry.provider_tools()] == [
        "node.details"
    ]
    with pytest.raises(registry_module.ToolRegistryError) as caught:
        registry.dispatch("node.select", {"taxid": 33090})

    assert caught.value.code == "mutation_not_allowed"


@pytest.mark.parametrize("tool_id", ["get_node_details", "Node.Details", "node"])
def test_registry_rejects_unstable_tool_ids(tool_id: str) -> None:
    registry_module = require_agent_module("unicorn_agent.registry")
    registry = registry_module.ToolRegistry()

    with pytest.raises(registry_module.ToolRegistryError) as caught:
        registry.register(
            tool_id=tool_id,
            description="Invalid ID.",
            when_to_use=NODE_DETAILS_USAGE,
            output_summary=NODE_DETAILS_OUTPUT,
            arguments_schema=NODE_DETAILS_SCHEMA,
            handler=lambda arguments: arguments,
        )

    assert caught.value.code == "invalid_tool_id"
