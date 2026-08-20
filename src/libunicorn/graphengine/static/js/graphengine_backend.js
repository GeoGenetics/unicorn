"use strict";

(function initUnicornGraphEngineBackend(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const state = namespace.state;
  const els = namespace.els;
  const core = namespace.core;
  const metadataResolver = namespace.metadata;
  const treeModel = namespace.treeModel;

  if (!state || !els || !core || !metadataResolver || !treeModel) {
    throw new Error("Unicorn graphengine backend expected state, DOM, core, metadata, and tree model modules to load first.");
  }

  const {
    buildRemoteTree,
  } = treeModel;

  function getUi() {
    return namespace.ui || null;
  }

  function resetTreeViewportSafe() {
    namespace.treeViewport?.resetZoom?.();
  }

  function getMinReadsValueSafe() {
    return typeof getUi()?.getMinReadsValue === "function"
      ? getUi().getMinReadsValue()
      : 0;
  }

  function syncMinReadsControlSafe() {
    if (typeof getUi()?.syncMinReadsControl === "function") {
      getUi().syncMinReadsControl();
    }
  }

  function renderSourceLegendSafe() {
    if (typeof getUi()?.renderSourceLegend === "function") {
      getUi().renderSourceLegend();
    }
  }

  function getVisibleSeriesSafe() {
    return typeof getUi()?.getVisibleSeries === "function"
      ? getUi().getVisibleSeries()
      : [];
  }

  function colorForSource(index) {
    return core.SOURCE_COLORS[index % core.SOURCE_COLORS.length];
  }

  function readStoredMetadataFieldSelection() {
    try {
      const raw = localStorage.getItem("unicorn.metadataFieldSelection");
      const parsed = raw ? JSON.parse(raw) : [];
      return Array.isArray(parsed) ? parsed.map((value) => String(value)) : [];
    } catch (_error) {
      return [];
    }
  }

  function writeStoredMetadataFieldSelection(fields) {
    localStorage.setItem("unicorn.metadataFieldSelection", JSON.stringify(fields));
  }

  function ensureMetadataFieldPreferences(metadata) {
    const fields = Array.isArray(metadata?.fields) ? metadata.fields.map((value) => String(value)) : [];
    if (!fields.length) {
      state.remote.metadataFieldSelection = [];
      return;
    }

    const storedSelection = readStoredMetadataFieldSelection();
    const nextSelection = storedSelection.filter((field) => fields.includes(field));
    state.remote.metadataFieldSelection = nextSelection.length ? nextSelection : fields.slice();
    writeStoredMetadataFieldSelection(state.remote.metadataFieldSelection);
  }

  function getSelectedMetadataFields() {
    const metadataFields = Array.isArray(state.remote.metadata?.fields) ? state.remote.metadata.fields : [];
    return state.remote.metadataFieldSelection.filter((field) => metadataFields.includes(field));
  }

  function buildMetadataChipHtml(key, value, color, index) {
    const fallback = core.SOURCE_COLORS[index % core.SOURCE_COLORS.length];
    const safeColor = /^#[0-9a-fA-F]{6}$/.test(String(color || "").trim()) ? String(color).trim() : fallback;
    return `<span class="metadata-chip" style="--metadata-chip-color:${core.escapeHtml(safeColor)}"><strong>${core.escapeHtml(key)}</strong>: ${core.escapeHtml(String(value || ""))}</span>`;
  }

  function handleMetadataFieldSelectionChange() {
    if (!els.metadataFieldSelect) return;
    state.remote.metadataFieldSelection = Array.from(els.metadataFieldSelect.selectedOptions).map((option) => option.value);
    writeStoredMetadataFieldSelection(state.remote.metadataFieldSelection);
    renderMetadataSummary();
  }

  function handleMetadataColorFieldChange() {
    if (!els.metadataColorFieldSelect) return;
    const field = String(els.metadataColorFieldSelect.value || "").trim();
    const mode = field ? `${metadataResolver.FIELD_MODE_PREFIX}${field}` : metadataResolver.NONE_VISUALIZATION_MODE;
    metadataResolver.setMetadataVisualizationMode(mode);
    applyMetadataColorsToSeries(state.series, getActiveSeriesRemoteDatasets());
    renderSourceLegendSafe();
    if (state.tree) globalObject.redraw();
    renderMetadataSummary();
  }

  function handleMetadataValueColorInput(field, value, color) {
    if (!field || value == null) return;
    metadataResolver.setMetadataValueColor(field, value, color);
    applyMetadataColorsToSeries(state.series, getActiveSeriesRemoteDatasets());
    renderSourceLegendSafe();
    if (state.tree) globalObject.redraw();
    renderMetadataSummary();
  }

  function initMetadataControls() {
    if (els.metadataFieldSelect && els.metadataFieldSelect.dataset.bound !== "1") {
      els.metadataFieldSelect.dataset.bound = "1";
      els.metadataFieldSelect.addEventListener("change", handleMetadataFieldSelectionChange);
    }
    if (els.metadataColorFieldSelect && els.metadataColorFieldSelect.dataset.bound !== "1") {
      els.metadataColorFieldSelect.dataset.bound = "1";
      els.metadataColorFieldSelect.addEventListener("change", handleMetadataColorFieldChange);
    }
  }

  function getActiveBackendRequestContext() {
    return normalizeProviderRequestContext(state.remote.requestContext);
  }

  function updateActiveBackendRequestContext(requestContext, options = {}) {
    const normalized = normalizeProviderRequestContext(requestContext);
    if (!normalized) return;
    const preserveExpandedTaxids = options.preserveExpandedTaxids !== false;
    const previous = getActiveBackendRequestContext();
    if (preserveExpandedTaxids && previous && (!normalized.expanded_taxids || !normalized.expanded_taxids.length)) {
      normalized.expanded_taxids = previous.expanded_taxids.slice();
    }
    state.remote.requestContext = normalized;
    if (normalized.nodes_file) {
      state.remote.backendNodesFile = normalized.nodes_file;
    }
    if (normalized.names_file || normalized.names_file === null) {
      state.remote.backendNamesFile = normalized.names_file || "";
    }
  }

  function normalizeRemoteMetadata(metadata) {
    if (!metadata || typeof metadata !== "object") {
      return null;
    }
    return {
      filename: String(metadata.filename || ""),
      fields: Array.isArray(metadata.fields) ? metadata.fields.map((value) => String(value)) : [],
      rows_total: Number(metadata.rows_total || 0),
      matched_rows: Number(metadata.matched_rows || 0),
      unmatched_rows: Number(metadata.unmatched_rows || 0),
      datasets_with_metadata: Array.isArray(metadata.datasets_with_metadata)
        ? metadata.datasets_with_metadata.map((value) => String(value))
        : [],
    };
  }

  function normalizeProviderRequestContext(requestContext) {
    if (!requestContext || typeof requestContext !== "object") {
      return null;
    }
    return {
      dataset_names: Array.isArray(requestContext.dataset_names)
        ? requestContext.dataset_names.map((name) => String(name))
        : [],
      nodes_file: requestContext.nodes_file ? String(requestContext.nodes_file) : null,
      names_file: requestContext.names_file ? String(requestContext.names_file) : null,
      min_reads: Number(requestContext.min_reads || 0),
      expanded_taxids: Array.isArray(requestContext.expanded_taxids)
        ? requestContext.expanded_taxids.map((value) => Number(value))
        : [],
      taxonomy_only: Boolean(requestContext.taxonomy_only),
    };
  }

  async function connectRemote() {
    const user = (els.remoteUser.value || "").trim() || "youruser";
    const host = (els.remoteHost.value || "").trim() || "remote-server";
    localStorage.setItem("unicorn.remoteUser", user);
    localStorage.setItem("unicorn.remoteHost", host);
    updateTunnelHint();
    updateConnectionState(false, "Testing tunnel...");
    addClientLog("info", "connect", `Testing backend tunnel via localhost for ${user}@${host}.`);
    try {
      const response = await fetch("http://localhost:8000/ping", {
        method: "GET",
      });
      if (!response.ok) {
        addClientLog("error", "connect", "Backend ping request failed.", `HTTP ${response.status}`);
        throw new Error(`Backend ping request failed with HTTP ${response.status}`);
      }
      const payload = await response.json();
      state.remote.connected = true;
      updateConnectionState(true, "Connected");
      updateRemoteServerStatus(payload);
      els.remoteDatasetsPanel.hidden = false;
      await refreshRemoteDatasets({ selectAll: true });
      setStatus(backendConnectionReadyMessage(user, host));
      addClientLog("success", "connect", "Backend tunnel test succeeded.", JSON.stringify(payload, null, 2));
    } catch (error) {
      state.remote.connected = false;
      state.remote.datasets = [];
      state.remote.selectedDatasets.clear();
      state.remote.backendNodesFile = "";
      state.remote.backendNamesFile = "";
      state.remote.metadata = null;
      state.remote.requestContext = null;
      state.remote.expandedTaxids.clear();
      state.remote.activeExpandedTaxids.clear();
      state.remote.taxonomyOnly = false;
      updateConnectionState(false, "Could not reach backend");
      els.remoteDatasetsPanel.hidden = true;
      renderRemoteDatasets();
      renderMetadataSummary();
      setStatus(`Could not reach the backend over localhost:8000. ${error.message || error}`);
      addClientLog("error", "connect", "Backend tunnel test failed.", core.errorToDetail(error));
    }
    updateRenderAvailability();
  }

  function updateTunnelHint() {
    const user = (els.remoteUser.value || "youruser").trim() || "youruser";
    const host = (els.remoteHost.value || "remote-server").trim() || "remote-server";
    els.tunnelCommand.textContent = `ssh -L 8000:localhost:8000 ${user}@${host}`;
  }

  function updateConnectionState(connected, message) {
    state.remote.connected = Boolean(connected);
    els.connectionState.textContent = message;
    els.connectionState.classList.toggle("online", connected);
    els.connectionState.classList.toggle("offline", !connected);
    els.remoteDatasetsPanel.hidden = !connected;
    if (!connected) {
      state.remote.datasets = [];
      state.remote.selectedDatasets.clear();
      state.remote.metadata = null;
      state.remote.metadataFieldSelection = [];
      state.remote.requestContext = null;
    }
    syncBackendRuntimeUiState();
  }

  async function uploadLoadedFiles() {
    if (!state.remote.connected) {
      setStatus("Connect to the backend before uploading files.");
      return;
    }
    const files = getLocalRemoteUploadFiles();
    const metadataFile = getLocalRemoteMetadataFile();
    if (!files.length && !metadataFile) {
      setStatus("Choose one or more local files first.");
      return;
    }
    const fileCount = files.length + (metadataFile ? 1 : 0);
    addClientLog("info", "upload", `Uploading ${fileCount.toLocaleString()} local file${fileCount === 1 ? "" : "s"} to the backend.`);
    try {
      const uploaded = files.length ? await uploadFilesToRemote(files) : 0;
      if (metadataFile) {
        await uploadMetadataToRemote(metadataFile);
      }
      await refreshRemoteServerStatus();
      await refreshRemoteDatasets();
      clearRemoteDatasetInputs();
      setStatus(`Uploaded ${fileCount.toLocaleString()} file${fileCount === 1 ? "" : "s"} to the backend.`);
    } catch (error) {
      setStatus(`Could not upload files to the backend: ${error.message || error}`);
    }
  }

  async function copyTunnelCommand() {
    const command = els.tunnelCommand.textContent || "";
    try {
      await navigator.clipboard.writeText(command);
      setStatus("Copied SSH tunnel command to the clipboard.");
    } catch (error) {
      setStatus(`Could not copy SSH tunnel command: ${error.message || error}`);
    }
  }

  async function loadAndRender() {
    if (!canRenderRemoteTree()) {
      setStatus(remoteRenderUnavailableMessage());
      return;
    }
    try {
      await loadAndRenderBackend();
    } catch (error) {
      setStatus(`Could not render backend tree: ${error.message || error}`);
      addClientLog("error", "tree", "Backend render failed.", core.errorToDetail(error));
    }
  }

  function getLocalRemoteUploadFiles() {
    const datasetFiles = getLocalRemoteDatasetUploadFiles();
    const taxonomyFiles = [els.nodesFile.files[0], els.namesFile.files[0]].filter(Boolean);
    return [...datasetFiles, ...taxonomyFiles];
  }

  function getLocalRemoteMetadataFile() {
    return els.metadataFile?.files?.[0] || null;
  }

  function getLocalRemoteDatasetUploadFiles() {
    return Array.from(document.querySelectorAll(".lca-file-input"))
      .map((input) => input.files && input.files[0])
      .filter(Boolean);
  }

  async function uploadFilesToRemote(files) {
    let uploaded = 0;
    for (const file of files) {
      const formData = new FormData();
      formData.append("file", file, file.name);
      addClientLog("info", "upload", `Uploading ${file.name} to backend.`);
      const response = await fetch("http://localhost:8000/upload", {
        method: "POST",
        body: formData,
      });
      if (!response.ok) {
        addClientLog("error", "upload", `Upload failed for ${file.name}.`, `HTTP ${response.status}`);
        throw new Error(`Upload failed for ${file.name} with HTTP ${response.status}`);
      }
      addClientLog("success", "upload", `Upload finished for ${file.name}.`);
      uploaded++;
    }
    return uploaded;
  }

  async function uploadMetadataToRemote(file) {
    const formData = new FormData();
    formData.append("file", file, file.name);
    addClientLog("info", "upload", `Uploading metadata ${file.name} to backend.`);
    const response = await fetch("http://localhost:8000/metadata/upload", {
      method: "POST",
      body: formData,
    });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok) {
      addClientLog("error", "upload", `Metadata upload failed for ${file.name}.`, payload?.detail?.message || `HTTP ${response.status}`);
      throw new Error(payload?.detail?.message || `Metadata upload failed for ${file.name} with HTTP ${response.status}`);
    }
    state.remote.metadata = normalizeRemoteMetadata(payload?.metadata);
    applyMetadataColorsToSeries(
      state.series,
      getActiveSeriesRemoteDatasets(),
    );
    renderSourceLegendSafe();
    addClientLog("success", "upload", `Metadata upload finished for ${file.name}.`, state.remote.metadata?.filename || "metadata.txt");
    return payload;
  }

  async function loadAndRenderBackend() {
    const localFiles = getLocalRemoteUploadFiles();
    const metadataFile = getLocalRemoteMetadataFile();
    if (localFiles.length) {
      await uploadFilesToRemote(localFiles);
      if (metadataFile) {
        await uploadMetadataToRemote(metadataFile);
      }
      await refreshRemoteServerStatus();
      await refreshRemoteDatasets();
      for (const file of localFiles) {
        if (file.name.endsWith(".bdamage.txt")) state.remote.selectedDatasets.add(file.name);
      }
      clearRemoteDatasetInputs();
    } else if (metadataFile) {
      await uploadMetadataToRemote(metadataFile);
      await refreshRemoteServerStatus();
      await refreshRemoteDatasets();
      clearRemoteDatasetInputs();
    }

    const files = getSelectedRemoteDatasets();
    const taxonomyOnly = files.length === 0;

    state.remote.expandedTaxids.clear();
    state.remote.activeExpandedTaxids.clear();
    addClientLog(
      "info",
      "tree",
      taxonomyOnly
        ? "Requesting taxonomy-only backend tree view."
        : `Requesting backend root tree view for ${files.length.toLocaleString()} dataset${files.length === 1 ? "" : "s"}.`,
      taxonomyOnly ? "No datasets selected." : files.join("\n"),
    );
    const payload = await fetchRemoteVisibleTree({
      datasetNames: files,
      nodesFile: els.nodesFile.files[0] ? els.nodesFile.files[0].name : null,
      namesFile: els.namesFile.files[0] ? els.namesFile.files[0].name : null,
      minReads: getMinReadsValueSafe(),
      expandedTaxids: [],
      taxonomyOnly,
    });
    if (!payload.tree) {
      addClientLog("error", "tree", "The backend returned no tree payload.");
      setStatus("The backend returned no tree to render.");
      return;
    }

    resetTreeViewportSafe();
    applyRemoteVisiblePayload(payload);
    state.centerOnNextRender = true;
    addClientLog(
      "success",
      "tree",
      taxonomyOnly
        ? "Loaded taxonomy-only backend tree."
        : `Loaded backend tree with ${Number(payload.direct_taxa || 0).toLocaleString()} direct taxa.`,
    );
    setStatus(taxonomyOnly
      ? "Loaded taxonomy-only tree. Counts, reports, damage, and compute views require datasets."
      : `Loaded backend tree for ${state.series.length.toLocaleString()} dataset${state.series.length === 1 ? "" : "s"} and ${state.remote.directTaxa.toLocaleString()} direct taxa.`);
    globalObject.redraw();
  }

  function clearRemoteDatasetInputs() {
    document.querySelectorAll(".lca-file-input").forEach((input) => {
      input.value = "";
    });
    if (els.lcaListFile) {
      els.lcaListFile.value = "";
    }
    if (els.metadataFile) {
      els.metadataFile.value = "";
    }
  }

  function buildSeriesFromRemoteDatasets(datasets) {
    const previousVisibility = new Map(
      state.series.map((source) => [source.label, Boolean(source.visible)]),
    );
    const nextSeries = datasets.map((dataset, index) => {
      const label = dataset.filename || dataset.id || `remote-dataset-${index + 1}`;
      return {
        label,
        color: colorForSource(index),
        visible: previousVisibility.has(label) ? previousVisibility.get(label) : true,
      };
    });
    applyMetadataColorsToSeries(nextSeries, datasets);
    return nextSeries;
  }

  function applyMetadataColorsToSeries(series, datasets) {
    const safeSeries = Array.isArray(series) ? series : [];
    const safeDatasets = Array.isArray(datasets) ? datasets : [];
    if (!safeSeries.length || !safeDatasets.length) {
      return safeSeries;
    }
    const summaries = safeDatasets.map((dataset, index) => ({
      filename: String(dataset?.filename || dataset?.id || `remote-dataset-${index + 1}`),
      color: safeSeries[index]?.color || colorForSource(index),
      metadata: dataset?.metadata && typeof dataset.metadata === "object" ? { ...dataset.metadata } : null,
    }));
    const resolved = metadataResolver.resolveDatasetMetadataDisplays(summaries);
    const colorByFilename = new Map(
      resolved.map((entry) => [entry.filename, entry.color]),
    );
    safeSeries.forEach((source, index) => {
      const fallback = colorForSource(index);
      source.color = colorByFilename.get(source.label) || fallback;
    });
    return safeSeries;
  }

  async function handleMinReadsChange() {
    getUi()?.setMinReadsValue?.(getUi()?.valueFromSliderPosition?.(els.minReads.value));
    if (hasBackendTree()) {
      try {
        const applied = await refreshRemoteTreeForMinReads();
        if (!applied) return;
        await globalObject.refreshCurrentReportIfNeeded();
        globalObject.redraw();
        return;
      } catch (error) {
        setStatus(`Could not refresh backend tree after changing the read filter: ${error.message || error}`);
        return;
      }
    }
    globalObject.redraw();
  }

  function hasBackendTree() {
    return state.remote.connected && state.remote.serverTreeActive && Boolean(state.tree);
  }

  function buildRemoteContextUrl(path, options = {}) {
    const url = new URL(`http://localhost:8000/${path}`);
    const activeRequestContext = getActiveBackendRequestContext();
    const files = Array.isArray(options.datasetNames)
      ? options.datasetNames
      : activeRequestContext?.dataset_names?.length
        ? activeRequestContext.dataset_names
        : getSelectedRemoteDatasets();
    for (const file of files) url.searchParams.append("files", file);
    const nodesFile = options.nodesFile ?? activeRequestContext?.nodes_file ?? (els.nodesFile.files[0] ? els.nodesFile.files[0].name : null);
    const namesFile = options.namesFile ?? activeRequestContext?.names_file ?? (els.namesFile.files[0] ? els.namesFile.files[0].name : null);
    const minReads = options.minReads != null
      ? Number(options.minReads)
      : activeRequestContext && Number.isFinite(activeRequestContext.min_reads)
        ? Number(activeRequestContext.min_reads)
        : getMinReadsValueSafe();
    if (nodesFile) url.searchParams.set("nodes_file", nodesFile);
    if (namesFile) url.searchParams.set("names_file", namesFile);
    url.searchParams.set("min_reads", String(minReads));
    const taxonomyOnly = options.taxonomyOnly != null
      ? Boolean(options.taxonomyOnly)
      : Boolean(activeRequestContext?.taxonomy_only || state.remote.taxonomyOnly);
    if (taxonomyOnly) url.searchParams.set("taxonomy_only", "true");
    if (options.includeExpanded === true) {
      if (Array.isArray(options.expandedTaxids)) {
        for (const taxid of options.expandedTaxids) {
          url.searchParams.append("expanded", String(taxid));
        }
      } else if (activeRequestContext?.expanded_taxids?.length) {
        for (const taxid of activeRequestContext.expanded_taxids) {
          url.searchParams.append("expanded", String(taxid));
        }
      }
    }
    for (const [key, value] of Object.entries(options.query || {})) {
      if (value == null) continue;
      url.searchParams.set(key, String(value));
    }
    return url;
  }

  async function fetchRemoteVisibleTree(options = {}) {
    const endpoint = options.taxid != null ? "expand-node" : "root-view";
    const url = buildRemoteContextUrl(endpoint, {
      includeExpanded: true,
      expandedTaxids: options.expandedTaxids || Array.from(state.remote.expandedTaxids),
      datasetNames: options.datasetNames,
      nodesFile: options.nodesFile,
      namesFile: options.namesFile,
      minReads: options.minReads,
      taxonomyOnly: options.taxonomyOnly,
      query: {
        ...(options.query && typeof options.query === "object" ? options.query : {}),
        ...(options.taxid != null ? { taxid: options.taxid } : {}),
      },
    }).toString();
    addClientLog("info", "tree", `GET /${endpoint}`, url);
    let response;
    try {
      response = await fetch(url, { method: "GET" });
    } catch (error) {
      addClientLog("error", "tree", `Remote ${endpoint} fetch failed.`, core.errorToDetail(error));
      throw error;
    }
    if (!response.ok) {
      addClientLog("error", "tree", `Remote ${endpoint} request failed.`, `HTTP ${response.status}`);
      throw new Error(`Remote ${endpoint} request failed with HTTP ${response.status}`);
    }
    addClientLog("success", "tree", `Remote ${endpoint} request succeeded.`);
    return response.json();
  }

  async function postRemoteVisibleTree(options = {}) {
    const body = buildRemoteContextPayload(options);
    addClientLog(
      "info",
      "tree",
      "POST /tree-view",
      `selected_datasets=${body.files.length.toLocaleString()} requested_expanded_taxids=${body.expanded_taxids.length.toLocaleString()}`,
    );
    let response;
    try {
      response = await fetch("http://localhost:8000/tree-view", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
        },
        body: JSON.stringify(body),
      });
    } catch (error) {
      addClientLog("error", "tree", "Remote tree-view fetch failed.", core.errorToDetail(error));
      throw error;
    }
    const payload = await response.json().catch(() => ({}));
    if (!response.ok) {
      addClientLog("error", "tree", "Remote tree-view request failed.", `HTTP ${response.status}`);
      throw new Error(`Remote tree-view request failed with HTTP ${response.status}`);
    }
    addClientLog("success", "tree", "Remote tree-view request succeeded.");
    return payload;
  }

  async function refreshRemoteTreeForMinReads() {
    const generation = state.remote.treeRefreshGeneration + 1;
    state.remote.treeRefreshGeneration = generation;
    const payload = await postRemoteVisibleTree({
      minReads: getMinReadsValueSafe(),
      expandedTaxids: Array.from(state.remote.expandedTaxids),
      taxonomyOnly: state.remote.taxonomyOnly,
    });
    if (generation !== state.remote.treeRefreshGeneration) {
      return false;
    }
    applyRemoteVisiblePayload(payload);
    return true;
  }

  function buildRemoteContextPayload(options = {}) {
    const activeRequestContext = getActiveBackendRequestContext();
    const files = Array.isArray(options.datasetNames)
      ? options.datasetNames
      : activeRequestContext?.dataset_names?.length
        ? activeRequestContext.dataset_names
        : getSelectedRemoteDatasets();
    const nodesFile = options.nodesFile ?? activeRequestContext?.nodes_file ?? (els.nodesFile.files[0] ? els.nodesFile.files[0].name : null);
    const namesFile = options.namesFile ?? activeRequestContext?.names_file ?? (els.namesFile.files[0] ? els.namesFile.files[0].name : null);
    const minReads = options.minReads != null
      ? Number(options.minReads)
      : activeRequestContext && Number.isFinite(activeRequestContext.min_reads)
        ? Number(activeRequestContext.min_reads)
        : getMinReadsValueSafe();
    return {
      files,
      nodes_file: nodesFile || null,
      names_file: namesFile || null,
      min_reads: minReads,
      expanded_taxids: Array.isArray(options.expandedTaxids)
        ? options.expandedTaxids.map((value) => Number(value))
        : Array.from(state.remote.expandedTaxids),
      taxonomy_only: options.taxonomyOnly != null
        ? Boolean(options.taxonomyOnly)
        : Boolean(state.remote.taxonomyOnly),
    };
  }

  async function postRemoteUncollapseToTips(taxids, options = {}) {
    const body = {
      ...buildRemoteContextPayload(options),
      taxids: Array.isArray(taxids) ? taxids.map((value) => Number(value)) : [],
    };
    addClientLog(
      "info",
      "tree",
      "Running Uncollapse Tips.",
      `selected_taxids=${body.taxids.length.toLocaleString()}`,
    );
    let response;
    try {
      response = await fetch("http://localhost:8000/uncollapse-to-tips", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
        },
        body: JSON.stringify(body),
      });
    } catch (error) {
      addClientLog("error", "tree", "Remote uncollapse-to-tips fetch failed.", core.errorToDetail(error));
      throw error;
    }
    const payload = await response.json().catch(() => ({}));
    if (!response.ok) {
      addClientLog(
        "error",
        "tree",
        "Remote uncollapse-to-tips request failed.",
        payload?.detail?.message || `HTTP ${response.status}`,
      );
      throw new Error(payload?.detail?.message || `Remote uncollapse-to-tips request failed with HTTP ${response.status}`);
    }
    addClientLog(
      "success",
      "tree",
      "Uncollapse Tips completed.",
      `selected_taxids=${body.taxids.length.toLocaleString()} active_expanded_taxids=${Number(Array.isArray(payload?.expanded_taxids) ? payload.expanded_taxids.length : 0).toLocaleString()}`,
    );
    return payload;
  }

  async function fetchRemoteNodeTooltip(taxid) {
    const response = await fetch(buildRemoteContextUrl("node-tooltip", {
      query: { taxid },
    }).toString(), { method: "GET" });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok) {
      throw new Error(payload?.detail?.message || `Remote node-tooltip request failed with HTTP ${response.status}`);
    }
    return payload;
  }

  async function fetchRemoteTableView(options = {}) {
    const response = await fetch(buildRemoteContextUrl("table-view", {
      query: {
        scope: options.scope || "root",
        taxid: options.taxid,
        sort: options.sort || "direct",
        limit: options.limit || 40,
      },
    }).toString(), { method: "GET" });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok) {
      throw new Error(payload?.detail?.message || `Remote table-view request failed with HTTP ${response.status}`);
    }
    return payload;
  }

  async function fetchRemoteSubtreeReport(taxid, options = {}) {
    const body = {
      ...buildRemoteContextPayload(options),
      taxid: taxid != null ? Number(taxid) : null,
      taxids: Array.isArray(options.taxids) ? options.taxids.map((value) => Number(value)) : [],
    };
    const response = await fetch("http://localhost:8000/subtree-report", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
      },
      body: JSON.stringify(body),
    });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok) {
      throw new Error(payload?.detail?.message || `Remote subtree-report request failed with HTTP ${response.status}`);
    }
    return payload;
  }

  async function fetchRemoteRankReport(taxids) {
    const body = {
      ...buildRemoteContextPayload({}),
      taxids: Array.isArray(taxids) ? taxids.map((value) => Number(value)) : [],
    };
    const response = await fetch("http://localhost:8000/rank-report", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
      },
      body: JSON.stringify(body),
    });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok) {
      throw new Error(payload?.detail?.message || `Remote rank-report request failed with HTTP ${response.status}`);
    }
    return payload;
  }

  function applyRemoteVisiblePayload(payload) {
    // Invalidates an in-flight threshold refresh before any newer tree state is shown.
    state.remote.treeRefreshGeneration += 1;
    const datasets = Array.isArray(payload.datasets) ? payload.datasets : [];
    state.series = buildSeriesFromRemoteDatasets(datasets);
    state.tree = buildRemoteTree(payload.tree);
    state.missingTaxids = new Set(Array.isArray(payload.missing_taxids) ? payload.missing_taxids : []);
    updateActiveBackendRequestContext(payload?.request_context, { preserveExpandedTaxids: false });
    const activeRequestContext = getActiveBackendRequestContext();
    state.remote.activeExpandedTaxids = new Set(
      Array.isArray(payload.expanded_taxids)
        ? payload.expanded_taxids.map((value) => Number(value))
        : Array.isArray(activeRequestContext?.expanded_taxids)
          ? activeRequestContext.expanded_taxids.map((value) => Number(value))
          : [],
    );
    if (Array.isArray(payload.requested_expanded_taxids)) {
      state.remote.expandedTaxids = new Set(
        payload.requested_expanded_taxids.map((value) => Number(value)),
      );
    }
    state.remote.serverTreeActive = true;
    state.remote.taxonomyOnly = Boolean(payload.taxonomy_only);
    state.remote.totalReads = Number(payload.total_reads || 0);
    state.remote.directTaxa = Number(payload.direct_taxa || 0);
    syncMinReadsControlSafe();
    renderSourceLegendSafe();
  }

  async function refreshRemoteDatasets(options = {}) {
    if (!state.remote.connected) {
      renderRemoteDatasets();
      return;
    }

    const { selectAll = false } = options;
    addClientLog("info", "datasets", "Refreshing backend dataset list.", "GET http://localhost:8000/datasets");
    let response;
    try {
      response = await fetch("http://localhost:8000/datasets", {
        method: "GET",
      });
    } catch (error) {
      addClientLog("error", "datasets", "Backend datasets fetch failed.", core.errorToDetail(error));
      throw error;
    }
    if (!response.ok) {
      const payload = await response.json().catch(() => ({}));
      const detail = payload?.detail;
      const message = typeof detail === "string"
        ? detail
        : typeof detail?.message === "string"
          ? detail.message
          : `Backend datasets request failed with HTTP ${response.status}`;
      const errorCode = typeof detail?.code === "string"
        ? detail.code
        : null;
      const filename = typeof detail?.filename === "string"
        ? detail.filename
        : null;
      const diagnostic = [
        errorCode,
        filename ? `file ${filename}` : null,
        message,
      ].filter(Boolean).join(" | ");
      addClientLog(
        "error",
        "datasets",
        "Backend datasets request failed.",
        diagnostic,
      );
      throw new Error(message);
    }

    const payload = await response.json();
    state.remote.metadata = normalizeRemoteMetadata(payload?.metadata);
    const datasets = Array.isArray(payload.datasets) ? payload.datasets : [];
    const previous = new Set(state.remote.selectedDatasets);
    const previouslyAllSelected = state.remote.datasets.length > 0
      && previous.size === state.remote.datasets.length;

    state.remote.datasets = datasets.map((dataset) => ({
      id: dataset.id || dataset.filename || "",
      filename: dataset.filename || dataset.id || "remote-dataset",
      bytes: Number(dataset.bytes || 0),
      modified_at: dataset.modified_at || "",
      metadata: dataset.metadata && typeof dataset.metadata === "object" ? dataset.metadata : null,
    }));

    if (selectAll || !previous.size || previouslyAllSelected) {
      state.remote.selectedDatasets = new Set(state.remote.datasets.map((dataset) => dataset.filename));
    } else {
      state.remote.selectedDatasets = new Set(
        state.remote.datasets
          .map((dataset) => dataset.filename)
          .filter((filename) => previous.has(filename)),
      );
    }

    renderRemoteDatasets();
    renderMetadataSummary();
    addClientLog("success", "datasets", `Loaded ${state.remote.datasets.length.toLocaleString()} backend dataset${state.remote.datasets.length === 1 ? "" : "s"}.`);
  }

  function renderRemoteDatasets() {
    els.remoteDatasetsList.innerHTML = "";

    if (!state.remote.connected) {
      els.remoteDatasetsMeta.textContent = "Connect to browse backend files";
      renderMetadataSummary();
      updateRenderAvailability();
      return;
    }

    const datasets = state.remote.datasets;
    const selected = state.remote.selectedDatasets;

    if (!datasets.length) {
      els.remoteDatasetsMeta.textContent = "0 files available";
      const empty = document.createElement("div");
      empty.className = "remote-datasets-empty";
      empty.textContent = "No .bdamage datasets found in uploads on the backend.";
      els.remoteDatasetsList.appendChild(empty);
      renderMetadataSummary();
      updateRenderAvailability();
      return;
    }

    els.remoteDatasetsMeta.textContent =
      `${selected.size.toLocaleString()} of ${datasets.length.toLocaleString()} file${datasets.length === 1 ? "" : "s"} selected${state.remote.metadata ? ` • metadata: ${state.remote.metadata.matched_rows.toLocaleString()} matched` : ""}`;

    for (const dataset of datasets) {
      const item = document.createElement("div");
      item.className = "remote-dataset-item";

      const label = document.createElement("label");
      const checkbox = document.createElement("input");
      checkbox.type = "checkbox";
      checkbox.checked = selected.has(dataset.filename);
      checkbox.addEventListener("change", async () => {
        if (checkbox.checked) state.remote.selectedDatasets.add(dataset.filename);
        else state.remote.selectedDatasets.delete(dataset.filename);
        renderRemoteDatasets();
        await refreshTreeForDatasetSelectionChange();
      });

      const copy = document.createElement("div");
      copy.className = "remote-dataset-copy";

      const name = document.createElement("div");
      name.className = "remote-dataset-name";
      name.textContent = dataset.filename;

      const detail = document.createElement("div");
      detail.className = "remote-dataset-detail";
      detail.textContent = formatRemoteDatasetDetail(dataset);

      copy.appendChild(name);
      copy.appendChild(detail);
      label.appendChild(checkbox);
      label.appendChild(copy);
      item.appendChild(label);
      els.remoteDatasetsList.appendChild(item);
    }

    renderMetadataSummary();
    updateRenderAvailability();
  }

  function selectAllRemoteDatasets() {
    state.remote.selectedDatasets = new Set(
      state.remote.datasets.map((dataset) => dataset.filename),
    );
    renderRemoteDatasets();
    refreshTreeForDatasetSelectionChange();
  }

  function clearRemoteDatasets() {
    state.remote.selectedDatasets.clear();
    renderRemoteDatasets();
    refreshTreeForDatasetSelectionChange();
  }

  function resetBackendTreeState() {
    state.tree = null;
    state.flat = [];
    state.series = [];
    state.remote.serverTreeActive = false;
    state.remote.totalReads = 0;
    state.remote.directTaxa = 0;
    state.remote.currentReport = null;
    state.remote.requestContext = null;
    state.remote.expandedTaxids.clear();
    state.remote.activeExpandedTaxids.clear();
    state.remote.taxonomyOnly = false;
    state.selected.clear();
    state.focusTaxid = null;
    state.missingTaxids.clear();
    if (els.svg) {
      els.svg.innerHTML = "";
    }
    globalObject.clearSubtreeReportView();
    renderSourceLegendSafe();
    globalObject.renderSummary([]);
    syncBackendRuntimeUiState();
  }

  function reconcileTreeStateAfterDatasetChange() {
    if (!state.tree) return;
    state.selected = new Set(
      Array.from(state.selected).filter((taxid) => Boolean(globalObject.findNodeByTaxid(state.tree, taxid))),
    );
    if (state.focusTaxid != null && !globalObject.findNodeByTaxid(state.tree, state.focusTaxid)) {
      state.focusTaxid = null;
    }
    if (state.remote.currentReport) {
      globalObject.clearSubtreeReportView();
    }
  }

  async function refreshTreeForDatasetSelectionChange() {
    if (!state.remote.connected || !state.remote.serverTreeActive) {
      return;
    }
    const datasetNames = getSelectedRemoteDatasets();
    if (!datasetNames.length) {
      if (!state.remote.taxonomyOnly) {
        resetBackendTreeState();
        setStatus("No backend datasets are selected. Render the taxonomy-only tree or select datasets for count-backed rendering.");
      }
      return;
    }
    try {
      addClientLog(
        "info",
        "tree",
        `Refreshing backend tree after dataset selection changed to ${datasetNames.length.toLocaleString()} dataset${datasetNames.length === 1 ? "" : "s"}.`,
        datasetNames.join("\n"),
      );
      const payload = await fetchRemoteVisibleTree({
        datasetNames,
        expandedTaxids: Array.from(state.remote.expandedTaxids),
        taxonomyOnly: false,
      });
      applyRemoteVisiblePayload(payload);
      reconcileTreeStateAfterDatasetChange();
      globalObject.redraw();
    } catch (error) {
      addClientLog("error", "tree", "Could not refresh backend tree after dataset selection change.", core.errorToDetail(error));
      setStatus(`Could not refresh the backend tree after changing datasets: ${error.message || error}`);
    }
  }

  function hasLocalRemoteTaxonomyOverride() {
    return Boolean(els.nodesFile.files[0]);
  }

  function hasRemoteBackendTaxonomy() {
    return Boolean(state.remote.backendNodesFile);
  }

  function canRenderRemoteTree() {
    return state.remote.connected
      && (hasLocalRemoteTaxonomyOverride() || hasRemoteBackendTaxonomy());
  }

  function remoteRenderUnavailableMessage() {
    if (!state.remote.connected) {
      return "Connect to the backend before rendering.";
    }
    return "Backend taxonomy is not ready yet. Upload nodes.dmp from this UI, or place nodes.dmp in the backend uploads directory.";
  }

  function backendConnectionReadyMessage(user, host) {
    const selected = getSelectedRemoteDatasets().length;
    const pendingUploads = getLocalRemoteDatasetUploadFiles().length;
    const metadataReady = state.remote.metadata
      ? `metadata (${state.remote.metadata.filename || "metadata.txt"}, ${state.remote.metadata.matched_rows.toLocaleString()} matched row${state.remote.metadata.matched_rows === 1 ? "" : "s"})`
      : "no backend metadata loaded yet";
    const datasetLabel = selected === 1 ? "dataset" : "datasets";
    if (canRenderRemoteTree()) {
      const taxonomySource = hasLocalRemoteTaxonomyOverride()
        ? `uploaded taxonomy staged from this UI (${els.nodesFile.files[0].name}${els.namesFile.files[0] ? `, ${els.namesFile.files[0].name}` : ""})`
        : `backend taxonomy (${state.remote.backendNodesFile}${state.remote.backendNamesFile ? `, ${state.remote.backendNamesFile}` : ""})`;
      if (selected > 0) {
        return `Tunnel check succeeded for ${user}@${host}. Backend HTTP endpoint is reachable, ${selected.toLocaleString()} ${datasetLabel} ${selected === 1 ? "is" : "are"} selected, ${taxonomySource} is ready for rendering, and ${metadataReady}.`;
      }
      if (pendingUploads > 0) {
        return `Tunnel check succeeded for ${user}@${host}. Backend HTTP endpoint is reachable, ${pendingUploads.toLocaleString()} local dataset${pendingUploads === 1 ? "" : "s"} ${pendingUploads === 1 ? "is" : "are"} queued for upload, ${taxonomySource} is ready for rendering, and ${metadataReady}.`;
      }
      return `Tunnel check succeeded for ${user}@${host}. Backend HTTP endpoint is reachable, ${taxonomySource} is ready for an empty taxonomy render, and ${metadataReady}.`;
    }
    if (pendingUploads > 0) {
      return `Tunnel check succeeded for ${user}@${host}. Local datasets are queued for upload, but backend rendering is waiting for taxonomy. Upload nodes.dmp to enable Render Tree. Backend metadata status: ${metadataReady}.`;
    }
    if (selected > 0) {
      return `Tunnel check succeeded for ${user}@${host}. Backend datasets are visible, but backend rendering is waiting for taxonomy. Upload nodes.dmp to enable Render Tree. Backend metadata status: ${metadataReady}.`;
    }
    return `Tunnel check succeeded for ${user}@${host}. Backend HTTP endpoint is reachable; taxonomy is ready for an empty taxonomy render, and ${metadataReady}.`;
  }

  function updateRemoteServerStatus(ping) {
    state.remote.backendNodesFile = String(ping?.taxonomy?.nodes_file || "");
    state.remote.backendNamesFile = String(ping?.taxonomy?.names_file || "");
    state.remote.metadata = normalizeRemoteMetadata(ping?.metadata);
    ensureMetadataFieldPreferences(state.remote.metadata);
    metadataResolver.ensureMetadataVisualizationState(state.remote.metadata);
    renderMetadataSummary();
    syncBackendRuntimeUiState();
  }

  async function refreshRemoteServerStatus() {
    if (!state.remote.connected) {
      updateRemoteServerStatus(null);
      return null;
    }
    const response = await fetch("http://localhost:8000/ping", { method: "GET" });
    if (!response.ok) {
      throw new Error(`Remote ping request failed with HTTP ${response.status}`);
    }
    const payload = await response.json();
    updateRemoteServerStatus(payload);
    return payload;
  }

  function syncBackendRuntimeUiState() {
    const backendTreeReady = hasBackendTree();
    const countDataAvailable = backendTreeReady && !state.remote.taxonomyOnly;
    const hasSelection = state.selected.size > 0;
    const hasTargetRank = Boolean(String(els.selectToRankValue?.value || "").trim());
    els.renderBtn.disabled = !canRenderRemoteTree();
    els.centerBtn.disabled = !backendTreeReady;
    els.toggleTableBtn.disabled = !countDataAvailable;
    els.subtreeReportBtn.disabled = !countDataAvailable;
    els.rankReportBtn.disabled = !countDataAvailable;
    if (els.damageBtn) {
      els.damageBtn.disabled = !countDataAvailable || !hasSelection;
    }
    els.selectDescendantsBtn.disabled = !backendTreeReady || !hasSelection;
    if (els.selectToRankBtn) {
      els.selectToRankBtn.disabled = !backendTreeReady || !hasSelection || !hasTargetRank;
    }
    if (els.selectToRankValue) {
      els.selectToRankValue.disabled = !backendTreeReady;
    }
    els.clearSelectionBtn.disabled = !backendTreeReady || !hasSelection;
    els.uncollapseBtn.disabled = !backendTreeReady || !hasSelection;
    els.uncollapseTipsBtn.disabled = !backendTreeReady || !hasSelection || state.remote.taxonomyOnly;
    namespace.agentUi?.syncAvailability?.();
  }

  function updateRenderAvailability() {
    syncBackendRuntimeUiState();
  }

  function getSelectedRemoteDatasets() {
    return state.remote.datasets
      .map((dataset) => dataset.filename)
      .filter((filename) => state.remote.selectedDatasets.has(filename));
  }

  function getActiveSeriesRemoteDatasets() {
    if (!Array.isArray(state.series) || !state.series.length) {
      return state.remote.datasets.filter((dataset) => state.remote.selectedDatasets.has(dataset.filename));
    }
    const datasetByFilename = new Map(
      state.remote.datasets.map((dataset) => [dataset.filename, dataset]),
    );
    return state.series.map((source) => {
      const filename = String(source?.label || "");
      return datasetByFilename.get(filename) || {
        filename,
        metadata: null,
      };
    }).filter((dataset) => dataset && dataset.filename);
  }

  function getColorForDatasetFilename(filename) {
    const matchingSeries = state.series.find((source) => source.label === filename);
    if (matchingSeries?.color) {
      return matchingSeries.color;
    }
    const datasetIndex = state.remote.datasets.findIndex((dataset) => dataset.filename === filename);
    return colorForSource(datasetIndex >= 0 ? datasetIndex : 0);
  }

  function getSelectedRemoteDatasetSummaries() {
    return state.remote.datasets
      .filter((dataset) => state.remote.selectedDatasets.has(dataset.filename))
      .map((dataset) => ({
        filename: dataset.filename,
        color: getColorForDatasetFilename(dataset.filename),
        metadata: dataset.metadata && typeof dataset.metadata === "object" ? { ...dataset.metadata } : null,
      }));
  }

  function getActiveSeriesRemoteDatasetSummaries() {
    const activeDatasets = getActiveSeriesRemoteDatasets();
    const colorByFilename = new Map(
      state.series.map((source, index) => [source.label, source.color || colorForSource(index)]),
    );
    return activeDatasets.map((dataset, index) => ({
      filename: dataset.filename,
      color: colorByFilename.get(dataset.filename) || colorForSource(index),
      metadata: dataset.metadata && typeof dataset.metadata === "object" ? { ...dataset.metadata } : null,
    }));
  }

  function getSelectedResolvedMetadataDisplays(options = {}) {
    return metadataResolver.resolveDatasetMetadataDisplays(
      getActiveSeriesRemoteDatasetSummaries(),
      options,
    );
  }

  function getSelectedMetadataLegendPayload(options = {}) {
    return metadataResolver.buildMetadataLegendPayload(
      getActiveSeriesRemoteDatasetSummaries(),
      options,
    );
  }

  function formatRemoteDatasetDetail(dataset) {
    const parts = [];
    if (dataset.bytes > 0) parts.push(core.formatBytes(dataset.bytes));
    if (dataset.modified_at) {
      const parsed = new Date(dataset.modified_at);
      parts.push(Number.isNaN(parsed.getTime()) ? dataset.modified_at : parsed.toLocaleString());
    }
    const metadata = dataset.metadata && typeof dataset.metadata === "object" ? dataset.metadata : null;
    if (metadata) {
      const metadataPreview = Object.entries(metadata)
        .slice(0, 2)
        .map(([key, value]) => `${key}: ${value}`)
        .join(" | ");
      if (metadataPreview) {
        parts.push(metadataPreview);
      }
    }
    return parts.join(" \u2022 ") || "ready";
  }

  function renderMetadataSummary() {
    if (!els.metadataSummaryMeta || !els.metadataSummaryFields || !els.metadataFieldSelect || !els.metadataColorFieldSelect || !els.metadataFieldColorMeta || !els.metadataFieldColorList || !els.metadataSelectedMeta || !els.metadataSelectedList) {
      return;
    }
    if (!state.remote.connected) {
      els.metadataSummaryMeta.textContent = "Connect to inspect backend metadata";
      els.metadataSummaryFields.innerHTML = "The backend will report whether <strong>metadata.txt</strong> is already present.";
      els.metadataFieldSelect.innerHTML = "";
      els.metadataFieldSelect.disabled = true;
      els.metadataColorFieldSelect.innerHTML = "";
      els.metadataColorFieldSelect.disabled = true;
      els.metadataFieldColorMeta.textContent = "No active metadata color field";
      els.metadataFieldColorList.textContent = "Connect to the backend to configure metadata value colors.";
      els.metadataSelectedMeta.textContent = "No backend dataset context yet";
      els.metadataSelectedList.textContent = "Connect to the backend to inspect dataset metadata bindings.";
      return;
    }
    const metadata = state.remote.metadata;
    if (!metadata) {
      els.metadataSummaryMeta.textContent = "No backend metadata loaded";
      els.metadataSummaryFields.innerHTML = "Upload one metadata table to store it on the backend as <strong>metadata.txt</strong>.";
      els.metadataFieldSelect.innerHTML = "";
      els.metadataFieldSelect.disabled = true;
      els.metadataColorFieldSelect.innerHTML = "";
      els.metadataColorFieldSelect.disabled = true;
      els.metadataFieldColorMeta.textContent = "No active metadata color field";
      els.metadataFieldColorList.textContent = "Choose a metadata field to edit value colors.";
      els.metadataSelectedMeta.textContent = "No metadata attached to selected datasets";
      els.metadataSelectedList.textContent = "Once metadata is loaded, selected datasets with matching filenames will appear here.";
      return;
    }

    ensureMetadataFieldPreferences(metadata);
    const fieldCount = metadata.fields.length;
    const selectedFields = getSelectedMetadataFields();
    const activeField = metadataResolver.getActiveMetadataVisualizationField();
    const selectedSummaries = getSelectedRemoteDatasetSummaries();
    const resolvedDisplays = getSelectedResolvedMetadataDisplays();
    const legendPayload = getSelectedMetadataLegendPayload();

    els.metadataSummaryMeta.textContent =
      `${metadata.matched_rows.toLocaleString()} matched sample${metadata.matched_rows === 1 ? "" : "s"} • ${fieldCount.toLocaleString()} field${fieldCount === 1 ? "" : "s"} • ${metadata.unmatched_rows.toLocaleString()} unmatched row${metadata.unmatched_rows === 1 ? "" : "s"}`;
    els.metadataSummaryFields.textContent = fieldCount
      ? `${metadata.filename || "metadata.txt"} • available fields: ${metadata.fields.join(", ")}`
      : "No metadata fields were detected beyond the required dataset column.";

    els.metadataFieldSelect.disabled = !fieldCount;
    els.metadataFieldSelect.innerHTML = metadata.fields.map((field) => {
      const selected = selectedFields.includes(field) ? " selected" : "";
      return `<option value="${core.escapeHtml(field)}"${selected}>${core.escapeHtml(field)}</option>`;
    }).join("");

    els.metadataColorFieldSelect.disabled = !fieldCount;
    els.metadataColorFieldSelect.innerHTML = [
      `<option value="">No metadata coloring</option>`,
      ...metadata.fields.map((field) => {
        const selected = activeField === field ? " selected" : "";
        return `<option value="${core.escapeHtml(field)}"${selected}>${core.escapeHtml(field)}</option>`;
      }),
    ].join("");

    if (!activeField) {
      els.metadataFieldColorMeta.textContent = "No active metadata color field";
      els.metadataFieldColorList.textContent = "Choose a metadata field to edit value colors.";
    } else {
      const values = Array.isArray(legendPayload?.values) ? legendPayload.values : [];
      els.metadataFieldColorMeta.textContent =
        `${activeField} • ${values.length.toLocaleString()} value${values.length === 1 ? "" : "s"} with editable display colors`;
      els.metadataFieldColorList.innerHTML = values.map((entry) => {
        const valueLabel = entry.value === metadataResolver.MISSING_METADATA_VALUE ? "(missing)" : String(entry.value);
        return `
          <label class="metadata-field-color-item">
            <span class="metadata-field-color-name">
              <span class="metadata-selected-swatch" style="background:${core.escapeHtml(entry.color)}"></span>
              <span>${core.escapeHtml(valueLabel)}</span>
            </span>
            <input
              class="metadata-field-color-input"
              type="color"
              value="${core.escapeHtml(entry.color)}"
              data-field-name="${core.escapeHtml(activeField)}"
              data-field-value="${core.escapeHtml(String(entry.value))}"
              aria-label="Choose display color for ${core.escapeHtml(valueLabel)} in ${core.escapeHtml(activeField)}"
            >
          </label>
        `;
      }).join("");
      els.metadataFieldColorList.querySelectorAll(".metadata-field-color-input").forEach((input) => {
        input.addEventListener("input", (event) => {
          const target = event.target;
          if (!(target instanceof HTMLInputElement)) return;
          handleMetadataValueColorInput(target.dataset.fieldName || "", target.dataset.fieldValue || "", target.value);
        });
      });
    }

    if (!selectedSummaries.length) {
      els.metadataSelectedMeta.textContent = "No selected datasets yet";
      els.metadataSelectedList.textContent = "Select one or more backend datasets to inspect their metadata bindings.";
      return;
    }
    const withMetadata = selectedSummaries.filter((dataset) => dataset.metadata && Object.keys(dataset.metadata).length > 0);
    els.metadataSelectedMeta.textContent =
      `${withMetadata.length.toLocaleString()} of ${selectedSummaries.length.toLocaleString()} selected dataset${selectedSummaries.length === 1 ? "" : "s"} matched metadata`;
    if (!withMetadata.length) {
      els.metadataSelectedList.textContent = "The current dataset selection has no matched metadata rows.";
      return;
    }
    const resolvedByFilename = new Map(resolvedDisplays.map((entry) => [entry.filename, entry]));
    els.metadataSelectedList.innerHTML = withMetadata.map((dataset) => {
      const resolvedDataset = resolvedByFilename.get(dataset.filename) || null;
      const metadataEntries = Object.entries(dataset.metadata || {})
        .filter(([key]) => selectedFields.includes(key));
      const metadataRows = metadataEntries
        .map(([key, value], index) => buildMetadataChipHtml(
          key,
          value,
          metadataResolver.getMetadataValueColor(
            key,
            metadataResolver.getDatasetMetadataValue(dataset, key),
          ),
          index,
        ))
        .join("");
      return `
        <div class="metadata-selected-item">
          <div class="metadata-selected-name">
            <span class="metadata-selected-swatch" style="background:${core.escapeHtml(resolvedDataset?.color || dataset.color)}"></span>
            <span title="${core.escapeHtml(dataset.filename)}">${core.escapeHtml(dataset.filename)}</span>
          </div>
          <div class="metadata-selected-values">${metadataRows || '<span class="metadata-selected-empty">No values for the currently selected metadata fields.</span>'}</div>
        </div>
      `;
    }).join("");
  }

  function addClientLog(level, stage, message, detail = "") {
    if (typeof globalObject.addClientLog === "function") {
      globalObject.addClientLog(level, stage, message, detail);
    }
  }

  function setStatus(message) {
    if (typeof globalObject.setStatus === "function") {
      globalObject.setStatus(message);
    }
  }

  namespace.backend = {
    getActiveBackendRequestContext,
    updateActiveBackendRequestContext,
    normalizeProviderRequestContext,
    connectRemote,
    updateTunnelHint,
    updateConnectionState,
    uploadLoadedFiles,
    copyTunnelCommand,
    loadAndRender,
    getLocalRemoteUploadFiles,
    getLocalRemoteDatasetUploadFiles,
    uploadFilesToRemote,
    loadAndRenderBackend,
    clearRemoteDatasetInputs,
    buildSeriesFromRemoteDatasets,
    buildRemoteTree,
    handleMinReadsChange,
    hasBackendTree,
    buildRemoteContextUrl,
    buildRemoteContextPayload,
    fetchRemoteVisibleTree,
    postRemoteVisibleTree,
    refreshRemoteTreeForMinReads,
    postRemoteUncollapseToTips,
    fetchRemoteNodeTooltip,
    fetchRemoteTableView,
    fetchRemoteSubtreeReport,
    fetchRemoteRankReport,
    applyRemoteVisiblePayload,
    refreshRemoteDatasets,
    renderRemoteDatasets,
    selectAllRemoteDatasets,
    clearRemoteDatasets,
    resetBackendTreeState,
    reconcileTreeStateAfterDatasetChange,
    refreshTreeForDatasetSelectionChange,
    hasLocalRemoteTaxonomyOverride,
    hasRemoteBackendTaxonomy,
    canRenderRemoteTree,
    remoteRenderUnavailableMessage,
    backendConnectionReadyMessage,
    updateRemoteServerStatus,
    refreshRemoteServerStatus,
    syncBackendRuntimeUiState,
    updateRenderAvailability,
    initMetadataControls,
    getSelectedRemoteDatasets,
    getSelectedRemoteDatasetSummaries,
    getSelectedResolvedMetadataDisplays,
    getSelectedMetadataLegendPayload,
    formatRemoteDatasetDetail,
    getVisibleSeriesSafe,
    renderMetadataSummary,
  };

  Object.assign(globalObject, namespace.backend);
})(window);
