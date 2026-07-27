from __future__ import annotations

import copy

import pytest
from jsonschema import Draft202012Validator, ValidationError

from .helpers import (
    AGENT_FIXTURE_DIR,
    contract_registry,
    contract_schemas,
    contract_validator,
    load_json,
    provider_fixtures,
    require_agent_module,
    valid_browser_turn_request,
)


def test_contract_schemas_and_embedded_examples_are_valid() -> None:
    schemas = contract_schemas()
    registry = contract_registry(schemas)

    for schema_name, schema in schemas.items():
        Draft202012Validator.check_schema(schema)
        validator = Draft202012Validator(schema, registry=registry)
        for example in schema.get("examples", []):
            validator.validate(example)


def test_provider_fixtures_match_the_fixture_schema() -> None:
    fixture_schema = load_json(AGENT_FIXTURE_DIR / "fixture.schema.json")
    validator = Draft202012Validator(fixture_schema)

    for _, _, fixture in provider_fixtures():
        validator.validate(fixture)


def test_successful_fixture_outputs_match_the_v1_provider_response_contract() -> None:
    validator = contract_validator("normalized_provider_response.schema.json")

    for _, _, fixture in provider_fixtures():
        normalized = fixture["expected_normalized_response"]
        if normalized is not None:
            validator.validate(normalized)


@pytest.mark.parametrize(
    "case",
    [
        "missing_version",
        "unknown_version",
        "empty_prompt",
        "unknown_provider",
        "duplicate_dataset",
        "invalid_taxid",
        "api_key_at_top_level",
        "api_key_in_provider",
    ],
)
def test_browser_turn_schema_rejects_invalid_or_secret_bearing_requests(
    case: str,
) -> None:
    payload = valid_browser_turn_request()

    if case == "missing_version":
        del payload["schema_version"]
    elif case == "unknown_version":
        payload["schema_version"] = "unicorn_agent_turn_v2"
    elif case == "empty_prompt":
        payload["prompt"] = ""
    elif case == "unknown_provider":
        payload["provider"]["name"] = "unknown"
    elif case == "duplicate_dataset":
        payload["graph_scope"]["datasets"] *= 2
    elif case == "invalid_taxid":
        payload["graph_scope"]["selected_taxids"] = [0]
    elif case == "api_key_at_top_level":
        payload["api_key"] = "must-not-enter-contract"
    elif case == "api_key_in_provider":
        payload["provider"]["api_key"] = "must-not-enter-contract"

    with pytest.raises(ValidationError):
        contract_validator("browser_turn_request.schema.json").validate(payload)


def test_runtime_contract_validator_accepts_valid_request_and_rejects_secret() -> None:
    contracts = require_agent_module("unicorn_agent.contracts")
    payload = valid_browser_turn_request()

    assert contracts.validate_browser_turn_request(payload) == payload

    invalid = copy.deepcopy(payload)
    invalid["api_key"] = "must-not-enter-contract"
    with pytest.raises(contracts.ContractValidationError):
        contracts.validate_browser_turn_request(invalid)

