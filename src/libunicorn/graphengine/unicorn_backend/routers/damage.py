"""Damage-profile HTTP routes."""

from __future__ import annotations

from pathlib import Path
from typing import Annotated, Any, Dict, List, Optional

from fastapi import APIRouter
from pydantic import BaseModel, ConfigDict, Field, field_validator

from unicorn_backend.damage import (
    damage_node_payload,
    damage_selected_payload,
)
from unicorn_backend.routers import run_damage_service
from unicorn_backend.store import GraphEngineStore


MAX_SELECTED_DAMAGE_TAXIDS = 250


class DamageScopeRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    files: List[str] = Field(min_length=1)
    nodes_file: Optional[str] = None
    names_file: Optional[str] = None

    @field_validator("files")
    @classmethod
    def validate_files(cls, values: List[str]) -> List[str]:
        if any(not value.strip() for value in values):
            raise ValueError("Dataset filenames must not be empty.")
        normalized = [value.strip() for value in values]
        identities = [Path(value).name for value in normalized]
        if len(identities) != len(set(identities)):
            raise ValueError("Dataset filenames must be unique.")
        return normalized


class DamageNodeRequest(DamageScopeRequest):
    taxid: int = Field(ge=1)


class DamageSelectedRequest(DamageScopeRequest):
    taxids: List[Annotated[int, Field(ge=1)]] = Field(
        min_length=1,
        max_length=MAX_SELECTED_DAMAGE_TAXIDS,
    )

    @field_validator("taxids")
    @classmethod
    def validate_taxids(cls, values: List[int]) -> List[int]:
        if len(values) != len(set(values)):
            raise ValueError("Selected damage taxids must be unique.")
        return values


def create_damage_router(*, store: GraphEngineStore) -> APIRouter:
    router = APIRouter()

    @router.post("/damage/node")
    def damage_node_route(
        payload: DamageNodeRequest,
    ) -> Dict[str, Any]:
        return damage_node(payload, store=store)

    @router.post("/damage/selected")
    def damage_selected_route(
        payload: DamageSelectedRequest,
    ) -> Dict[str, Any]:
        return damage_selected(payload, store=store)

    return router


def damage_node(
    payload: DamageNodeRequest,
    *,
    store: GraphEngineStore,
) -> Dict[str, Any]:
    return run_damage_service(
        damage_node_payload,
        store,
        taxid=payload.taxid,
        files=payload.files,
        nodes_file=payload.nodes_file,
        names_file=payload.names_file,
    )


def damage_selected(
    payload: DamageSelectedRequest,
    *,
    store: GraphEngineStore,
) -> Dict[str, Any]:
    return run_damage_service(
        damage_selected_payload,
        store,
        taxids=payload.taxids,
        files=payload.files,
        nodes_file=payload.nodes_file,
        names_file=payload.names_file,
    )
