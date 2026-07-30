"""Loading, caching, and lifecycle ownership for Graph Engine data."""

from __future__ import annotations

import csv
import logging
import math
from pathlib import Path
from threading import RLock
from typing import Any, Dict, List, Optional, Tuple

from fastapi import HTTPException

from unicorn_backend.config import BackendConfig
from unicorn_backend.damage_contract import (
    BDAMAGE_COLUMNS,
    BDAMAGE_FIT_COLUMNS,
    BDAMAGE_HEADER_COLUMNS,
    BDAMAGE_POSITION_COLUMNS,
    BDAMAGE_POSITION_COUNT,
    BDAMAGE_SCHEMA_VERSION,
)
from unicorn_backend.models import (
    DamagePosition,
    DamageProfile,
    DatasetModel,
    FileInfo,
    MetadataModel,
    SelectionModel,
    TaxonomyModel,
    TaxonomyNode,
    TreeModel,
    TreeNodeModel,
)


LOGGER = logging.getLogger("unicorn.graphengine")

_FIT_VALID_COLUMNS = (
    "A",
    "q",
    "c",
    "phi",
    "fitCT0",
    "fitGA0",
    "nll",
)


def _error_detail(
    message: str,
    *,
    code: str,
    **extra: Any,
) -> Dict[str, Any]:
    detail: Dict[str, Any] = {
        "message": message,
        "code": code,
    }
    detail.update(extra)
    return detail


def _raise_filesystem_http_error(
    path: Path,
    *,
    operation: str,
    file_role: str,
) -> None:
    try:
        path_text = str(path)
    except Exception:
        path_text = path.name
    raise HTTPException(
        status_code=403,
        detail={
            "message": (
                f"Backend could not {operation} {file_role} because the file "
                "is not readable."
            ),
            "code": "backend_file_not_readable",
            "file_role": file_role,
            "path": path_text,
            "filename": path.name,
        },
    )


def _file_info(path: Path) -> FileInfo:
    try:
        return FileInfo.from_path(path)
    except PermissionError:
        _raise_filesystem_http_error(
            path,
            operation="stat",
            file_role="backend file",
        )


def _raise_bdamage_error(
    path: Path,
    *,
    code: str,
    message: str,
    line: Optional[int] = None,
    **extra: Any,
) -> None:
    detail = _error_detail(
        message,
        code=code,
        filename=path.name,
        **extra,
    )
    if line is not None:
        detail["line"] = line
    raise HTTPException(status_code=400, detail=detail)


def _parse_non_negative_integer(
    path: Path,
    *,
    column: str,
    raw_value: str,
    line: int,
) -> int:
    try:
        value = int(raw_value.strip())
    except ValueError:
        _raise_bdamage_error(
            path,
            code="invalid_bdamage_value",
            message=(
                f"Column {column!r} must contain a non-negative integer."
            ),
            line=line,
            column=column,
            value=raw_value,
        )
    if value < 0:
        _raise_bdamage_error(
            path,
            code="invalid_bdamage_value",
            message=(
                f"Column {column!r} must contain a non-negative integer."
            ),
            line=line,
            column=column,
            value=raw_value,
        )
    return value


def _parse_damage_float(
    path: Path,
    *,
    column: str,
    raw_value: str,
    line: int,
) -> float:
    try:
        value = float(raw_value.strip())
    except ValueError:
        _raise_bdamage_error(
            path,
            code="invalid_bdamage_value",
            message=f"Column {column!r} must contain a floating-point value.",
            line=line,
            column=column,
            value=raw_value,
        )
    if math.isinf(value):
        _raise_bdamage_error(
            path,
            code="invalid_bdamage_value",
            message=f"Column {column!r} does not support infinite values.",
            line=line,
            column=column,
            value=raw_value,
        )
    return value


