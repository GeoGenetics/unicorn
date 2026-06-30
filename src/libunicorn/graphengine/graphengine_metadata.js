"use strict";

(function initUnicornGraphEngineMetadata(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const state = namespace.state;
  const core = namespace.core;

  if (!state || !core) {
    throw new Error("Unicorn graphengine metadata expected state and core modules to load first.");
  }

  const NONE_VISUALIZATION_MODE = "none";
  const FIELD_MODE_PREFIX = "field:";
  const MISSING_METADATA_VALUE = "__missing__";
  const MISSING_METADATA_COLOR = "#9cad9f";
  const MODE_STORAGE_KEY = "unicorn.metadataVisualizationMode";
  const VALUE_COLORS_STORAGE_KEY = "unicorn.metadataValueColors";

  function getAvailableMetadataFields(metadata = state.remote.metadata) {
    return Array.isArray(metadata?.fields) ? metadata.fields.map((value) => String(value)) : [];
  }

  function normalizeMetadataVisualizationMode(mode, availableFields = getAvailableMetadataFields()) {
    const candidate = String(mode || NONE_VISUALIZATION_MODE).trim();
    if (candidate === NONE_VISUALIZATION_MODE) {
      return NONE_VISUALIZATION_MODE;
    }
    if (!candidate.startsWith(FIELD_MODE_PREFIX)) {
      return NONE_VISUALIZATION_MODE;
    }
    const field = candidate.slice(FIELD_MODE_PREFIX.length).trim();
    if (!field || !availableFields.includes(field)) {
      return NONE_VISUALIZATION_MODE;
    }
    return `${FIELD_MODE_PREFIX}${field}`;
  }

  function readStoredMetadataVisualizationMode() {
    try {
      return String(localStorage.getItem(MODE_STORAGE_KEY) || NONE_VISUALIZATION_MODE);
    } catch (_error) {
      return NONE_VISUALIZATION_MODE;
    }
  }

  function writeStoredMetadataVisualizationMode(mode) {
    localStorage.setItem(MODE_STORAGE_KEY, mode);
  }

  function readStoredMetadataValueColors() {
    try {
      const raw = localStorage.getItem(VALUE_COLORS_STORAGE_KEY);
      const parsed = raw ? JSON.parse(raw) : {};
      return parsed && typeof parsed === "object" && !Array.isArray(parsed) ? parsed : {};
    } catch (_error) {
      return {};
    }
  }

  function writeStoredMetadataValueColors(colors) {
    localStorage.setItem(VALUE_COLORS_STORAGE_KEY, JSON.stringify(colors));
  }

  function normalizeColor(value, fallback) {
    const candidate = String(value || "").trim();
    return /^#[0-9a-fA-F]{6}$/.test(candidate) ? candidate : fallback;
  }

  function getActiveMetadataVisualizationField(mode = state.remote.metadataVisualizationMode) {
    const normalized = normalizeMetadataVisualizationMode(mode);
    if (normalized === NONE_VISUALIZATION_MODE) {
      return null;
    }
    return normalized.slice(FIELD_MODE_PREFIX.length);
  }

  function getMetadataVisualizationMode() {
    return normalizeMetadataVisualizationMode(state.remote.metadataVisualizationMode);
  }

  function setMetadataVisualizationMode(mode, availableFields = getAvailableMetadataFields()) {
    const normalized = normalizeMetadataVisualizationMode(mode, availableFields);
    state.remote.metadataVisualizationMode = normalized;
    writeStoredMetadataVisualizationMode(normalized);
    return normalized;
  }

  function ensureMetadataVisualizationState(metadata = state.remote.metadata) {
    const availableFields = getAvailableMetadataFields(metadata);
    const storedMode = readStoredMetadataVisualizationMode();
    let normalizedMode = normalizeMetadataVisualizationMode(storedMode, availableFields);
    if (normalizedMode === NONE_VISUALIZATION_MODE && availableFields.length) {
      normalizedMode = `${FIELD_MODE_PREFIX}${availableFields[0]}`;
    }
    state.remote.metadataVisualizationMode = normalizedMode;
    writeStoredMetadataVisualizationMode(state.remote.metadataVisualizationMode);

    const storedColors = readStoredMetadataValueColors();
    const nextColors = {};
    for (const [field, values] of Object.entries(storedColors)) {
      if (!availableFields.includes(field) || !values || typeof values !== "object" || Array.isArray(values)) {
        continue;
      }
      nextColors[field] = {};
      for (const [value, color] of Object.entries(values)) {
        nextColors[field][value] = normalizeColor(
          color,
          value === MISSING_METADATA_VALUE ? MISSING_METADATA_COLOR : core.SOURCE_COLORS[0],
        );
      }
    }
    state.remote.metadataValueColors = nextColors;
    writeStoredMetadataValueColors(nextColors);
  }

  function getDatasetMetadataValue(datasetSummary, field) {
    if (!field || !datasetSummary || typeof datasetSummary !== "object") {
      return MISSING_METADATA_VALUE;
    }
    const metadata = datasetSummary.metadata;
    if (!metadata || typeof metadata !== "object") {
      return MISSING_METADATA_VALUE;
    }
    const rawValue = metadata[field];
    if (rawValue == null) {
      return MISSING_METADATA_VALUE;
    }
    const value = String(rawValue).trim();
    return value ? value : MISSING_METADATA_VALUE;
  }

  function ensureFieldValueColorAssignments(field, values) {
    if (!field) return {};
    if (!state.remote.metadataValueColors[field] || typeof state.remote.metadataValueColors[field] !== "object") {
      state.remote.metadataValueColors[field] = {};
    }
    const mapping = state.remote.metadataValueColors[field];
    const orderedValues = Array.from(new Set(values.map((value) => String(value))))
      .filter((value) => value && value !== MISSING_METADATA_VALUE)
      .sort((left, right) => left.localeCompare(right));
    let assignedCount = Object.keys(mapping).filter((value) => value !== MISSING_METADATA_VALUE).length;
    for (const value of orderedValues) {
      if (!mapping[value]) {
        mapping[value] = core.SOURCE_COLORS[assignedCount % core.SOURCE_COLORS.length];
        assignedCount++;
      }
    }
    mapping[MISSING_METADATA_VALUE] = normalizeColor(
      mapping[MISSING_METADATA_VALUE],
      MISSING_METADATA_COLOR,
    );
    state.remote.metadataValueColors = {
      ...state.remote.metadataValueColors,
      [field]: mapping,
    };
    writeStoredMetadataValueColors(state.remote.metadataValueColors);
    return mapping;
  }

  function getMetadataValueColor(field, value) {
    if (!field || value == null) {
      return MISSING_METADATA_COLOR;
    }
    const normalizedValue = String(value);
    const mapping = ensureFieldValueColorAssignments(field, [normalizedValue]);
    return normalizeColor(mapping[normalizedValue], MISSING_METADATA_COLOR);
  }

  function setMetadataValueColor(field, value, color) {
    if (!field || value == null) {
      return MISSING_METADATA_COLOR;
    }
    const normalizedValue = String(value);
    const fallback = normalizedValue === MISSING_METADATA_VALUE
      ? MISSING_METADATA_COLOR
      : core.SOURCE_COLORS[0];
    const nextColor = normalizeColor(color, fallback);
    const mapping = ensureFieldValueColorAssignments(field, [normalizedValue]);
    mapping[normalizedValue] = nextColor;
    state.remote.metadataValueColors = {
      ...state.remote.metadataValueColors,
      [field]: mapping,
    };
    writeStoredMetadataValueColors(state.remote.metadataValueColors);
    return nextColor;
  }

  function buildResolvedDatasetLabel(datasetSummary, field, value, missing) {
    const filename = String(datasetSummary?.filename || "");
    if (!field || value == null) {
      return filename;
    }
    if (missing) {
      return `${filename} [${field}: missing]`;
    }
    return `${filename} [${field}: ${value}]`;
  }

  function resolveDatasetMetadataDisplay(datasetSummary, options = {}) {
    const availableFields = Array.isArray(options.availableFields) ? options.availableFields : getAvailableMetadataFields();
    const mode = normalizeMetadataVisualizationMode(options.mode ?? state.remote.metadataVisualizationMode, availableFields);
    const field = getActiveMetadataVisualizationField(mode);
    if (!field) {
      return {
        filename: String(datasetSummary?.filename || ""),
        metadata_field: null,
        metadata_value: null,
        color: String(datasetSummary?.color || core.SOURCE_COLORS[0]),
        label: String(datasetSummary?.filename || ""),
        missing: false,
      };
    }

    const metadataValue = getDatasetMetadataValue(datasetSummary, field);
    const missing = metadataValue === MISSING_METADATA_VALUE;
    return {
      filename: String(datasetSummary?.filename || ""),
      metadata_field: field,
      metadata_value: metadataValue,
      color: getMetadataValueColor(field, metadataValue),
      label: buildResolvedDatasetLabel(datasetSummary, field, metadataValue, missing),
      missing,
    };
  }

  function resolveDatasetMetadataDisplays(datasetSummaries, options = {}) {
    const summaries = Array.isArray(datasetSummaries) ? datasetSummaries : [];
    const availableFields = Array.isArray(options.availableFields) ? options.availableFields : getAvailableMetadataFields();
    const mode = normalizeMetadataVisualizationMode(options.mode ?? state.remote.metadataVisualizationMode, availableFields);
    const field = getActiveMetadataVisualizationField(mode);
    if (field) {
      const values = summaries.map((datasetSummary) => getDatasetMetadataValue(datasetSummary, field));
      ensureFieldValueColorAssignments(field, values);
    }
    return summaries.map((datasetSummary) => resolveDatasetMetadataDisplay(datasetSummary, { mode, availableFields }));
  }

  function buildMetadataLegendPayload(datasetSummaries, options = {}) {
    const resolved = resolveDatasetMetadataDisplays(datasetSummaries, options);
    if (!resolved.length || !resolved[0].metadata_field) {
      return null;
    }
    const field = resolved[0].metadata_field;
    const values = Array.from(new Set([
      ...resolved.map((entry) => entry.metadata_value),
      MISSING_METADATA_VALUE,
    ]))
      .sort((left, right) => {
        if (left === MISSING_METADATA_VALUE) return 1;
        if (right === MISSING_METADATA_VALUE) return -1;
        return String(left).localeCompare(String(right));
      })
      .map((value) => ({
        value,
        color: getMetadataValueColor(field, value),
      }));
    return {
      field,
      values,
    };
  }

  namespace.metadata = {
    NONE_VISUALIZATION_MODE,
    FIELD_MODE_PREFIX,
    MISSING_METADATA_VALUE,
    MISSING_METADATA_COLOR,
    getAvailableMetadataFields,
    normalizeMetadataVisualizationMode,
    getMetadataVisualizationMode,
    setMetadataVisualizationMode,
    getActiveMetadataVisualizationField,
    ensureMetadataVisualizationState,
    getDatasetMetadataValue,
    getMetadataValueColor,
    setMetadataValueColor,
    resolveDatasetMetadataDisplay,
    resolveDatasetMetadataDisplays,
    buildMetadataLegendPayload,
  };
})(window);
