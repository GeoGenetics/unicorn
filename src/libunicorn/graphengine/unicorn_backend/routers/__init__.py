"""FastAPI routers for the Unicorn Graph Engine backend."""

from __future__ import annotations

from typing import Any, Dict, Optional

from fastapi import HTTPException

from unicorn_backend.damage import DamageServiceError
from unicorn_backend.reports import ReportServiceError
from unicorn_backend.tree import TreeServiceError


def run_tree_service(operation, *args, **kwargs):
    try:
        return operation(*args, **kwargs)
    except TreeServiceError as error:
        raise HTTPException(
            status_code=error.status_code,
            detail=error.detail,
        ) from error


def run_report_service(operation, *args, **kwargs):
    try:
        return operation(*args, **kwargs)
    except (ReportServiceError, TreeServiceError) as error:
        raise HTTPException(
            status_code=error.status_code,
            detail=error.detail,
        ) from error


def run_damage_service(operation, *args, **kwargs):
    try:
        return operation(*args, **kwargs)
    except (DamageServiceError, TreeServiceError) as error:
        raise HTTPException(
            status_code=error.status_code,
            detail=error.detail,
        ) from error


def error_detail(
    message: str,
    *,
    code: str,
    request_context: Optional[Dict[str, Any]] = None,
    **extra: Any,
) -> Dict[str, Any]:
    detail: Dict[str, Any] = {
        "message": message,
        "code": code,
    }
    if request_context is not None:
        detail["request_context"] = request_context
    detail.update(extra)
    return detail