class GraphEngineStore:
    """Own backend files, parsed models, caches, and their lock lifecycle."""

    def __init__(self, *, config: BackendConfig) -> None:
        self.config = config
        self._dataset_cache: Dict[str, DatasetModel] = {}
        self._selection_cache: Dict[Tuple[str, ...], SelectionModel] = {}
        self._taxonomy_cache: Optional[TaxonomyModel] = None
        self._metadata_cache: Optional[MetadataModel] = None
        self._tree_cache: Dict[
            Tuple[
                Tuple[str, ...],
                Tuple[
                    str,
                    Tuple[int, float],
                    Optional[str],
                    Optional[Tuple[int, float]],
                ],
            ],
            TreeModel,
        ] = {}
        self._lock = RLock()

    def _is_bdamage_file(self, path: Path) -> bool:
        return path.is_file() and path.name.endswith(".bdamage.txt")

    def resolve_taxonomy_paths(
        self,
        nodes_name: Optional[str] = None,
        names_name: Optional[str] = None,
    ) -> Tuple[Optional[Path], Optional[Path]]:
        return (
            self._default_taxonomy_path(
                nodes_name or self.config.nodes_filename
            ),
            self._default_taxonomy_path(
                names_name or self.config.names_filename
            ),
        )

    def _default_taxonomy_path(self, filename: str) -> Optional[Path]:
        if not filename:
            return None
        candidate = Path(filename)
        if candidate.is_absolute():
            return candidate if candidate.exists() else None
        candidate = self.config.upload_dir / filename
        return candidate if candidate.exists() else None

    def _parse_dataset(self, path: Path) -> DatasetModel:
        fileinfo = _file_info(path)
        counts_map: Dict[int, int] = {}
        names_map: Dict[int, str] = {}
        counts_payload: List[Dict[str, Any]] = []
        damage_by_taxid: Dict[int, DamageProfile] = {}
        total_reads = 0
        try:
            with path.open(
                "r",
                encoding="utf-8",
                newline="",
            ) as handle:
                reader = csv.reader(
                    handle,
                    delimiter="\t",
                    quotechar='"',
                    strict=True,
                )
                raw_header: Optional[List[str]] = None
                for row in reader:
                    if not row or all(not value.strip() for value in row):
                        continue
                    raw_header = list(row)
                    break

                if raw_header is None:
                    _raise_bdamage_error(
                        path,
                        code="unsupported_bdamage_schema",
                        message=(
                            "Damage dataset is empty and does not contain "
                            "the required 43-column header."
                        ),
                    )

                normalized_header = [
                    "taxid" if value == "#taxid" else value
                    for value in raw_header
                ]
                if "#taxid" not in raw_header:
                    _raise_bdamage_error(
                        path,
                        code="unsupported_bdamage_schema",
                        message=(
                            "Damage dataset header must identify taxids "
                            "with the V1 '#taxid' column."
                        ),
                        line=reader.line_num,
                        required_column="#taxid",
                    )
                if len(normalized_header) != len(set(normalized_header)):
                    duplicates = sorted({
                        column
                        for column in normalized_header
                        if normalized_header.count(column) > 1
                    })
                    _raise_bdamage_error(
                        path,
                        code="unsupported_bdamage_schema",
                        message=(
                            "Damage dataset header contains duplicate columns."
                        ),
                        line=reader.line_num,
                        duplicate_columns=duplicates,
                    )

                missing = [
                    column
                    for column in BDAMAGE_COLUMNS
                    if column not in normalized_header
                ]
                unknown = [
                    column
                    for column in normalized_header
                    if column not in BDAMAGE_COLUMNS
                ]
                legacy_header = normalized_header == [
                    "taxid",
                    "count",
                    "name",
                ]
                if legacy_header:
                    _raise_bdamage_error(
                        path,
                        code="unsupported_bdamage_schema",
                        message=(
                            "Legacy three-column .bdamage.txt files are not "
                            "supported. Regenerate this dataset with the "
                            "current unicorn lca command."
                        ),
                        line=reader.line_num,
                        required_columns=list(BDAMAGE_HEADER_COLUMNS),
                    )
                if unknown:
                    _raise_bdamage_error(
                        path,
                        code="unsupported_bdamage_schema",
                        message=(
                            "Damage dataset header contains columns outside "
                            "the supported Unicorn V1 schema."
                        ),
                        line=reader.line_num,
                        unknown_columns=unknown,
                    )
                if missing:
                    _raise_bdamage_error(
                        path,
                        code="missing_bdamage_columns",
                        message=(
                            "Damage dataset is missing required V1 columns."
                        ),
                        line=reader.line_num,
                        missing_columns=missing,
                    )
                if len(normalized_header) != len(BDAMAGE_COLUMNS):
                    _raise_bdamage_error(
                        path,
                        code="unsupported_bdamage_schema",
                        message=(
                            "Damage dataset header does not match the "
                            "supported Unicorn V1 schema."
                        ),
                        line=reader.line_num,
                    )

                column_index = {
                    column: index
                    for index, column in enumerate(normalized_header)
                }
                for row in reader:
                    if not row or all(not value.strip() for value in row):
                        continue
                    if len(row) != len(normalized_header):
                        _raise_bdamage_error(
                            path,
                            code="invalid_bdamage_value",
                            message=(
                                "Damage dataset row has a different number "
                                "of fields than its header."
                            ),
                            line=reader.line_num,
                            expected_columns=len(normalized_header),
                            actual_columns=len(row),
                        )

                    taxid = _parse_non_negative_integer(
                        path,
                        column="taxid",
                        raw_value=row[column_index["taxid"]],
                        line=reader.line_num,
                    )
                    count = _parse_non_negative_integer(
                        path,
                        column="count",
                        raw_value=row[column_index["count"]],
                        line=reader.line_num,
                    )
                    if taxid in damage_by_taxid:
                        _raise_bdamage_error(
                            path,
                            code="duplicate_bdamage_taxid",
                            message=(
                                "Damage dataset contains more than one "
                                f"profile for taxid {taxid}."
                            ),
                            line=reader.line_num,
                            taxid=taxid,
                        )

                    clean_name = (
                        row[column_index["name"]].strip()
                        or "NA"
                    )
                    values = {
                        column: _parse_damage_float(
                            path,
                            column=column,
                            raw_value=row[column_index[column]],
                            line=reader.line_num,
                        )
                        for column in (
                            *BDAMAGE_FIT_COLUMNS,
                            *BDAMAGE_POSITION_COLUMNS,
                        )
                    }
                    positions = tuple(
                        DamagePosition(
                            position=position,
                            k5=values[f"K5_{position}"],
                            n5=values[f"N5_{position}"],
                            k3=values[f"K3_{position}"],
                            n3=values[f"N3_{position}"],
                            dx5=values[f"Dx5_{position}"],
                            dx3=values[f"Dx3_{position}"],
                        )
                        for position in range(BDAMAGE_POSITION_COUNT)
                    )
                    missing_fields = tuple(
                        column
                        for column, value in values.items()
                        if not math.isfinite(value)
                    )
                    has_evidence = any(
                        (
                            math.isfinite(position.n5)
                            and position.n5 > 0
                        )
                        or (
                            math.isfinite(position.n3)
                            and position.n3 > 0
                        )
                        for position in positions
                    )
                    fit_valid = (
                        has_evidence
                        and all(
                            math.isfinite(values[column])
                            for column in _FIT_VALID_COLUMNS
                        )
                    )
                    damage_by_taxid[taxid] = DamageProfile(
                        taxid=taxid,
                        ct_frequency=values["CTfreq"],
                        ga_frequency=values["GAfreq"],
                        amplitude=values["A"],
                        decay=values["q"],
                        background=values["c"],
                        phi=values["phi"],
                        zfit=values["Zfit"],
                        fit_ct0=values["fitCT0"],
                        fit_ga0=values["fitGA0"],
                        nll=values["nll"],
                        positions=positions,
                        fit_valid=fit_valid,
                        missing_fields=missing_fields,
                    )

                    counts_map[taxid] = count
                    if clean_name != "NA":
                        names_map[taxid] = clean_name
                    counts_payload.append(
                        {
                            "taxid": taxid,
                            "count": count,
                            "name": clean_name,
                        }
                    )
                    total_reads += count
        except PermissionError:
            _raise_filesystem_http_error(
                path,
                operation="read",
                file_role="dataset file",
            )
        except csv.Error as error:
            _raise_bdamage_error(
                path,
                code="invalid_bdamage_value",
                message=f"Damage dataset is not valid TSV: {error}.",
            )
        return DatasetModel(
            fileinfo=fileinfo,
            counts_map=counts_map,
            names_map=names_map,
            counts_payload=counts_payload,
            total_reads=total_reads,
            total_taxa=len(counts_map),
            damage_by_taxid=damage_by_taxid,
            damage_schema=BDAMAGE_SCHEMA_VERSION,
        )

    def _parse_nodes(self, path: Path) -> Dict[int, TaxonomyNode]:
        nodes: Dict[int, TaxonomyNode] = {}
        try:
            with path.open(
                "r",
                encoding="utf-8",
                errors="replace",
            ) as handle:
                for raw_line in handle:
                    line = raw_line.strip()
                    if not line:
                        continue
                    parts = [part.strip() for part in line.split("|")]
                    if len(parts) < 2:
                        continue
                    try:
                        taxid = int(parts[0])
                        parent = int(parts[1])
                    except ValueError:
                        continue
                    rank = (
                        parts[2]
                        if len(parts) > 2 and parts[2]
                        else "no rank"
                    )
                    nodes[taxid] = TaxonomyNode(
                        taxid=taxid,
                        parent=parent,
                        rank=rank,
                    )
        except PermissionError:
            _raise_filesystem_http_error(
                path,
                operation="read",
                file_role="taxonomy nodes file",
            )
        return nodes

    def _parse_names(self, path: Optional[Path]) -> Dict[int, str]:
        names: Dict[int, str] = {}
        if not path or not path.exists():
            return names
        try:
            with path.open(
                "r",
                encoding="utf-8",
                errors="replace",
            ) as handle:
                for raw_line in handle:
                    line = raw_line.strip()
                    if not line:
                        continue
                    parts = [part.strip() for part in line.split("|")]
                    if len(parts) < 2:
                        continue
                    try:
                        taxid = int(parts[0])
                    except ValueError:
                        continue
                    name = parts[1]
                    cls = parts[3] if len(parts) > 3 else ""
                    if not name:
                        continue
                    if cls == "scientific name" or taxid not in names:
                        names[taxid] = name
        except PermissionError:
            _raise_filesystem_http_error(
                path,
                operation="read",
                file_role="taxonomy names file",
            )
        return names

    def _parse_metadata(self, path: Path) -> MetadataModel:
        fileinfo = _file_info(path)
        try:
            with path.open(
                "r",
                encoding="utf-8",
                errors="replace",
                newline="",
            ) as handle:
                reader = csv.reader(handle, delimiter="\t")
                header: Optional[List[str]] = None
                rows_by_dataset: Dict[str, Dict[str, str]] = {}
                rows_total = 0
                dataset_column = -1
                fields: List[str] = []
                for raw_row in reader:
                    row = [cell.strip() for cell in raw_row]
                    if not row or not any(row):
                        continue
                    if header is None:
                        header = row
                        if "dataset" not in header:
                            raise HTTPException(
                                status_code=400,
                                detail=_error_detail(
                                    "Metadata file must contain a 'dataset' "
                                    "header column.",
                                    code="metadata_missing_dataset_column",
                                ),
                            )
                        dataset_column = header.index("dataset")
                        fields = [
                            column
                            for column in header
                            if column and column != "dataset"
                        ]
                        continue
                    if len(row) < len(header):
                        row = row + ([""] * (len(header) - len(row)))
                    elif len(row) > len(header):
                        row = row[:len(header)]
                    dataset_name = row[dataset_column].strip()
                    if not dataset_name:
                        raise HTTPException(
                            status_code=400,
                            detail=_error_detail(
                                "Metadata rows must include a non-empty "
                                "dataset value.",
                                code="metadata_missing_dataset_value",
                                row_number=rows_total + 2,
                            ),
                        )
                    dataset_key = Path(dataset_name).name
                    if dataset_key in rows_by_dataset:
                        raise HTTPException(
                            status_code=400,
                            detail=_error_detail(
                                "Metadata file contains duplicate dataset "
                                "rows.",
                                code="metadata_duplicate_dataset",
                                dataset=dataset_key,
                            ),
                        )
                    row_payload: Dict[str, str] = {}
                    for index, column in enumerate(header):
                        if not column or column == "dataset":
                            continue
                        row_payload[column] = (
                            row[index]
                            if index < len(row)
                            else ""
                        )
                    rows_by_dataset[dataset_key] = row_payload
                    rows_total += 1
                if header is None:
                    raise HTTPException(
                        status_code=400,
                        detail=_error_detail(
                            "Metadata file must contain a header row.",
                            code="metadata_missing_header",
                        ),
                    )
        except PermissionError:
            _raise_filesystem_http_error(
                path,
                operation="read",
                file_role="metadata file",
            )

        return MetadataModel(
            fileinfo=fileinfo,
            fields=tuple(fields),
            rows_by_dataset=rows_by_dataset,
            rows_total=rows_total,
        )

    def _prune_stale_caches(self, available_names: set[str]) -> None:
        stale_datasets = [
            name
            for name in self._dataset_cache
            if name not in available_names
        ]
        for name in stale_datasets:
            del self._dataset_cache[name]
        stale_selections = [
            key
            for key in self._selection_cache
            if any(name not in available_names for name in key)
        ]
        for key in stale_selections:
            del self._selection_cache[key]
        stale_trees = [
            key
            for key in self._tree_cache
            if any(name not in available_names for name in key[0])
        ]
        for key in stale_trees:
            del self._tree_cache[key]

    def list_files(self) -> List[Path]:
        with self._lock:
            files = sorted(
                path
                for path in self.config.upload_dir.iterdir()
                if self._is_bdamage_file(path)
            )
            self._prune_stale_caches({path.name for path in files})
            return files

    def get_metadata(self) -> Optional[MetadataModel]:
        metadata_path = (
            self.config.upload_dir / self.config.metadata_filename
        )
        with self._lock:
            cached = self._metadata_cache
            if not metadata_path.exists():
                self._metadata_cache = None
                return None
            current_info = _file_info(metadata_path)
            if (
                cached
                and cached.fileinfo.fingerprint()
                == current_info.fingerprint()
            ):
                return cached
        try:
            metadata = self._parse_metadata(metadata_path)
        except HTTPException as error:
            LOGGER.warning(
                "Could not auto-load backend metadata file %s: %s",
                metadata_path,
                (
                    error.detail.get("message")
                    if isinstance(error.detail, dict)
                    else error.detail
                ),
            )
            with self._lock:
                self._metadata_cache = None
            return None
        with self._lock:
            self._metadata_cache = metadata
        return metadata

    def load_metadata(self, path: Path) -> MetadataModel:
        metadata = self._parse_metadata(path)
        with self._lock:
            self._metadata_cache = metadata
        return metadata

    def metadata_summary_payload(self) -> Optional[Dict[str, Any]]:
        metadata = self.get_metadata()
        if metadata is None:
            return None
        available_dataset_names = {
            path.name
            for path in self.list_files()
        }
        return metadata.to_status_payload(available_dataset_names)

    def metadata_for_dataset(
        self,
        dataset_name: str,
    ) -> Optional[Dict[str, str]]:
        metadata = self.get_metadata()
        if metadata is None:
            return None
        row = metadata.rows_by_dataset.get(Path(dataset_name).name)
        return dict(row) if row is not None else None

    def dataset_summary_payload(
        self,
        dataset: DatasetModel,
    ) -> Dict[str, Any]:
        return dataset.to_summary_payload(
            self.metadata_for_dataset(dataset.fileinfo.name)
        )

    def selection_status_payload(
        self,
        selection: SelectionModel,
    ) -> Dict[str, Any]:
        return {
            "dataset_count": len(selection.datasets),
            "datasets": [
                self.dataset_summary_payload(dataset)
                for dataset in selection.datasets
            ],
            "total_reads": selection.total_reads,
            "total_taxa": selection.total_taxa,
            "direct_taxa": len(selection.direct_counts),
        }

    def get_or_load_dataset(self, path: Path) -> DatasetModel:
        with self._lock:
            current_info = _file_info(path)
            cached = self._dataset_cache.get(path.name)
            if (
                cached
                and cached.fileinfo.fingerprint()
                == current_info.fingerprint()
            ):
                return cached
            loaded = self._parse_dataset(path)
            self._dataset_cache[path.name] = loaded
            stale_selections = [
                key
                for key in self._selection_cache
                if path.name in key
            ]
            for key in stale_selections:
                del self._selection_cache[key]
            stale_trees = [
                key
                for key in self._tree_cache
                if path.name in key[0]
            ]
            for key in stale_trees:
                del self._tree_cache[key]
            return loaded

    def get_or_load_taxonomy(
        self,
        nodes_name: Optional[str] = None,
        names_name: Optional[str] = None,
    ) -> TaxonomyModel:
        nodes_path, names_path = self.resolve_taxonomy_paths(
            nodes_name,
            names_name,
        )
        if not nodes_path or not nodes_path.exists():
            raise HTTPException(
                status_code=404,
                detail={
                    "message": (
                        "Taxonomy nodes file not found on the backend."
                    ),
                    "nodes_file": (
                        nodes_name or self.config.nodes_filename
                    ),
                },
            )

        nodes_info = _file_info(nodes_path)
        names_info = (
            _file_info(names_path)
            if names_path and names_path.exists()
            else None
        )
        with self._lock:
            cached = self._taxonomy_cache
            if (
                cached
                and cached.nodes_fileinfo.fingerprint()
                == nodes_info.fingerprint()
                and cached.nodes_fileinfo.name == nodes_info.name
                and (
                    (not cached.names_fileinfo and not names_info)
                    or (
                        cached.names_fileinfo
                        and names_info
                        and cached.names_fileinfo.name == names_info.name
                        and cached.names_fileinfo.fingerprint()
                        == names_info.fingerprint()
                    )
                )
            ):
                return cached

        taxonomy = TaxonomyModel(
            nodes_fileinfo=nodes_info,
            names_fileinfo=names_info,
            nodes_map=self._parse_nodes(nodes_path),
            names_map=self._parse_names(names_path),
        )
        with self._lock:
            self._taxonomy_cache = taxonomy
            self._tree_cache.clear()
        return taxonomy

    def build_selection(self, names: List[str]) -> SelectionModel:
        selection_key = tuple(sorted(names))
        with self._lock:
            cached = self._selection_cache.get(selection_key)
            if cached:
                still_valid = True
                for dataset in cached.datasets:
                    path = (
                        self.config.upload_dir
                        / dataset.fileinfo.name
                    )
                    if not path.exists():
                        still_valid = False
                        break
                    current_info = _file_info(path)
                    if (
                        dataset.fileinfo.fingerprint()
                        != current_info.fingerprint()
                    ):
                        still_valid = False
                        break
                if still_valid:
                    return cached

        available = {
            path.name: path
            for path in self.list_files()
        }
        missing = [
            name
            for name in selection_key
            if name not in available
        ]
        if missing:
            raise HTTPException(
                status_code=404,
                detail={
                    "message": (
                        "Requested dataset(s) not found in upload directory."
                    ),
                    "missing": missing,
                },
            )

        datasets = [
            self.get_or_load_dataset(available[name])
            for name in selection_key
        ]
        direct_counts: Dict[int, int] = {}
        names_map: Dict[int, str] = {}
        total_reads = 0
        total_taxa = 0
        for dataset in datasets:
            total_reads += dataset.total_reads
            total_taxa += dataset.total_taxa
            for taxid, count in dataset.counts_map.items():
                direct_counts[taxid] = (
                    direct_counts.get(taxid, 0) + count
                )
            for taxid, name in dataset.names_map.items():
                names_map.setdefault(taxid, name)

        selection = SelectionModel(
            dataset_names=selection_key,
            datasets=datasets,
            direct_counts=direct_counts,
            names_map=names_map,
            total_reads=total_reads,
            total_taxa=total_taxa,
        )
        with self._lock:
            self._selection_cache[selection_key] = selection
        return selection

    def _compute_totals(
        self,
        node: TreeNodeModel,
        depth: int,
    ) -> None:
        node.depth = depth
        node.total = node.direct
        node.total_by_source = list(node.direct_by_source)
        for child in node.children:
            self._compute_totals(child, depth + 1)
            node.total += child.total
            for index, value in enumerate(child.total_by_source):
                node.total_by_source[index] += value
        node.children.sort(
            key=lambda child: (-child.total, child.name)
        )

    def build_tree_model(
        self,
        selection: SelectionModel,
        taxonomy: TaxonomyModel,
    ) -> TreeModel:
        tree_key = (selection.dataset_names, taxonomy.cache_key())
        with self._lock:
            cached = self._tree_cache.get(tree_key)
            if cached:
                return cached

        counts = selection.direct_counts
        included: set[int] = set()
        missing_taxids: set[int] = set()
        for taxid, count in counts.items():
            if not count or taxid == 0:
                continue
            if taxid not in taxonomy.nodes_map:
                missing_taxids.add(taxid)
                continue
            current = taxid
            seen: set[int] = set()
            while (
                current in taxonomy.nodes_map
                and current not in seen
            ):
                seen.add(current)
                included.add(current)
                parent = taxonomy.nodes_map[current].parent
                if not parent or parent == current:
                    break
                current = parent

        objects: Dict[int, TreeNodeModel] = {}
        for taxid in included:
            raw = taxonomy.nodes_map[taxid]
            objects[taxid] = TreeNodeModel(
                taxid=taxid,
                parent=raw.parent,
                rank=raw.rank,
                name=(
                    selection.names_map.get(taxid)
                    or taxonomy.names_map.get(taxid)
                    or str(taxid)
                ),
                direct=counts.get(taxid, 0),
                direct_by_source=[
                    dataset.counts_map.get(taxid, 0)
                    for dataset in selection.datasets
                ],
                total_by_source=[0] * len(selection.datasets),
            )

        roots: List[TreeNodeModel] = []
        for node in objects.values():
            parent = (
                objects.get(node.parent)
                if node.parent is not None
                else None
            )
            if parent and parent.taxid != node.taxid:
                parent.children.append(node)
            else:
                roots.append(node)

        unknown = counts.get(0, 0)
        if unknown:
            unclassified = TreeNodeModel(
                taxid=0,
                parent=None,
                rank="unclassified",
                name=(
                    selection.names_map.get(0)
                    or taxonomy.names_map.get(0)
                    or "unclassified"
                ),
                direct=unknown,
                direct_by_source=[
                    dataset.counts_map.get(0, 0)
                    for dataset in selection.datasets
                ],
                total_by_source=[0] * len(selection.datasets),
            )
            if len(roots) == 1:
                roots[0].children.append(unclassified)
            else:
                roots.append(unclassified)

        if len(roots) == 1:
            root = roots[0]
        else:
            root = TreeNodeModel(
                taxid=-1,
                parent=None,
                rank="synthetic root",
                name="root",
                direct=0,
                direct_by_source=[0] * len(selection.datasets),
                total_by_source=[0] * len(selection.datasets),
                children=roots,
            )

        self._compute_totals(root, 0)
        tree = TreeModel(
            root=root,
            missing_taxids=sorted(missing_taxids),
            dataset_names=selection.dataset_names,
            taxonomy_key=taxonomy.cache_key(),
        )
        with self._lock:
            self._tree_cache[tree_key] = tree
        return tree

    def cache_status(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "dataset_cache_entries": len(self._dataset_cache),
                "selection_cache_entries": len(
                    self._selection_cache
                ),
                "tree_cache_entries": len(self._tree_cache),
                "taxonomy_cached": self._taxonomy_cache is not None,
                "cached_datasets": sorted(
                    self._dataset_cache.keys()
                ),
                "cached_selections": [
                    list(key)
                    for key in sorted(self._selection_cache.keys())
                ],
            }
