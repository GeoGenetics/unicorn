from __future__ import annotations

import io
import json
from urllib.error import HTTPError

import pytest

from unicorn_agent.providers import (
    GoogleAdapter,
    LocalOpenAICompatibleAdapter,
    OpenAIAdapter,
    create_provider_adapter,
)
from unicorn_agent.providers.base import ProviderAdapterError

from .helpers import valid_provider_input


class StubResponse:
    def __init__(self, body: dict, *, status_code: int = 200) -> None:
        self._body = json.dumps(body).encode("utf-8")
        self._status_code = status_code

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        return None

    def getcode(self) -> int:
        return self._status_code

    def read(self) -> bytes:
        return self._body


@pytest.mark.parametrize(
    ("provider_config", "adapter_type"),
    [
        (
            {
                "name": "local_openai_compat",
                "base_url": "http://localhost:8542",
            },
            LocalOpenAICompatibleAdapter,
        ),
        (
            {
                "name": "google",
                "base_url": "",
            },
            GoogleAdapter,
        ),
        (
            {
                "name": "openai",
                "base_url": "",
            },
            OpenAIAdapter,
        ),
    ],
)
def test_provider_factory_selects_request_scoped_adapter(
    provider_config,
    adapter_type,
) -> None:
    assert isinstance(
        create_provider_adapter(provider_config),
        adapter_type,
    )


@pytest.mark.parametrize(
    (
        "adapter",
        "api_key",
        "expected_url",
        "expected_auth_header",
        "expected_auth_value",
    ),
    [
        (
            LocalOpenAICompatibleAdapter(
                base_url="http://localhost:8542",
            ),
            None,
            "http://localhost:8542/v1/chat/completions",
            "Authorization",
            None,
        ),
        (
            GoogleAdapter(
                base_url="https://generativelanguage.googleapis.com/v1",
            ),
            "google-secret",
            "https://generativelanguage.googleapis.com/v1/interactions",
            "X-goog-api-key",
            "google-secret",
        ),
        (
            OpenAIAdapter(base_url="https://api.openai.com/v1"),
            "openai-secret",
            "https://api.openai.com/v1/responses",
            "Authorization",
            "Bearer openai-secret",
        ),
    ],
)
def test_provider_transport_maps_endpoint_headers_and_json_body(
    monkeypatch,
    adapter,
    api_key,
    expected_url,
    expected_auth_header,
    expected_auth_value,
) -> None:
    captured = {}

    def fake_urlopen(request, *, timeout):
        captured["request"] = request
        captured["timeout"] = timeout
        return StubResponse(
            {
                "id": "fixture_response",
                "output_text": "{}",
            }
        )

    monkeypatch.setattr(
        "unicorn_agent.providers.base.urlopen",
        fake_urlopen,
    )
    native_request = adapter.map_request(valid_provider_input())

    transport = adapter.send_request(
        native_request,
        api_key=api_key,
    )

    request = captured["request"]
    assert request.full_url == expected_url
    assert request.get_header(expected_auth_header) == expected_auth_value
    assert request.get_header("Content-type") == "application/json"
    assert json.loads(request.data.decode("utf-8")) == native_request
    assert transport.status_code == 200
    assert transport.body["id"] == "fixture_response"


@pytest.mark.parametrize(
    "adapter",
    [
        GoogleAdapter(),
        OpenAIAdapter(),
    ],
)
def test_hosted_provider_rejects_missing_api_key_before_transport(
    monkeypatch,
    adapter,
) -> None:
    def unexpected_urlopen(*args, **kwargs):
        raise AssertionError("Network transport should not be reached.")

    monkeypatch.setattr(
        "unicorn_agent.providers.base.urlopen",
        unexpected_urlopen,
    )

    with pytest.raises(ProviderAdapterError) as caught:
        adapter.send_request(
            adapter.map_request(valid_provider_input()),
            api_key=None,
        )

    assert caught.value.code.endswith("_api_key_required")


def test_http_error_body_is_preserved_for_provider_inspection(
    monkeypatch,
) -> None:
    adapter = OpenAIAdapter()

    def fake_urlopen(request, *, timeout):
        del request, timeout
        raise HTTPError(
            url="https://api.openai.com/v1/responses",
            code=429,
            msg="Too Many Requests",
            hdrs=None,
            fp=io.BytesIO(
                json.dumps(
                    {
                        "error": {
                            "message": "Fixture quota exceeded.",
                        }
                    }
                ).encode("utf-8")
            ),
        )

    monkeypatch.setattr(
        "unicorn_agent.providers.base.urlopen",
        fake_urlopen,
    )
    transport = adapter.send_request(
        adapter.map_request(valid_provider_input()),
        api_key="fixture-secret",
    )

    assert transport.status_code == 429
    assert transport.body["error"]["message"] == "Fixture quota exceeded."
    with pytest.raises(ProviderAdapterError) as caught:
        adapter.inspect_response(transport.body)
    assert caught.value.code == "openai_api_error"
    assert str(caught.value) == "Fixture quota exceeded."


@pytest.mark.parametrize(
    ("adapter_type", "base_url"),
    [
        (
            LocalOpenAICompatibleAdapter,
            "file:///tmp/provider",
        ),
        (
            GoogleAdapter,
            "https://user:secret@example.com",
        ),
        (
            OpenAIAdapter,
            "https://api.openai.com?key=secret",
        ),
    ],
)
def test_provider_rejects_unsafe_base_url(adapter_type, base_url) -> None:
    adapter = adapter_type(base_url=base_url)
    with pytest.raises(ProviderAdapterError) as caught:
        adapter.send_request(
            adapter.map_request(valid_provider_input()),
            api_key="fixture-secret",
        )
    assert caught.value.code.endswith("_invalid_base_url")
