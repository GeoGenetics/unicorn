"use strict";

// Modularization transition note:
// `graphengine.js` is currently being split into smaller browser-side modules.
// During Pass 1 it still owns most runtime logic, but `state`, `els`, and boot
// wiring now live in dedicated files.
const unicornGraphEngine = window.UnicornGraphEngine = window.UnicornGraphEngine || {};
const state = unicornGraphEngine.state;
const els = unicornGraphEngine.els;
const core = unicornGraphEngine.core;
const backend = unicornGraphEngine.backend;
const ui = unicornGraphEngine.ui;

if (!state || !els || !core || !backend || !ui) {
  throw new Error("Unicorn graphengine expected state, DOM, core, backend, and UI modules to load before graphengine.js.");
}

const {
  SOURCE_COLORS,
  RANK_DISPLAY_ORDER,
  AGENT_PROVIDER_MODELS,
  AGENT_SUPPORTED_PROVIDER_TARGETS,
  AGENT_V1_READ_ONLY_TOOL_NAMES,
  AGENT_MAX_TOOL_ITERATIONS,
  formatBytes,
  svgEl,
  formatLogTime,
  errorToDetail,
  escapeHtml,
} = core;

const {
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
  fetchRemoteVisibleTree,
  fetchRemoteNodeTooltip,
  fetchRemoteTableView,
  fetchRemoteSubtreeReport,
  fetchRemoteRankReport,
  fetchRemoteFullTreeModel,
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
  getSelectedRemoteDatasets,
  formatRemoteDatasetDetail,
} = backend;

const {
  initRemotePanel,
  initFileInputs,
  initMinReadsControls,
  getMinReadsMax,
  getMinReadsScale,
  getMinReadsValue,
  setMinReadsValue,
  valueFromSliderPosition,
  sliderPositionFromValue,
  syncMinReadsControl,
  handleMinReadsScaleChange,
  handleMinReadsMaxChange,
  ensureFileRowControls,
  initSidebarPanel,
  initSectionToggle,
  setSectionCollapsed,
  toggleSidebar,
  setSidebarCollapsed,
  getCountViewMode,
  getCountViewLabel,
  initControlsResize,
  setControlsWidth,
  initSummaryResize,
  setSummaryHeight,
  initTablePanel,
  initTableResize,
  toggleTablePanel,
  setTablePanelVisible,
  setTableHeight,
  addLcaInput,
  clearLcaListFile,
  colorForSource,
  renderSourceLegend,
  getVisibleSeries,
} = ui;

const unicornAgentRegistry = createUnicornAgentRegistry();
window.unicornAgentRegistry = unicornAgentRegistry;
if (!window.UnicornAgentProviderModule || typeof window.UnicornAgentProviderModule.createProviderAdapter !== "function") {
  throw new Error("Unicorn agent provider module failed to load before graphengine.js.");
}
const unicornAgentProviderAdapter = window.UnicornAgentProviderModule.createProviderAdapter({
  runtimeProviderName: state.agent.runtimeProvider,
  getRuntimeConfig: getAgentRuntimeConfig,
  extractTaxidFromPrompt,
  executeTurn: executeAgentProviderTurn,
});
window.unicornAgentProviderAdapter = unicornAgentProviderAdapter;

function initializeGraphengine() {
  els.renderBtn.addEventListener("click", loadAndRender);
  els.addLcaBtn.addEventListener("click", addLcaInput);
  els.clearLcaListBtn.addEventListener("click", clearLcaListFile);
  els.connectBtn.addEventListener("click", connectRemote);
  els.uploadBtn.addEventListener("click", uploadLoadedFiles);
  els.copyTunnelBtn.addEventListener("click", copyTunnelCommand);
  els.remoteRefreshBtn.addEventListener("click", async () => {
    try {
      await refreshRemoteDatasets();
      const count = state.remote.datasets.length;
      setStatus(`Backend dataset list refreshed. ${count.toLocaleString()} file${count === 1 ? "" : "s"} available.`);
    } catch (error) {
      setStatus(`Could not refresh backend datasets: ${error.message || error}`);
    }
  });
  els.remoteSelectAllBtn.addEventListener("click", selectAllRemoteDatasets);
  els.remoteClearAllBtn.addEventListener("click", clearRemoteDatasets);
  els.remoteUser.addEventListener("input", updateTunnelHint);
  els.remoteHost.addEventListener("input", updateTunnelHint);
  els.centerBtn.addEventListener("click", () => {
    state.focusTaxid = null;
    centerRoot();
  });
  if (els.countViewMode) {
    els.countViewMode.addEventListener("change", () => {
      redraw();
    });
  }
  els.toggleTableBtn.addEventListener("click", toggleTablePanel);
  els.countMode.addEventListener("change", redraw);
  els.scaleMode.addEventListener("change", redraw);
  els.minReads.addEventListener("input", handleMinReadsChange);
  if (els.minReadsScale) {
    els.minReadsScale.addEventListener("change", handleMinReadsScaleChange);
  }
  if (els.minReadsMax) {
    els.minReadsMax.addEventListener("input", handleMinReadsMaxChange);
    els.minReadsMax.addEventListener("change", handleMinReadsMaxChange);
  }
  els.searchBox.addEventListener("input", redraw);
  els.sidebarToggle.addEventListener("click", toggleSidebar);
  els.uncollapseBtn.addEventListener("click", uncollapseSelected);
  els.uncollapseTipsBtn.addEventListener("click", uncollapseSelectedToTips);
  if (els.subtreeReportBtn) {
    els.subtreeReportBtn.addEventListener("click", openSelectedSubtreeReport);
  }
  if (els.rankReportBtn) {
    els.rankReportBtn.addEventListener("click", openSelectedRankReport);
  }
  if (els.agentSendBtn) {
    els.agentSendBtn.addEventListener("click", handleAgentSend);
  }
  if (els.agentClearBtn) {
    els.agentClearBtn.addEventListener("click", clearAgentTranscript);
  }
  if (els.agentProviderSelect) {
    els.agentProviderSelect.addEventListener("change", handleAgentProviderChange);
  }
  if (els.agentModelSelect) {
    els.agentModelSelect.addEventListener("change", handleAgentModelChange);
  }
  if (els.agentApiKey) {
    els.agentApiKey.addEventListener("input", handleAgentApiKeyInput);
  }
  if (els.agentBaseUrl) {
    els.agentBaseUrl.addEventListener("input", handleAgentBaseUrlInput);
  }
  if (els.agentPrompt) {
    els.agentPrompt.addEventListener("keydown", (event) => {
      if (event.key === "Enter" && !event.shiftKey) {
        event.preventDefault();
        handleAgentSend();
      }
    });
  }
  els.selectDescendantsBtn.addEventListener("click", selectDescendants);
  if (els.selectToRankBtn) {
    els.selectToRankBtn.addEventListener("click", selectToRank);
  }
  if (els.selectToRankValue) {
    els.selectToRankValue.addEventListener("change", syncBackendRuntimeUiState);
  }
  els.clearSelectionBtn.addEventListener("click", clearSelection);
  if (els.exportMatrixBtn) {
    els.exportMatrixBtn.addEventListener("click", exportCurrentSubtreeMatrix);
  }
  if (els.pcoaBtn) {
    els.pcoaBtn.addEventListener("click", openCurrentCountMatrixPcoa);
  }
  if (els.barplotBtn) {
    els.barplotBtn.addEventListener("click", openCurrentCountMatrixBarplot);
  }
  if (els.clearClientLogBtn) {
    els.clearClientLogBtn.addEventListener("click", clearClientLog);
  }

  initControlsResize();
  initSummaryResize();
  initTablePanel();
  initTableResize();
  initSidebarPanel();
  initChartPan();
  initRemotePanel();
  initFileInputs();
  initMinReadsControls();
  initAgentControls();
  renderAgentTranscript();
}

unicornGraphEngine.initialize = initializeGraphengine;

function createUnicornAgentRegistry() {
  const tools = {
    get_graph_context: async () => buildAgentContext(),
    list_selected_datasets: async () => {
      requireAgentBackendConnection();
      return normalizeSelectedDatasetsForProvider(buildAgentContext());
    },
    get_selected_nodes: async () => {
      requireAgentBackendTree();
      return normalizeSelectedNodesForProvider(
        getSelectedNodes().map((node) => summarizeNodeForAgent(node)),
        buildAgentContext(),
      );
    },
    get_node_details: async (args = {}) => {
      requireAgentBackendTree();
      const taxid = normalizeToolTaxid(args.taxid);
      if (taxid == null) {
        throw new Error("get_node_details requires a numeric taxid.");
      }
      const payload = await fetchRemoteNodeTooltip(taxid);
      return normalizeNodeDetailsForProvider(payload, { taxid });
    },
    get_table_view: async (args = {}) => {
      requireAgentBackendTree();
      const options = {
        scope: args.scope || "root",
        taxid: normalizeToolTaxid(args.taxid),
        sort: args.sort || "direct",
        limit: normalizeToolLimit(args.limit, 40),
      };
      const payload = await fetchRemoteTableView(options);
      return normalizeTableViewForProvider(payload, options);
    },
    select_taxon: async (args = {}) => {
      requireAgentBackendTree();
      const taxid = normalizeToolTaxid(args.taxid);
      if (taxid == null) {
        throw new Error("select_taxon requires a numeric taxid.");
      }
      const additive = Boolean(args.additive);
      const node = findNodeByTaxid(state.tree, taxid);
      if (!node) {
        throw new Error(`Taxid ${taxid} is not present in the active tree.`);
      }
      if (!additive) {
        state.selected.clear();
      }
      state.selected.add(node.taxid);
      redraw();
      return {
        ok: true,
        mode: "backend",
        additive,
        selected: getSelectedNodes().map((selectedNode) => summarizeNodeForAgent(selectedNode)),
      };
    },
    focus_taxon: async (args = {}) => {
      requireAgentBackendTree();
      const taxid = normalizeToolTaxid(args.taxid);
      if (taxid == null) {
        throw new Error("focus_taxon requires a numeric taxid.");
      }
      const node = findNodeByTaxid(state.tree, taxid);
      if (!node) {
        throw new Error(`Taxid ${taxid} is not present in the active tree.`);
      }
      state.focusTaxid = node.taxid;
      centerNode(node);
      redraw();
      return {
        ok: true,
        mode: "backend",
        focused: summarizeNodeForAgent(node),
      };
    },
    center_root: async () => {
      requireAgentBackendTree();
      state.focusTaxid = null;
      centerRoot();
      redraw();
      return {
        ok: true,
        mode: "backend",
        focused: null,
      };
    },
  };

  return {
    listTools() {
      return Object.keys(tools);
    },
    async invokeTool(name, args = {}) {
      const tool = tools[name];
      if (!tool) {
        throw new Error(`Unknown Unicorn agent tool: ${name}`);
      }
      return tool(args);
    },
  };
}

function initAgentControls() {
  if (!els.agentProviderSelect || !els.agentModelSelect || !els.agentApiKey || !els.agentBaseUrl) return;
  els.agentProviderSelect.value = state.agent.configuredProvider;
  populateAgentModelOptions(state.agent.configuredProvider, state.agent.configuredModel);
  els.agentApiKey.value = state.agent.apiKey;
  els.agentBaseUrl.value = state.agent.baseUrl;
  syncAgentRuntimeProvider();
  renderAgentProviderState();
}

function populateAgentModelOptions(provider, selectedModel) {
  if (!els.agentModelSelect) return;
  const models = AGENT_PROVIDER_MODELS[provider] || [];
  if (!models.length) {
    els.agentModelSelect.innerHTML = "";
    state.agent.configuredModel = "";
    return;
  }
  const nextSelected = models.some((model) => model.value === selectedModel)
    ? selectedModel
    : models[0].value;
  els.agentModelSelect.innerHTML = models.map((model) => `
    <option value="${escapeHtml(model.value)}">${escapeHtml(model.label)}</option>
  `).join("");
  els.agentModelSelect.value = nextSelected;
  state.agent.configuredModel = nextSelected;
}

function handleAgentProviderChange() {
  if (!els.agentProviderSelect) return;
  state.agent.configuredProvider = normalizeConfiguredAgentProvider(els.agentProviderSelect.value);
  els.agentProviderSelect.value = state.agent.configuredProvider;
  populateAgentModelOptions(state.agent.configuredProvider, state.agent.configuredModel);
  syncAgentRuntimeProvider();
  renderAgentProviderState();
}

function handleAgentModelChange() {
  if (!els.agentModelSelect) return;
  state.agent.configuredModel = els.agentModelSelect.value || "";
  syncAgentRuntimeProvider();
  renderAgentProviderState();
}

function handleAgentApiKeyInput() {
  if (!els.agentApiKey) return;
  state.agent.apiKey = els.agentApiKey.value || "";
  syncAgentRuntimeProvider();
  renderAgentProviderState();
}

function handleAgentBaseUrlInput() {
  if (!els.agentBaseUrl) return;
  state.agent.baseUrl = normalizeAgentBaseUrl(els.agentBaseUrl.value);
  els.agentBaseUrl.value = state.agent.baseUrl;
  syncAgentRuntimeProvider();
  renderAgentProviderState();
}

function normalizeConfiguredAgentProvider(provider) {
  return AGENT_SUPPORTED_PROVIDER_TARGETS.includes(provider)
    ? provider
    : AGENT_SUPPORTED_PROVIDER_TARGETS[0];
}

function normalizeAgentBaseUrl(baseUrl) {
  return String(baseUrl || "").trim();
}

function getAgentRuntimeConfig() {
  return {
    runtime_provider: state.agent.runtimeProvider,
    transport_mode: state.agent.transportMode,
    configured_provider: normalizeConfiguredAgentProvider(state.agent.configuredProvider),
    configured_model: String(state.agent.configuredModel || ""),
    api_key: String(state.agent.apiKey || ""),
    base_url: normalizeAgentBaseUrl(state.agent.baseUrl),
    backend_base_url: "http://localhost:8000",
  };
}

function getConfiguredAgentProvider() {
  return normalizeConfiguredAgentProvider(state.agent.configuredProvider);
}

function shouldUseOpenAIRuntime() {
  return getConfiguredAgentProvider() === "openai"
    && Boolean(String(state.agent.apiKey || "").trim());
}

function shouldUseGoogleRuntime() {
  return getConfiguredAgentProvider() === "google"
    && Boolean(String(state.agent.apiKey || "").trim());
}

function syncAgentRuntimeProvider() {
  if (shouldUseOpenAIRuntime()) {
    state.agent.runtimeProvider = "openai";
    return;
  }
  if (shouldUseGoogleRuntime()) {
    state.agent.runtimeProvider = "google";
    return;
  }
  state.agent.runtimeProvider = "mock";
}

function buildAgentContext() {
  const backendConnected = Boolean(state.remote.connected);
  const activeRequestContext = getActiveBackendRequestContext();
  const backendTreeReady = Boolean(backendConnected && state.remote.serverTreeActive && state.tree);
  const selectedDatasets = activeRequestContext?.dataset_names?.length
    ? activeRequestContext.dataset_names.slice()
    : backendConnected
      ? getSelectedRemoteDatasets()
      : [];
  const expandedTaxids = activeRequestContext?.expanded_taxids?.length
    ? activeRequestContext.expanded_taxids.slice()
    : [];
  const selectedTaxids = backendTreeReady ? Array.from(state.selected) : [];
  const focusNode = backendTreeReady && state.focusTaxid != null
    ? state.flat.find((node) => node.taxid === state.focusTaxid) || null
    : null;
  const visibleRoot = backendTreeReady
    ? {
        taxid: Number(state.tree.taxid),
        name: String(state.tree.name || ""),
        rank: String(state.tree.rank || ""),
        direct: Number(state.tree.direct || 0),
        total: Number(state.tree.total || 0),
        child_count: Array.isArray(state.tree.children) ? state.tree.children.length : 0,
      }
    : null;
  const currentReport = backendTreeReady && state.remote.currentReport
    ? summarizeCurrentAgentReport(state.remote.currentReport)
    : null;

  return {
    captured_at: new Date().toISOString(),
    mode: "backend",
    datasets: {
      selected: selectedDatasets,
      count: selectedDatasets.length,
      total_reads: Number(backendConnected ? state.remote.totalReads : 0),
      direct_taxa: Number(backendConnected ? state.remote.directTaxa : 0),
    },
    taxonomy: {
      nodes_file: getActiveNodesFilename(),
      names_file: getActiveNamesFilename(),
    },
    filters: {
      min_reads: activeRequestContext && Number.isFinite(activeRequestContext.min_reads)
        ? Number(activeRequestContext.min_reads)
        : getMinReadsValue(),
      count_mode: String(els.countMode?.value || "total"),
      scale_mode: String(els.scaleMode?.value || "sqrt"),
      search: String(els.searchBox?.value || "").trim(),
    },
    tree: {
      loaded: backendTreeReady,
      server_tree_active: backendTreeReady,
      visible_root: visibleRoot,
      visible_node_count: backendTreeReady ? state.flat.length : 0,
      focused_taxid: focusNode ? Number(focusNode.taxid) : null,
      focused_name: focusNode ? String(focusNode.name || "") : null,
      selected_taxids: selectedTaxids,
      expanded_taxids: expandedTaxids,
      collapsed_taxids: [],
    },
    report: currentReport,
    backend: {
      connected: backendConnected,
      backend_nodes_file: state.remote.backendNodesFile || null,
      backend_names_file: state.remote.backendNamesFile || null,
      request_context: activeRequestContext,
    },
  };
}

function summarizeCurrentAgentReport(reportState) {
  if (!reportState || typeof reportState !== "object") return null;
  if (reportState.type === "subtree") {
    const summary = reportState.report?.summary && typeof reportState.report.summary === "object"
      ? reportState.report.summary
      : {};
    return {
      type: "subtree",
      selected_taxids: Array.isArray(summary.selected_taxids) ? summary.selected_taxids.map((value) => Number(value)) : [],
      selected_node_count: Number(summary.selected_node_count || 0),
    };
  }
  if (reportState.type === "rank") {
    const summary = reportState.report?.summary && typeof reportState.report.summary === "object"
      ? reportState.report.summary
      : {};
    return {
      type: "rank",
      selected_taxids: Array.isArray(summary.selected_taxids) ? summary.selected_taxids.map((value) => Number(value)) : [],
      rank_count: Array.isArray(reportState.report?.rows) ? reportState.report.rows.length : 0,
    };
  }
  return {
    type: String(reportState.type || "unknown"),
  };
}

function normalizeGraphContextForProviderContext(graphContext) {
  if (!graphContext || typeof graphContext !== "object") {
    return null;
  }
  return {
    dataset_names: Array.isArray(graphContext.datasets?.selected)
      ? graphContext.datasets.selected.map((name) => String(name))
      : [],
    nodes_file: graphContext.taxonomy?.nodes_file ? String(graphContext.taxonomy.nodes_file) : null,
    names_file: graphContext.taxonomy?.names_file ? String(graphContext.taxonomy.names_file) : null,
    min_reads: Number(graphContext.filters?.min_reads || 0),
    expanded_taxids: Array.isArray(graphContext.tree?.expanded_taxids)
      ? graphContext.tree.expanded_taxids.map((value) => Number(value))
      : [],
  };
}

function normalizeSelectedDatasetsForProvider(graphContext) {
  const datasets = graphContext?.datasets && typeof graphContext.datasets === "object"
    ? graphContext.datasets
    : {};
  return {
    ok: true,
    mode: "backend",
    tool: "list_selected_datasets",
    request: {},
    datasets: {
      selected: Array.isArray(datasets.selected)
        ? datasets.selected.map((name) => String(name))
        : [],
      count: Number(datasets.count || 0),
      total_reads: Number(datasets.total_reads || 0),
      direct_taxa: Number(datasets.direct_taxa || 0),
    },
    context: normalizeGraphContextForProviderContext(graphContext),
  };
}

function normalizeSelectedNodesForProvider(selectedNodes, graphContext) {
  const rows = Array.isArray(selectedNodes) ? selectedNodes : [];
  return {
    ok: true,
    mode: "backend",
    tool: "get_selected_nodes",
    request: {},
    count: rows.length,
    selected: rows.map((node) => ({
      taxid: Number(node?.taxid || 0),
      name: String(node?.name || ""),
      rank: String(node?.rank || ""),
      direct: Number(node?.direct || 0),
      subtree: Number(node?.subtree || 0),
      child_count: Number(node?.child_count || 0),
    })),
    context: normalizeGraphContextForProviderContext(graphContext),
  };
}

function normalizeNodeDetailsForProvider(payload, request = {}) {
  const node = payload?.node && typeof payload.node === "object" ? payload.node : {};
  return {
    ok: Boolean(payload?.ok),
    mode: "backend",
    tool: "get_node_details",
    request: {
      taxid: Number(request.taxid),
    },
    node: {
      taxid: Number(node.taxid || 0),
      name: String(node.name || ""),
      rank: String(node.rank || ""),
      parent: node.parent == null ? null : Number(node.parent),
      depth: Number(node.depth || 0),
      direct: Number(node.direct || 0),
      subtree: Number(node.subtree || 0),
      child_count: Number(node.child_count || 0),
      lineage: Array.isArray(node.lineage)
        ? node.lineage.map((entry) => ({
          taxid: Number(entry?.taxid || 0),
          name: String(entry?.name || ""),
          rank: String(entry?.rank || ""),
        }))
        : [],
      datasets: Array.isArray(node.datasets)
        ? node.datasets.map((entry) => ({
          dataset: String(entry?.dataset || ""),
          direct: Number(entry?.direct || 0),
          subtree: Number(entry?.subtree || 0),
        }))
        : [],
    },
    context: normalizeProviderRequestContext(payload?.request_context),
  };
}

function normalizeTableViewForProvider(payload, request = {}) {
  const rows = Array.isArray(payload?.rows) ? payload.rows : [];
  const target = payload?.target && typeof payload.target === "object" ? payload.target : null;
  const scope = request.scope === "node" ? "node" : "root";
  const sort = request.sort === "subtree" ? "subtree" : "direct";
  const limit = normalizeToolLimit(request.limit, 40);
  return {
    ok: Boolean(payload?.ok),
    mode: "backend",
    tool: "get_table_view",
    request: {
      scope,
      taxid: scope === "node" && request.taxid != null ? Number(request.taxid) : null,
      sort,
      limit,
    },
    target: target ? {
      taxid: Number(target.taxid || 0),
      name: String(target.name || ""),
      rank: String(target.rank || ""),
      direct: Number(target.direct || 0),
      subtree: Number(target.subtree || 0),
      child_count: Number(target.child_count || 0),
    } : null,
    row_count: Number(payload?.row_count ?? rows.length),
    rows: rows.map((row) => ({
      taxid: Number(row?.taxid || 0),
      name: String(row?.name || ""),
      rank: String(row?.rank || ""),
      depth: Number(row?.depth || 0),
      direct: Number(row?.direct || 0),
      subtree: Number(row?.subtree || 0),
      child_count: Number(row?.child_count || 0),
    })),
    context: normalizeProviderRequestContext(payload?.request_context),
  };
}

function requireAgentBackendConnection() {
  if (!state.remote.connected) {
    throw new Error("Unicorn agent tools require a backend connection. Start the graphengine backend locally or remotely, connect it, and try again.");
  }
}

function requireAgentBackendTree() {
  requireAgentBackendConnection();
  if (!state.remote.serverTreeActive || !state.tree) {
    throw new Error("Unicorn agent tools require a backend-backed tree. Render a tree from the backend first.");
  }
}

function getActiveNodesFilename() {
  const requestContext = getActiveBackendRequestContext();
  if (requestContext?.nodes_file) return requestContext.nodes_file;
  if (els.nodesFile?.files?.[0]) return els.nodesFile.files[0].name;
  if (state.remote.backendNodesFile) return state.remote.backendNodesFile;
  return null;
}

function getActiveNamesFilename() {
  const requestContext = getActiveBackendRequestContext();
  if (requestContext?.names_file) return requestContext.names_file;
  if (els.namesFile?.files?.[0]) return els.namesFile.files[0].name;
  if (state.remote.backendNamesFile) return state.remote.backendNamesFile;
  return null;
}

function clearAgentTranscript() {
  state.agent.history = [];
  renderAgentTranscript();
  setStatus("Cleared agent transcript.");
}

function pushAgentEntry(entry) {
  state.agent.history.push({
    timestamp: new Date().toISOString(),
    role: String(entry.role || "assistant"),
    message: String(entry.message || ""),
    tools: Array.isArray(entry.tools) ? entry.tools.map((tool) => String(tool)) : [],
    note: entry.note ? String(entry.note) : "",
  });
  renderAgentTranscript();
}

function renderAgentTranscript() {
  if (!els.agentTranscript || !els.agentMeta) return;
  const entries = state.agent.history;
  const providerMeta = unicornAgentProviderAdapter.getCurrentProviderMeta();
  const providerLabel = providerMeta?.label || state.agent.runtimeProvider || "unknown";
  const providerMode = providerMeta?.mode || "unknown";
  const transportLabel = providerMeta?.client_side ? "client-side only" : "backend transport";
  els.agentMeta.textContent = entries.length
    ? `${entries.length.toLocaleString()} transcript entr${entries.length === 1 ? "y" : "ies"} · provider: ${providerLabel} · ${providerMode} mode · ${transportLabel}`
    : `Provider: ${providerLabel} · ${providerMode} mode · ${transportLabel}.`;
  if (!entries.length) {
    els.agentTranscript.innerHTML = `<div class="agent-empty">Render a backend-backed tree, then agent replies will appear here.</div>`;
    return;
  }
  els.agentTranscript.innerHTML = entries.map((entry) => `
    <div class="agent-entry role-${escapeHtml(entry.role)}">
      <div class="agent-entry-head">
        <span class="agent-entry-role">${escapeHtml(entry.role)}</span>
        <span class="agent-entry-time">${escapeHtml(formatLogTime(entry.timestamp))}</span>
      </div>
      <div class="agent-entry-message">${escapeHtml(entry.message)}</div>
      ${entry.tools.length ? `
        <div class="agent-entry-tools">
          ${entry.tools.map((tool) => `<span class="agent-tool-badge">${escapeHtml(tool)}</span>`).join("")}
        </div>
      ` : ""}
      ${entry.note ? `<div class="agent-entry-note">${escapeHtml(entry.note)}</div>` : ""}
    </div>
  `).join("");
  els.agentTranscript.scrollTop = els.agentTranscript.scrollHeight;
  renderAgentProviderState();
}

function renderAgentProviderState() {
  if (!els.agentProviderState) return;
  const runtimeConfig = getAgentRuntimeConfig();
  const configuredProvider = runtimeConfig.configured_provider;
  const configuredModel = state.agent.configuredModel || "unset";
  const hasKey = Boolean(state.agent.apiKey);
  const baseUrlLabel = runtimeConfig.base_url || "provider default endpoint";
  let runtimeHint = "Mock runtime remains active.";
  if (runtimeConfig.runtime_provider === "openai") {
    runtimeHint = "Backend-side OpenAI transport is active through the graphengine server.";
  } else if (runtimeConfig.runtime_provider === "google") {
    runtimeHint = "Backend-side Google Gemini transport is active through the graphengine server.";
  } else if (configuredProvider === "google") {
    runtimeHint = "Enter a Google API key to switch the runtime adapter to backend-side Gemini transport.";
  } else {
    runtimeHint = "Enter an OpenAI API key to switch the runtime adapter to backend-side OpenAI transport.";
  }
  els.agentProviderState.textContent = `Configured target: ${configuredProvider} · model: ${configuredModel} · API key ${hasKey ? "entered" : "not entered"} · base URL: ${baseUrlLabel} · runtime adapter: ${runtimeConfig.runtime_provider} · transport: ${runtimeConfig.transport_mode}. ${runtimeHint}`;
}

function getProviderExposedToolNames(providerName = "openai") {
  if (AGENT_SUPPORTED_PROVIDER_TARGETS.includes(normalizeConfiguredAgentProvider(providerName))) {
    return AGENT_V1_READ_ONLY_TOOL_NAMES.slice();
  }
  return AGENT_V1_READ_ONLY_TOOL_NAMES.slice();
}

function buildProviderToolDefinitions(providerName = "openai") {
  return getProviderExposedToolNames(providerName).map((name) => ({
    name,
    description: describeAgentTool(name),
    input_schema: {},
  }));
}

function describeAgentTool(name) {
  const descriptions = {
    get_graph_context: "Returns the current Unicorn graph context snapshot.",
    list_selected_datasets: "Returns selected datasets and aggregate totals.",
    get_selected_nodes: "Returns the nodes currently selected in the graph.",
    get_node_details: "Returns detailed information for one taxon in the current graph context.",
    get_table_view: "Returns ranked rows for the current root or one node scope.",
    select_taxon: "Selects one taxon in the active Unicorn graph.",
    focus_taxon: "Focuses the graph on one taxon and centers the chart.",
    center_root: "Clears current focus and recenters on the root.",
  };
  return descriptions[name] || "Unicorn-native tool.";
}

function buildProviderRequestPayload(providerMeta, input, toolResults, iteration) {
  const configuredProvider = getConfiguredAgentProvider();
  return {
    provider: {
      name: String(configuredProvider || providerMeta?.name || AGENT_SUPPORTED_PROVIDER_TARGETS[0]),
      model: String(state.agent.configuredModel || "unset"),
    },
    system_prompt: "You are the Unicorn Graph Engine agent. Answer only from Unicorn context and Unicorn tool outputs. Use only Unicorn-native tools when needed, and do not invent unsupported facts.",
    user_prompt: String(input?.prompt || ""),
    graph_context: input?.context || buildAgentContext(),
    tools: buildProviderToolDefinitions(configuredProvider),
    conversation: state.agent.history
      .filter((entry) => entry.role === "user" || entry.role === "assistant")
      .map((entry) => ({
        role: entry.role,
        content: entry.message,
      })),
    tool_results: toolResults.map((toolResult) => ({
      tool_name: toolResult.tool_name,
      args: toolResult.args,
      result: toolResult.result,
    })),
    turn_config: {
      max_tool_iterations: AGENT_MAX_TOOL_ITERATIONS,
      iteration,
    },
  };
}

async function executeAgentProviderTurn(input) {
  const provider = unicornAgentProviderAdapter.getCurrentProviderMeta();
  if (!provider) {
    throw new Error("No active agent provider is configured.");
  }
  const implementation = unicornAgentProviderAdapter.getCurrentProviderImplementation();
  if (!implementation || typeof implementation.runRequest !== "function") {
    throw new Error("Active agent provider does not implement runRequest().");
  }

  const toolsUsed = [];
  const toolResults = [];
  let latestContext = input?.context || buildAgentContext();
  addClientLog("info", "agent", `Provider turn started for ${provider.label}.`);

  for (let iteration = 0; iteration < AGENT_MAX_TOOL_ITERATIONS; iteration++) {
    const requestPayload = buildProviderRequestPayload(provider, {
      ...input,
      context: latestContext,
    }, toolResults, iteration);
    const allowedToolNames = new Set(getProviderExposedToolNames(requestPayload.provider?.name));
    addClientLog("info", "agent", `Provider request payload prepared for iteration ${iteration + 1}.`);

    const rawResponse = await implementation.runRequest(requestPayload);
    const response = window.UnicornAgentProviderModule.normalizeInternalProviderResponse(rawResponse);
    addClientLog("info", "agent", `Provider responded with ${response?.type || "unknown"} on iteration ${iteration + 1}.`);

    if (response.type === "assistant_message") {
      const answer = String(response.content || "");
      const note = toolResults.length
        ? "Provider produced a grounded answer after Unicorn tool execution."
        : "Provider produced a direct answer without requiring Unicorn tools.";
      if (answer) {
        return {
          provider: implementation.name,
          provider_label: implementation.label,
          provider_mode: implementation.mode,
          client_side: implementation.clientSide,
          answer,
          toolsUsed: toolsUsed.slice(),
          note,
        };
      }
      continue;
    }

    if (response.type === "tool_call") {
      const toolName = String(response.tool_name || "");
      const args = response.args && typeof response.args === "object" ? response.args : {};
      if (!allowedToolNames.has(toolName)) {
        throw new Error(`Provider requested a Unicorn tool outside the V1 read-only scope: ${toolName}`);
      }
      if (!unicornAgentRegistry.listTools().includes(toolName)) {
        throw new Error(`Provider requested an unknown Unicorn tool: ${toolName}`);
      }
      const currentCallKey = serializeAgentToolCall(toolName, args);
      const previousToolResult = toolResults.length ? toolResults[toolResults.length - 1] : null;
      const previousCallKey = previousToolResult
        ? serializeAgentToolCall(previousToolResult.tool_name, previousToolResult.args)
        : "";
      if (previousToolResult && currentCallKey === previousCallKey) {
        addClientLog(
          "info",
          "agent",
          `Stopped repeated Unicorn tool call ${toolName} and synthesized a final answer from the existing tool result.`,
          JSON.stringify(args, null, 2),
        );
        const fallbackAnswer = synthesizeFinalAnswerFromToolResult(previousToolResult);
        if (fallbackAnswer) {
          return {
            provider: implementation.name,
            provider_label: implementation.label,
            provider_mode: implementation.mode,
            client_side: implementation.clientSide,
            answer: String(fallbackAnswer.answer || ""),
            toolsUsed: Array.isArray(fallbackAnswer.toolsUsed) && fallbackAnswer.toolsUsed.length
              ? fallbackAnswer.toolsUsed
              : toolsUsed,
            note: String(fallbackAnswer.note || ""),
          };
        }
        throw new Error(`Provider repeated the same Unicorn tool call without using the existing result: ${toolName}`);
      }
      addClientLog("info", "agent", `Executing Unicorn tool ${toolName}.`, JSON.stringify(args, null, 2));
      const result = await unicornAgentRegistry.invokeTool(toolName, args);
      toolsUsed.push(toolName);
      toolResults.push({
        tool_name: toolName,
        args,
        result,
      });
      pushAgentEntry({
        role: "tool",
        message: `Executed Unicorn tool: ${toolName}`,
        tools: [toolName],
      });
      latestContext = buildAgentContext();
      continue;
    }

    if (response.type === "final_answer") {
      return {
        provider: implementation.name,
        provider_label: implementation.label,
        provider_mode: implementation.mode,
        client_side: implementation.clientSide,
        answer: String(response.content || ""),
        toolsUsed: Array.isArray(response.tool_summary) && response.tool_summary.length
          ? response.tool_summary.map((tool) => String(tool))
          : toolsUsed,
        note: String(response.notes || ""),
      };
    }

    if (response.type === "error") {
      throw new Error(String(response.message || "Provider returned an error payload."));
    }

    throw new Error(`Provider adapter returned unsupported response type: ${response.type}`);
  }

  addClientLog("error", "agent", "Provider turn hit the max tool iteration limit.");
  throw new Error(`Provider turn exceeded the maximum of ${AGENT_MAX_TOOL_ITERATIONS} tool iterations.`);
}

async function runMockAgent(prompt, context = buildAgentContext()) {
  const result = await executeAgentProviderTurn({
    prompt,
    context,
  });
  return {
    answer: result.answer,
    toolsUsed: result.toolsUsed,
    note: result.note,
  };
}

async function runMockAgentLegacy(prompt, context = buildAgentContext()) {
  const toolsUsed = ["get_graph_context"];
  const lower = String(prompt || "").trim().toLowerCase();

  if (!context.backend.connected) {
    return {
      answer: "No Unicorn backend connection is active yet. Start the graphengine backend, connect to it, render a tree, then ask me about the current graph state.",
      toolsUsed,
      note: "Mock mode only. Agent grounding now assumes a backend-backed Unicorn session.",
    };
  }

  if (!context.tree.loaded) {
    return {
      answer: "No active backend-backed Unicorn tree is loaded yet. Render a tree first, then ask me about the current graph state.",
      toolsUsed,
      note: "Mock mode only. Agent grounding now assumes a backend-backed Unicorn session.",
    };
  }

  if (lower.includes("selected")) {
    toolsUsed.push("get_selected_nodes");
    const selected = await unicornAgentRegistry.invokeTool("get_selected_nodes", {});
    if (!selected.selected.length) {
      return {
        answer: "No nodes are currently selected in the graph.",
        toolsUsed,
        note: "Try selecting one or more taxa in the tree, then ask again.",
      };
    }
    const top = selected.selected.slice(0, 3)
      .map((node) => `${node.name} (${node.taxid})`)
      .join(", ");
    return {
      answer: `${selected.count} node${selected.count === 1 ? "" : "s"} ${selected.count === 1 ? "is" : "are"} currently selected. The current selection includes ${top}.`,
      toolsUsed,
      note: "Mock mode only. This summary comes from Unicorn's current selection state.",
    };
  }

  if (lower.includes("dataset") || lower.includes("sample")) {
    toolsUsed.push("list_selected_datasets");
    const datasets = await unicornAgentRegistry.invokeTool("list_selected_datasets", {});
    const names = datasets.datasets.selected.slice(0, 3).join(", ");
    return {
      answer: `${datasets.datasets.count} dataset${datasets.datasets.count === 1 ? "" : "s"} ${datasets.datasets.count === 1 ? "is" : "are"} active in the current ${datasets.mode} session. Total reads: ${Number(datasets.datasets.total_reads || 0).toLocaleString()}. Direct taxa: ${Number(datasets.datasets.direct_taxa || 0).toLocaleString()}.`,
      toolsUsed,
      note: names ? `Active datasets: ${names}${datasets.datasets.selected.length > 3 ? " ..." : ""}` : "No active datasets were reported.",
    };
  }

  if (lower.includes("damage") || lower.includes("top") || lower.includes("table") || lower.includes("rank")) {
    toolsUsed.push("get_table_view");
    const table = await unicornAgentRegistry.invokeTool("get_table_view", {
      scope: "root",
      sort: "direct",
      limit: 5,
    });
    const rows = Array.isArray(table.rows) ? table.rows : [];
    if (!rows.length) {
      return {
        answer: "I could not find any ranked rows in the current table scope.",
        toolsUsed,
        note: "Mock mode only. The table query returned no rows.",
      };
    }
    const first = rows[0];
    const preview = rows.slice(0, 3)
      .map((row) => `${row.name} (${Number(row.direct || 0).toLocaleString()} direct)`)
      .join(", ");
    return {
      answer: `From the current root table scope, ${first.name} is the strongest direct-read row with ${Number(first.direct || 0).toLocaleString()} direct reads and ${Number(first.subtree || 0).toLocaleString()} subtree reads.`,
      toolsUsed,
      note: `Top rows preview: ${preview}`,
    };
  }

  const taxid = extractTaxidFromPrompt(prompt);
  if (taxid != null) {
    toolsUsed.push("get_node_details");
    const details = await unicornAgentRegistry.invokeTool("get_node_details", { taxid });
    const node = details.node || {};
    return {
      answer: `${node.name || taxid} (${taxid}) is currently visible with ${Number(node.direct || 0).toLocaleString()} direct reads and ${Number(node.subtree || 0).toLocaleString()} subtree reads.`,
      toolsUsed,
      note: `Rank: ${node.rank || "NA"}. Filtered child count: ${Number(node.child_count || 0).toLocaleString()}.`,
    };
  }

  return {
    answer: `The current Unicorn session is using ${context.mode} mode with ${Number(context.datasets.count || 0).toLocaleString()} active dataset${Number(context.datasets.count || 0) === 1 ? "" : "s"} and ${Number(context.tree.visible_node_count || 0).toLocaleString()} visible nodes.`,
    toolsUsed,
    note: "Mock mode only. This response is grounded in the current backend-backed graph context.",
  };
}

function extractTaxidFromPrompt(prompt) {
  const match = String(prompt || "").match(/\b\d+\b/);
  if (!match) return null;
  const taxid = Number(match[0]);
  return Number.isFinite(taxid) ? taxid : null;
}

function normalizeToolTaxid(value) {
  const taxid = Number(value);
  return Number.isFinite(taxid) ? taxid : null;
}

function normalizeToolLimit(value, fallback) {
  const limit = Number(value);
  if (!Number.isFinite(limit) || limit <= 0) return fallback;
  return Math.max(1, Math.min(200, Math.round(limit)));
}

function detectUnsupportedAgentMutationRequest(prompt) {
  const text = String(prompt || "").trim();
  if (!text) return null;
  const lower = text.toLowerCase();
  const actionMatchers = [
    { action: "select", pattern: /^(please\s+)?select\b/ },
    { action: "focus", pattern: /^(please\s+)?focus\b/ },
    { action: "center_root", pattern: /^(please\s+)?center\b/ },
    { action: "collapse", pattern: /^(please\s+)?collapse\b/ },
    { action: "uncollapse", pattern: /^(please\s+)?uncollapse\b/ },
  ];
  for (const matcher of actionMatchers) {
    if (matcher.pattern.test(lower)) {
      return {
        action: matcher.action,
        taxid: extractTaxidFromPrompt(text),
      };
    }
  }
  return null;
}

function buildUnsupportedAgentMutationMessage(request) {
  const action = String(request?.action || "modify");
  const taxid = request?.taxid != null ? Number(request.taxid) : null;
  if (action === "select") {
    return taxid != null
      ? `I cannot select node ${taxid} from the provider-backed agent yet because the current Unicorn provider contract is read-only.`
      : "I cannot select nodes from the provider-backed agent yet because the current Unicorn provider contract is read-only.";
  }
  if (action === "focus") {
    return taxid != null
      ? `I cannot focus node ${taxid} from the provider-backed agent yet because the current Unicorn provider contract is read-only.`
      : "I cannot change graph focus from the provider-backed agent yet because the current Unicorn provider contract is read-only.";
  }
  if (action === "center_root") {
    return "I cannot recenter the graph from the provider-backed agent yet because the current Unicorn provider contract is read-only.";
  }
  if (action === "collapse" || action === "uncollapse") {
    return "I cannot change graph expansion state from the provider-backed agent yet because the current Unicorn provider contract is read-only.";
  }
  return "I cannot modify Unicorn graph state from the provider-backed agent yet because the current provider contract is read-only.";
}

function serializeAgentToolCall(toolName, args) {
  return `${String(toolName || "")}:${JSON.stringify(args && typeof args === "object" ? args : {})}`;
}

function synthesizeFinalAnswerFromToolResult(toolResult) {
  if (!toolResult || typeof toolResult !== "object") return null;
  const toolName = String(toolResult.tool_name || "");
  const result = toolResult.result && typeof toolResult.result === "object" ? toolResult.result : {};

  if (toolName === "get_node_details") {
    const node = result.node && typeof result.node === "object" ? result.node : {};
    const taxid = Number(node.taxid || toolResult.args?.taxid || 0);
    return {
      answer: `${node.name || taxid} (${taxid}) is currently visible with ${Number(node.direct || 0).toLocaleString()} direct reads and ${Number(node.subtree || 0).toLocaleString()} subtree reads.`,
      note: `Rank: ${node.rank || "NA"}. Filtered child count: ${Number(node.child_count || 0).toLocaleString()}.`,
      toolsUsed: ["get_node_details"],
    };
  }

  if (toolName === "get_selected_nodes") {
    const rows = Array.isArray(result.selected) ? result.selected : [];
    if (!rows.length) {
      return {
        answer: "No nodes are currently selected in the graph.",
        note: "Repeated provider tool request was stopped after Unicorn had already returned the current selection state.",
        toolsUsed: ["get_selected_nodes"],
      };
    }
    const count = Number(result.count || rows.length);
    const top = rows.slice(0, 3).map((node) => `${node.name} (${node.taxid})`).join(", ");
    return {
      answer: `${count} node${count === 1 ? "" : "s"} ${count === 1 ? "is" : "are"} currently selected. The current selection includes ${top}.`,
      note: "Repeated provider tool request was stopped after Unicorn had already returned the current selection state.",
      toolsUsed: ["get_selected_nodes"],
    };
  }

  if (toolName === "list_selected_datasets") {
    const datasets = result.datasets && typeof result.datasets === "object" ? result.datasets : {};
    return {
      answer: `${Number(datasets.count || 0)} dataset${Number(datasets.count || 0) === 1 ? "" : "s"} ${Number(datasets.count || 0) === 1 ? "is" : "are"} active in the current ${result.mode || "backend"} session. Total reads: ${Number(datasets.total_reads || 0).toLocaleString()}. Direct taxa: ${Number(datasets.direct_taxa || 0).toLocaleString()}.`,
      note: "Repeated provider tool request was stopped after Unicorn had already returned the dataset summary.",
      toolsUsed: ["list_selected_datasets"],
    };
  }

  if (toolName === "get_table_view") {
    const rows = Array.isArray(result.rows) ? result.rows : [];
    if (!rows.length) {
      return {
        answer: "I could not find any ranked rows in the current table scope.",
        note: "Repeated provider tool request was stopped after Unicorn had already returned an empty table result.",
        toolsUsed: ["get_table_view"],
      };
    }
    const first = rows[0];
    const preview = rows.slice(0, 3)
      .map((row) => `${row.name} (${Number(row.direct || 0).toLocaleString()} direct)`)
      .join(", ");
    return {
      answer: `From the current ${result.scope || "root"} table scope, ${first.name} is the strongest direct-read row with ${Number(first.direct || 0).toLocaleString()} direct reads and ${Number(first.subtree || 0).toLocaleString()} subtree reads.`,
      note: `Repeated provider tool request was stopped after Unicorn had already returned the table result. Top rows preview: ${preview}`,
      toolsUsed: ["get_table_view"],
    };
  }

  return null;
}

function summarizeNodeForAgent(node) {
  if (!node) return null;
  return {
    taxid: Number(node.taxid),
    name: String(node.name || ""),
    rank: String(node.rank || ""),
    direct: Number(node.direct || 0),
    subtree: Number(node.total || 0),
    child_count: nodeHasChildren(node) ? getVisibleChildCount(node) : 0,
  };
}

function getVisibleChildCount(node) {
  const children = Array.isArray(node.children) ? node.children : [];
  return children.filter((child) => Number(child.total || 0) > 0).length;
}

function handleAgentSend() {
  if (!els.agentPrompt) return;
  const prompt = els.agentPrompt.value.trim();
  if (!prompt) {
    setStatus("Enter an agent prompt to continue.");
    return;
  }
  pushAgentEntry({
    role: "user",
    message: prompt,
  });
  const unsupportedMutation = detectUnsupportedAgentMutationRequest(prompt);
  if (unsupportedMutation) {
    addClientLog(
      "info",
      "agent",
      "Rejected unsupported state-mutating agent request at the Unicorn boundary.",
      JSON.stringify(unsupportedMutation, null, 2),
    );
    pushAgentEntry({
      role: "assistant",
      message: buildUnsupportedAgentMutationMessage(unsupportedMutation),
      note: "The provider-backed agent currently exposes only the read-only Unicorn tool contract.",
    });
    els.agentPrompt.value = "";
    setStatus("Agent request was rejected because the current provider contract is read-only.");
    return;
  }
  const providerMeta = unicornAgentProviderAdapter.getCurrentProviderMeta();
  const context = buildAgentContext();
  setStatus(`${providerMeta?.label || "Agent"} is drafting a reply from Unicorn backend state...`);
  unicornAgentProviderAdapter.runTurn({
    prompt,
    context,
    registry: unicornAgentRegistry,
  })
    .then((response) => {
      console.log("[Unicorn Agent Context]", context);
      console.log("[Unicorn Agent Provider Response]", response);
      addClientLog("info", "agent", "Captured agent context in the browser runtime.");
      pushAgentEntry({
        role: "assistant",
        message: response.answer,
        tools: response.toolsUsed,
        note: response.note,
      });
      els.agentPrompt.value = "";
      setStatus(`${response.provider_label || "Agent"} reply rendered in the graphengine UI.`);
    })
    .catch((error) => {
      addClientLog("error", "agent", "Agent provider reply failed.", errorToDetail(error));
      pushAgentEntry({
        role: "assistant",
        message: `I could not build an agent reply from the current Unicorn state: ${error.message || error}`,
        note: state.agent.runtimeProvider === "openai"
          ? "Backend-side OpenAI transport is active. Check backend reachability, API key, and provider response details in the client log."
          : state.agent.runtimeProvider === "google"
            ? "Backend-side Google Gemini transport is active. Check backend reachability, API key, model access, and provider response details in the client log."
            : getConfiguredAgentProvider() === "google"
              ? "Mock runtime is still active. Enter a Google API key to switch the runtime adapter to backend-side Gemini transport."
            : "Mock runtime is still active. Enter an OpenAI API key to switch the runtime adapter to backend-side OpenAI transport.",
      });
      setStatus(`Agent provider reply failed: ${error.message || error}`);
    });
}

function formatNodeCountSummary(node) {
  const direct = Number(node?.direct || 0).toLocaleString();
  const subtree = Number((node?.subtree ?? node?.total) || 0).toLocaleString();
  const mode = getCountViewMode();
  if (mode === "direct") return `${direct} direct`;
  if (mode === "subtree") return `${subtree} cumulative`;
  return `${direct} direct / ${subtree} cumulative`;
}


function rankSortKey(rank) {
  const normalized = String(rank || "no rank").trim().toLowerCase();
  const index = RANK_DISPLAY_ORDER.indexOf(normalized);
  return index >= 0 ? index : RANK_DISPLAY_ORDER.length + normalized.charCodeAt(0);
}

function updateSelectToRankOptions() {
  if (!els.selectToRankValue) return;
  const previousValue = String(els.selectToRankValue.value || "");
  const rankSet = new Set();
  for (const node of state.flat) {
    const rank = String(node.rank || "no rank").trim();
    if (!rank) continue;
    rankSet.add(rank);
  }
  const ranks = Array.from(rankSet).sort((a, b) => {
    const aKey = rankSortKey(a);
    const bKey = rankSortKey(b);
    if (aKey !== bKey) return aKey - bKey;
    return a.localeCompare(b);
  });
  els.selectToRankValue.innerHTML = [
    `<option value="">Choose rank</option>`,
    ...ranks.map((rank) => `<option value="${escapeHtml(rank)}">${escapeHtml(rank)}</option>`),
  ].join("");
  if (ranks.includes(previousValue)) {
    els.selectToRankValue.value = previousValue;
  }
}

function redraw() {
  if (!state.tree) return;
  const minReads = 0;
  const search = els.searchBox.value.trim().toLowerCase();
  const visible = [];
  const links = [];
  const leaves = { count: 0 };
  collectVisible(state.tree, null, visible, links, leaves, minReads);
  layoutVisible(state.tree, new Set(visible));
  state.flat = visible;
  updateSelectToRankOptions();
  renderSvg(visible, links, search);
  renderSummary(visible);
  renderTopTable();
  syncBackendRuntimeUiState();
  const activeSeries = getVisibleSeries().length;
  const directTaxa = Number(state.remote.directTaxa || 0);
  setStatus(`Rendered ${visible.length.toLocaleString()} visible nodes from a backend tree with ${directTaxa.toLocaleString()} direct taxa across ${activeSeries.toLocaleString()} active dataset${activeSeries === 1 ? "" : "s"}.`);
}

function collectVisible(node, parent, nodes, links, leaves, minReads) {
  if (node !== state.tree && node.total <= 0) return false;
  // The minimum-read filter is defined on subtree totals, so any node below
  // the threshold is hidden and its nearest visible ancestor becomes the
  // rendered collapse point for that branch.
  if (node !== state.tree && node.total < minReads) return false;
  nodes.push(node);
  if (parent) links.push([parent, node]);
  let visibleChildren = 0;
  for (const child of node.children) {
    if (collectVisible(child, node, nodes, links, leaves, minReads)) visibleChildren++;
  }
  if (visibleChildren === 0) {
    node._leaf = leaves.count++;
  }
  return true;
}

function layoutVisible(root, visibleSet) {
  const rowGap = 34;
  const levelGap = 230;
  const top = 42;
  const left = 42;
  const setY = (node) => {
    const children = node.children.filter((child) => visibleSet.has(child));
    if (children.length === 0) {
      node.x = left + node.depth * levelGap;
      node.y = top + (node._leaf || 0) * rowGap;
      return node.y;
    }
    const ys = children.map(setY);
    node.x = left + node.depth * levelGap;
    node.y = ys.reduce((sum, y) => sum + y, 0) / ys.length;
    return node.y;
  };
  setY(root);
}

function renderSvg(nodes, links, search) {
  const maxDepth = nodes.reduce((m, n) => Math.max(m, n.depth), 0);
  const maxY = nodes.reduce((m, n) => Math.max(m, n.y || 0), 0);
  const width = Math.max(980, maxDepth * 230 + 420);
  const height = Math.max(520, maxY + 80);
  const mode = els.countMode.value;
  const maxValue = Math.max(1, ...nodes.map((n) => mode === "direct" ? n.direct : n.total));

  els.svg.setAttribute("width", width);
  els.svg.setAttribute("height", height);
  els.svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
  els.svg.innerHTML = "";

  const edgeLayer = svgEl("g", { class: "edges" });
  for (const [a, b] of links) {
    edgeLayer.appendChild(svgEl("path", {
      class: "edge",
      d: `M ${a.x} ${a.y} C ${a.x + 95} ${a.y}, ${b.x - 95} ${b.y}, ${b.x} ${b.y}`,
    }));
  }
  els.svg.appendChild(edgeLayer);

  const nodeLayer = svgEl("g", { class: "nodes" });
  for (const node of nodes) {
    const value = mode === "direct" ? node.direct : node.total;
    const match = search && (`${node.taxid} ${node.name}`.toLowerCase().includes(search));
    const dim = search && !match;
    const selected = state.selected.has(node.taxid);
    const group = svgEl("g", {
      class: `node${match ? " match" : ""}${dim ? " dim" : ""}${selected ? " selected" : ""}`,
      transform: `translate(${node.x}, ${node.y})`,
    });
    const radius = radiusFor(value, maxValue);
    group.appendChild(svgEl("circle", {
      class: "node-hit",
      r: Math.max(radius + 7, 12),
    }));
    renderNodePie(group, node, radius);
    group.appendChild(svgEl("text", {
      x: radius + 8,
      y: -5,
    }, labelFor(node)));
    group.appendChild(svgEl("text", {
      class: "count",
      x: radius + 8,
      y: 10,
    }, formatNodeCountSummary(node)));
    group.addEventListener("click", (event) => {
      if (performance.now() < state.suppressClicksUntil) return;
      if (event.ctrlKey || event.metaKey) {
        if (state.clickTimer) {
          clearTimeout(state.clickTimer);
          state.clickTimer = null;
        }
        toggleSelection(node);
        return;
      }
      if (state.clickTimer) clearTimeout(state.clickTimer);
      state.clickTimer = setTimeout(() => {
        state.clickTimer = null;
        toggleCollapse(node);
      }, 220);
    });
    group.addEventListener("dblclick", (event) => {
      if (performance.now() < state.suppressClicksUntil) return;
      event.preventDefault();
      if (state.clickTimer) {
        clearTimeout(state.clickTimer);
        state.clickTimer = null;
      }
      toggleFocus(node);
    });
    group.addEventListener("mouseenter", (event) => scheduleTooltip(event, node));
    group.addEventListener("mousemove", (event) => updateTooltipPosition(event));
    group.addEventListener("mouseleave", hideTooltip);
    nodeLayer.appendChild(group);
  }
  els.svg.appendChild(nodeLayer);
  if (state.centerOnNextRender) {
    requestCenterRoot();
    state.centerOnNextRender = false;
  }
}

function renderNodePie(group, node, radius) {
  const values = getNodeSeriesValues(node);
  const total = values.reduce((sum, value) => sum + value, 0);
  if (total <= 0) {
    group.appendChild(svgEl("circle", {
      r: radius,
      fill: fillFor(node),
    }));
    return;
  }
  const origin = -Math.PI / 2;
  let start = origin;
  let drawn = 0;
  for (let i = 0; i < values.length; i++) {
    const value = values[i];
    if (!value) continue;
    const fraction = value / total;
    const end = drawn + fraction >= 0.999999
      ? origin + (Math.PI * 2)
      : start + (Math.PI * 2 * fraction);
    group.appendChild(svgEl("path", {
      d: pieSlicePath(radius, start, end),
      fill: state.series[i]?.color || fillFor(node),
    }));
    start = end;
    drawn += fraction;
  }
  group.appendChild(svgEl("circle", {
    r: radius,
    fill: "none",
    stroke: "#1f2b2e",
    "stroke-width": "1.3",
  }));
}

function getNodeSeriesValues(node) {
  const values = els.countMode.value === "direct" ? node.directBySource : node.totalBySource;
  return values.map((value, index) => state.series[index]?.visible ? value : 0);
}

function pieSlicePath(radius, startAngle, endAngle) {
  if (Math.abs(endAngle - startAngle) >= Math.PI * 2 - 0.0001) {
    return [
      `M 0 ${-radius}`,
      `A ${radius} ${radius} 0 1 1 0 ${radius}`,
      `A ${radius} ${radius} 0 1 1 0 ${-radius}`,
      "Z",
    ].join(" ");
  }
  const x1 = Math.cos(startAngle) * radius;
  const y1 = Math.sin(startAngle) * radius;
  const x2 = Math.cos(endAngle) * radius;
  const y2 = Math.sin(endAngle) * radius;
  const largeArc = endAngle - startAngle > Math.PI ? 1 : 0;
  return `M 0 0 L ${x1} ${y1} A ${radius} ${radius} 0 ${largeArc} 1 ${x2} ${y2} Z`;
}

function requestCenterRoot() {
  requestAnimationFrame(() => {
    requestAnimationFrame(centerRoot);
  });
}

function centerRoot() {
  centerNode(state.tree);
}

function centerNode(node) {
  if (!node || !els.chartWrap) return;
  const rootX = node.x || 0;
  const rootY = node.y || 0;
  els.chartWrap.scrollTo({
    left: Math.max(0, rootX - 80),
    top: Math.max(0, rootY - (els.chartWrap.clientHeight / 2)),
    behavior: "auto",
  });
}

function toggleFocus(node) {
  if (state.focusTaxid === node.taxid) {
    state.focusTaxid = null;
    centerRoot();
    return;
  }
  state.focusTaxid = node.taxid;
  centerNode(node);
}

async function toggleCollapse(node) {
  if (!nodeHasChildren(node)) return;
  try {
    if (node.expanded) {
      state.remote.expandedTaxids.delete(node.taxid);
      const payload = await fetchRemoteVisibleTree({
        expandedTaxids: Array.from(state.remote.expandedTaxids),
      });
      applyRemoteVisiblePayload(payload);
    } else {
      const payload = await fetchRemoteVisibleTree({
        taxid: node.taxid,
        expandedTaxids: Array.from(state.remote.expandedTaxids),
      });
      applyRemoteVisiblePayload(payload);
    }
    redraw();
  } catch (error) {
    setStatus(`Could not update the backend tree view: ${error.message || error}`);
  }
}

function toggleSelection(node) {
  if (state.selected.has(node.taxid)) state.selected.delete(node.taxid);
  else state.selected.add(node.taxid);
  redraw();
}

function clearSelection() {
  if (!state.selected.size) {
    setStatus("No nodes are currently selected.");
    return;
  }
  state.selected.clear();
  clearSubtreeReportView();
  redraw();
}

function selectDescendants() {
  if (!state.selected.size) {
    setStatus("Select one or more nodes first, then use Select Descendants.");
    return;
  }

  const selectedNodes = getSelectedNodes();
  if (!selectedNodes.length) {
    setStatus("The current selection could not be resolved in the active tree.");
    return;
  }

  for (const node of selectedNodes) {
    addDescendantsToSelection(node);
  }
  redraw();
}

function selectToRank() {
  if (!state.selected.size) {
    setStatus("Select one or more nodes first, then use Select To Rank.");
    return;
  }
  const targetRank = String(els.selectToRankValue?.value || "").trim();
  if (!targetRank) {
    setStatus("Choose a target rank first, then use Select To Rank.");
    return;
  }
  const selectedNodes = getSelectedNodes();
  if (!selectedNodes.length) {
    setStatus("The current selection could not be resolved in the active tree.");
    return;
  }
  const nextSelection = new Set();
  for (const node of selectedNodes) {
    addDescendantsAtRankToSelection(node, targetRank, nextSelection);
  }
  if (!nextSelection.size) {
    setStatus(`No visible descendant nodes at rank ${targetRank} were found below the current selection.`);
    return;
  }
  state.selected = nextSelection;
  redraw();
  setStatus(`Selected ${nextSelection.size.toLocaleString()} visible node${nextSelection.size === 1 ? "" : "s"} at rank ${targetRank}.`);
}

function addDescendantsAtRankToSelection(node, targetRank, selection) {
  for (const child of node.children || []) {
    if (String(child.rank || "").trim() === targetRank) {
      selection.add(child.taxid);
    }
    addDescendantsAtRankToSelection(child, targetRank, selection);
  }
}

function addDescendantsToSelection(node) {
  for (const child of node.children) {
    state.selected.add(child.taxid);
    addDescendantsToSelection(child);
  }
}

function uncollapseSelected() {
  if (!state.selected.size) {
    setStatus("Select one or more nodes first, then use Uncollapse.");
    return;
  }

  const selectedNodes = getSelectedNodes();
  if (!selectedNodes.length) {
    setStatus("The current selection could not be resolved in the active tree.");
    return;
  }

  expandSelectedNodes(selectedNodes).catch((error) => {
    setStatus(`Could not expand the selected backend nodes: ${error.message || error}`);
  });
}

function uncollapseSelectedToTips() {
  if (!state.selected.size) {
    setStatus("Select one or more nodes first, then use Uncollapse Tips.");
    return;
  }

  const selectedNodes = getSelectedNodes();
  if (!selectedNodes.length) {
    setStatus("The current selection could not be resolved in the active tree.");
    return;
  }

  uncollapseSelectedToTipsRemote(selectedNodes);
}

async function expandSelectedNodes(selectedNodes) {
  const expandedTaxids = new Set(state.remote.expandedTaxids);
  for (const node of selectedNodes) {
    if (nodeHasChildren(node)) {
      expandedTaxids.add(node.taxid);
    }
  }
  const visiblePayload = await fetchRemoteVisibleTree({
    expandedTaxids: Array.from(expandedTaxids),
  });
  applyRemoteVisiblePayload(visiblePayload);
  redraw();
}

async function uncollapseSelectedToTipsRemote(selectedNodes) {
  try {
    const modelPayload = await fetchRemoteFullTreeModel();
    const fullTree = modelPayload?.tree ? buildRemoteTree(modelPayload.tree) : null;
    if (!fullTree) {
      setStatus("Could not load the full backend tree for Uncollapse Tips.");
      return;
    }
    const expandedTaxids = new Set(state.remote.expandedTaxids);
    for (const node of selectedNodes) {
      const target = findNodeByTaxid(fullTree, node.taxid);
      if (target) {
        collectExpandableTaxids(target, expandedTaxids);
      }
    }
    const visiblePayload = await fetchRemoteVisibleTree({
      expandedTaxids: Array.from(expandedTaxids),
    });
    applyRemoteVisiblePayload(visiblePayload);
    redraw();
  } catch (error) {
    setStatus(`Could not uncollapse selected nodes to tips on the backend: ${error.message || error}`);
  }
}

function getSelectedNodes() {
  if (!state.tree) return [];
  const nodes = [];
  const seen = new Set();
  walkTree(state.tree, (node) => {
    if (state.selected.has(node.taxid) && !seen.has(node.taxid)) {
      seen.add(node.taxid);
      nodes.push(node);
    }
  });
  return nodes;
}

function walkTree(node, visit) {
  visit(node);
  for (const child of node.children) {
    walkTree(child, visit);
  }
}

function findNodeByTaxid(node, taxid) {
  if (!node) return null;
  if (node.taxid === taxid) return node;
  for (const child of node.children || []) {
    const found = findNodeByTaxid(child, taxid);
    if (found) return found;
  }
  return null;
}

function collectExpandableTaxids(node, expandedTaxids) {
  if (nodeHasChildren(node)) {
    expandedTaxids.add(node.taxid);
  }
  for (const child of node.children || []) {
    collectExpandableTaxids(child, expandedTaxids);
  }
}

function nodeHasChildren(node) {
  if (typeof node.hasChildren === "boolean") return node.hasChildren;
  if (typeof node.childCount === "number") return node.childCount > 0;
  return Array.isArray(node.children) && node.children.length > 0;
}

function radiusFor(value, maxValue) {
  if (!value) return 4;
  const t = els.scaleMode.value === "linear" ? value / maxValue : Math.sqrt(value / maxValue);
  return 4 + t * 24;
}

function fillFor(node) {
  if (node.taxid === 0) return "#6f7a80";
  if (node.direct > 0 && nodeHasChildren(node)) return "#e2a44e";
  if (node.direct > 0) return "#c85f43";
  if (node.depth === 0) return "#255f75";
  return "#9cad9f";
}

function labelFor(node) {
  const name = node.name || String(node.taxid);
  return name.length > 34 ? `${name.slice(0, 31)}...` : name;
}

function positionTooltip(event) {
  els.tooltip.style.left = `${event.clientX + 14}px`;
  els.tooltip.style.top = `${event.clientY + 14}px`;
}

function scheduleTooltip(event, node) {
  if (state.tooltipTimer) clearTimeout(state.tooltipTimer);
  state.tooltipNode = node;
  state.tooltipPoint = { clientX: event.clientX, clientY: event.clientY };
  state.tooltipTimer = setTimeout(() => {
    state.tooltipTimer = null;
    if (state.tooltipNode !== node) return;
    showTooltip(state.tooltipPoint || event, node);
  }, 360);
}

function updateTooltipPosition(event) {
  state.tooltipPoint = { clientX: event.clientX, clientY: event.clientY };
  if (els.tooltip.hidden) return;
  positionTooltip(event);
}

function showTooltip(event, node) {
  showBackendTooltip(event, node);
}

async function showBackendTooltip(event, node) {
  const requestId = ++state.remote.tooltipRequestId;
  els.tooltip.hidden = false;
  positionTooltip(event);
  els.tooltip.innerHTML = `
    <strong>${escapeHtml(node.name)}</strong>
    taxid: ${node.taxid}<br>
    loading tooltip...
  `;
  try {
    const payload = await fetchRemoteNodeTooltip(node.taxid);
    if (requestId !== state.remote.tooltipRequestId) return;
    if (state.tooltipNode !== node) return;
    const remoteNode = payload.node || {};
    const countMode = getCountViewMode();
    const breakdown = Array.isArray(remoteNode.datasets)
      ? remoteNode.datasets
        .filter((entry) => Number(entry.subtree || 0) > 0 || Number(entry.direct || 0) > 0)
        .sort((a, b) => {
          if (countMode === "direct") return Number(b.direct || 0) - Number(a.direct || 0);
          return Number(b.subtree || 0) - Number(a.subtree || 0);
        })
        .map((entry, index) => `
          <div class="tooltip-source">
            <span class="tooltip-swatch" style="background:${getDatasetColor(entry.dataset, index)}"></span>
            <span>${escapeHtml(entry.dataset)}: ${formatNodeCountSummary(entry)}</span>
          </div>
        `)
        .join("")
      : "";
    els.tooltip.hidden = false;
    positionTooltip(state.tooltipPoint || event);
    els.tooltip.innerHTML = `
      <strong>${escapeHtml(remoteNode.name || node.name)}</strong>
      taxid: ${Number(remoteNode.taxid ?? node.taxid)}<br>
      rank: ${escapeHtml(remoteNode.rank || node.rank || "NA")}<br>
      counts: ${escapeHtml(formatNodeCountSummary(remoteNode))}<br>
      children: ${Number(remoteNode.child_count || 0).toLocaleString()}<br>
      ${breakdown ? `<div class="tooltip-breakdown"><em>per dataset</em>${breakdown}</div>` : ""}
    `;
  } catch (error) {
    if (requestId !== state.remote.tooltipRequestId) return;
    if (state.tooltipNode !== node) return;
    els.tooltip.hidden = false;
    positionTooltip(state.tooltipPoint || event);
    els.tooltip.innerHTML = `
      <strong>${escapeHtml(node.name)}</strong>
      taxid: ${node.taxid}<br>
      ${escapeHtml(error.message || "Could not load tooltip.")}
    `;
  }
}

function hideTooltip() {
  if (state.tooltipTimer) {
    clearTimeout(state.tooltipTimer);
    state.tooltipTimer = null;
  }
  state.remote.tooltipRequestId++;
  state.tooltipNode = null;
  state.tooltipPoint = null;
  els.tooltip.hidden = true;
}

function renderSummary(visible) {
  const totalReads = hasBackendTree() ? state.remote.totalReads : 0;
  const directTaxa = hasBackendTree() ? state.remote.directTaxa : 0;
  els.readCount.textContent = totalReads.toLocaleString();
  els.taxonCount.textContent = directTaxa.toLocaleString();
  els.visibleCount.textContent = visible.length.toLocaleString();
  els.missingCount.textContent = state.missingTaxids.size.toLocaleString();
  els.selectedCount.textContent = state.selected.size.toLocaleString();
}

function renderTopTable() {
  if (state.remote.currentReport) {
    renderCurrentReportView();
    return;
  }
  clearSubtreeReportView({ preserveTableContext: true });
  renderBackendTopTable();
}

function clearSubtreeReportView(options = {}) {
  const preserveTableContext = Boolean(options.preserveTableContext);
  state.remote.currentReport = null;
  if (!preserveTableContext) {
    state.remote.currentTable = null;
  }
  if (els.subtreeReport) {
    els.subtreeReport.hidden = true;
    els.subtreeReport.innerHTML = "";
  }
  if (els.tablePanelTitle) {
    const targetName = state.remote.currentTable?.target?.name;
    els.tablePanelTitle.textContent = targetName
      ? `Top direct placements: ${targetName}`
      : "Top direct placements";
  }
  if (els.exportMatrixBtn) {
    els.exportMatrixBtn.hidden = true;
  }
  if (els.pcoaMetric) {
    els.pcoaMetric.hidden = true;
  }
  if (els.pcoaBtn) {
    els.pcoaBtn.hidden = true;
  }
  if (els.barplotBtn) {
    els.barplotBtn.hidden = true;
  }
  if (els.topTableWrap) {
    els.topTableWrap.hidden = false;
  }
}

async function openSelectedSubtreeReport() {
  if (!hasBackendTree()) {
    setStatus("Render a backend-backed tree first, then request a count matrix.");
    return;
  }
  const selectedNodes = getSelectedNodes();
  if (!selectedNodes.length) {
    setStatus("Select one or more nodes first, then use Count Matrix.");
    return;
  }
  const taxids = selectedNodes.map((node) => node.taxid);
  setTablePanelVisible(true);
  const requestId = ++state.remote.tableRequestId;
  if (!els.subtreeReport || !els.tablePanelTitle) {
    setStatus("Count matrix UI is not available in the current HTML shell. Try a hard refresh.");
    return;
  }
  els.tablePanelTitle.textContent = "Count matrix";
  els.subtreeReport.hidden = false;
  els.subtreeReport.innerHTML = `<div class="subtree-report-card">Loading count matrix for ${taxids.length} selected node(s)...</div>`;
  els.topTable.innerHTML = "";
  try {
    const payload = await fetchRemoteSubtreeReport(null, { taxids });
    if (requestId !== state.remote.tableRequestId) return;
    renderSubtreeReport(payload.report || null);
  } catch (error) {
    if (requestId !== state.remote.tableRequestId) return;
    els.subtreeReport.hidden = false;
    els.subtreeReport.innerHTML = `
      <div class="subtree-report-card">
        ${escapeHtml(error.message || "Could not load count matrix.")}
      </div>
    `;
    els.topTable.innerHTML = "";
  }
}

async function openSelectedRankReport() {
  if (!hasBackendTree()) {
    setStatus("Render a backend-backed tree first, then request a rank report.");
    return;
  }
  const selectedNodes = getSelectedNodes();
  if (!selectedNodes.length) {
    setStatus("Select one or more nodes first, then use Rank Report.");
    return;
  }
  const taxids = selectedNodes.map((node) => node.taxid);
  setTablePanelVisible(true);
  const requestId = ++state.remote.tableRequestId;
  if (!els.subtreeReport || !els.tablePanelTitle) {
    setStatus("Rank report UI is not available in the current HTML shell. Try a hard refresh.");
    return;
  }
  els.tablePanelTitle.textContent = "Rank report";
  els.subtreeReport.hidden = false;
  els.subtreeReport.innerHTML = `<div class="subtree-report-card">Loading rank report for ${taxids.length} selected node(s)...</div>`;
  els.topTable.innerHTML = "";
  try {
    const payload = await fetchRemoteRankReport(taxids);
    if (requestId !== state.remote.tableRequestId) return;
    renderRankReport(payload.report || null, taxids);
  } catch (error) {
    if (requestId !== state.remote.tableRequestId) return;
    els.subtreeReport.hidden = false;
    els.subtreeReport.innerHTML = `
      <div class="subtree-report-card">
        ${escapeHtml(error.message || "Could not load rank report.")}
      </div>
    `;
    els.topTable.innerHTML = "";
  }
}

async function refreshCurrentReportIfNeeded() {
  if (!hasBackendTree() || !state.remote.currentReport) return;
  if (state.remote.currentReport.type === "subtree") {
    const taxids = Array.isArray(state.remote.currentReport.report?.summary?.selected_taxids)
      ? state.remote.currentReport.report.summary.selected_taxids
      : [];
    if (!taxids.length) return;
    const payload = await fetchRemoteSubtreeReport(null, { taxids });
    renderSubtreeReport(payload.report || null);
    return;
  }
  if (state.remote.currentReport.type === "rank") {
    const taxids = Array.isArray(state.remote.currentReport.report?.summary?.selected_taxids)
      ? state.remote.currentReport.report.summary.selected_taxids
      : [];
    if (!taxids.length) return;
    const payload = await fetchRemoteRankReport(taxids);
    renderRankReport(payload.report || null, taxids);
  }
}

function renderCurrentReportView() {
  const active = state.remote.currentReport;
  if (!active) return;
  if (active.type === "subtree") {
    renderSubtreeReport(active.report);
    return;
  }
  if (active.type === "rank") {
    const taxids = Array.isArray(active.report?.summary?.selected_taxids) ? active.report.summary.selected_taxids : [];
    renderRankReport(active.report, taxids);
  }
}

function renderSubtreeReport(report) {
  if (!els.subtreeReport || !els.tablePanelTitle) return;
  if (!report || !report.summary || !report.matrix) {
    els.subtreeReport.hidden = false;
    els.subtreeReport.innerHTML = `<div class="subtree-report-card">No count matrix was returned by the backend.</div>`;
    els.topTable.innerHTML = "";
    return;
  }
  state.remote.currentReport = { type: "subtree", report };
  const countViewMode = getCountViewMode();
  const countViewLabel = getCountViewLabel();
  els.tablePanelTitle.textContent = `Count matrix (${Number(report.summary.selected_node_count || 0).toLocaleString()} selected)`;
  if (els.exportMatrixBtn) {
    els.exportMatrixBtn.hidden = false;
  }
  if (els.pcoaMetric) {
    els.pcoaMetric.hidden = countViewMode === "both";
  }
  if (els.pcoaBtn) {
    els.pcoaBtn.hidden = countViewMode === "both";
  }
  if (els.barplotBtn) {
    els.barplotBtn.hidden = countViewMode === "both";
  }
  if (els.topTableWrap) {
    els.topTableWrap.hidden = true;
  }
  els.subtreeReport.hidden = false;
  els.subtreeReport.innerHTML = `
    <div class="subtree-report-card">
      <h3>Selected-node ${escapeHtml(countViewLabel)} count matrix</h3>
      <div class="subtree-report-meta">
        <span>selected nodes: ${Number(report.summary.selected_node_count || 0).toLocaleString()}</span>
        <span>${escapeHtml(formatCountSummaryLabel(countViewMode))}: ${escapeHtml(formatCountSummaryValue(report.summary, countViewMode))}</span>
        <span>datasets: ${Array.isArray(report.summary.dataset_names) ? report.summary.dataset_names.length.toLocaleString() : "0"}</span>
      </div>
    </div>
    <div class="subtree-report-grid">
      <div class="subtree-report-card subtree-report-matrix">
        <h3>Selected-node ${escapeHtml(countViewLabel)} counts by dataset</h3>
        ${renderSubtreeMatrix(report.matrix || {})}
      </div>
    </div>
  `;
}

function renderRankReport(report, taxids) {
  if (!els.subtreeReport || !els.tablePanelTitle) return;
  if (!report || !report.summary) {
    els.subtreeReport.hidden = false;
    els.subtreeReport.innerHTML = `<div class="subtree-report-card">No rank report was returned by the backend.</div>`;
    els.topTable.innerHTML = "";
    return;
  }
  state.remote.currentReport = {
    type: "rank",
    report,
  };
  els.tablePanelTitle.textContent = `Rank report (${report.summary.selected_node_count} selected)`;
  if (els.exportMatrixBtn) {
    els.exportMatrixBtn.hidden = false;
  }
  if (els.pcoaMetric) {
    els.pcoaMetric.hidden = true;
  }
  if (els.pcoaBtn) {
    els.pcoaBtn.hidden = true;
  }
  if (els.barplotBtn) {
    els.barplotBtn.hidden = true;
  }
  if (els.topTableWrap) {
    els.topTableWrap.hidden = true;
  }
  els.subtreeReport.hidden = false;
  els.subtreeReport.innerHTML = `
    <div class="subtree-report-card">
      <h3>Selected-node direct-read count matrix by rank</h3>
      <div class="subtree-report-meta">
        <span>selected nodes: ${Number(report.summary.selected_node_count || 0).toLocaleString()}</span>
        <span>total direct reads: ${Number(report.summary.total_direct || 0).toLocaleString()}</span>
        <span>ranks: ${Array.isArray(report.rows) ? report.rows.length.toLocaleString() : "0"}</span>
      </div>
    </div>
    <div class="subtree-report-card subtree-report-matrix">
      <h3>Rank-by-dataset direct count matrix</h3>
      ${renderRankMatrix(report)}
    </div>
  `;
}

function renderRankMatrix(report) {
  const rows = Array.isArray(report.rows) ? report.rows : [];
  const datasetNames = Array.isArray(report.summary?.dataset_names) ? report.summary.dataset_names : [];
  if (!rows.length || !datasetNames.length) {
    return `<div class="subtree-report-empty">No rank-matrix rows available for the current selection.</div>`;
  }
  return `
    <table>
      <thead>
        <tr>
          <th>Rank</th>
          <th>Nodes</th>
          <th>Total direct</th>
          ${datasetNames.map((name) => `<th>${escapeHtml(name)}</th>`).join("")}
        </tr>
      </thead>
      <tbody>
        ${rows.map((row) => `
          <tr>
            <td>${escapeHtml(row.rank || "no rank")}</td>
            <td>${Number(row.node_count || 0).toLocaleString()}</td>
            <td>${Number(row.direct || 0).toLocaleString()}</td>
            ${(Array.isArray(row.datasets) ? row.datasets : []).map((entry) => `
              <td>${Number(entry.direct || 0).toLocaleString()}</td>
            `).join("")}
          </tr>
        `).join("")}
      </tbody>
    </table>
  `;
}

function renderDatasetBreakdownTable(rows, directOnly = false) {
  if (!Array.isArray(rows) || !rows.length) {
    return `<div class="subtree-report-empty">No per-dataset summary available.</div>`;
  }
  return `
    <table>
      <thead>
        <tr>
          <th>Dataset</th>
          <th>Direct</th>
          ${directOnly ? "" : "<th>Subtree</th>"}
        </tr>
      </thead>
      <tbody>
        ${rows.map((row) => `
          <tr>
            <td>${escapeHtml(row.dataset || "")}</td>
            <td>${Number(row.direct || 0).toLocaleString()}</td>
            ${directOnly ? "" : `<td>${Number(row.subtree || 0).toLocaleString()}</td>`}
          </tr>
        `).join("")}
      </tbody>
    </table>
  `;
}

function renderSubtreeMatrix(matrix) {
  const rows = Array.isArray(matrix.rows) ? matrix.rows : [];
  const datasetNames = Array.isArray(matrix.dataset_names) ? matrix.dataset_names : [];
  const countViewMode = getCountViewMode();
  if (!rows.length || !datasetNames.length) {
    return `<div class="subtree-report-empty">No count matrix rows available for the current selection.</div>`;
  }
  const valueHeader = countViewMode === "direct"
    ? "Direct"
    : countViewMode === "subtree"
      ? "Cumulative"
      : "Direct / Cumulative";
  return `
    <table>
      <thead>
        <tr>
          <th>Node</th>
          <th>Rank</th>
          <th>${valueHeader}</th>
          ${datasetNames.map((name) => `<th>${escapeHtml(name)}</th>`).join("")}
        </tr>
      </thead>
      <tbody>
        ${rows.map((row) => `
          <tr>
            <td>${escapeHtml(row.name || "")}</td>
            <td>${escapeHtml(row.rank || "NA")}</td>
            <td>${escapeHtml(formatMatrixCountCell(row, countViewMode))}</td>
            ${(Array.isArray(row.datasets) ? row.datasets : []).map((entry) => `
              <td>${escapeHtml(formatMatrixCountCell(entry, countViewMode))}</td>
            `).join("")}
          </tr>
        `).join("")}
      </tbody>
    </table>
  `;
}

function getDatasetColor(datasetName, index) {
  const matched = state.series.find((source) => source.label === datasetName);
  if (matched?.color) return matched.color;
  return SOURCE_COLORS[index % SOURCE_COLORS.length];
}

function formatCountSummaryLabel(mode) {
  if (mode === "direct") return "total direct reads";
  if (mode === "subtree") return "total cumulative reads";
  return "total counts";
}

function formatCountSummaryValue(summary, mode) {
  const direct = Number(summary?.total_direct || 0).toLocaleString();
  const subtree = Number(summary?.total_subtree || 0).toLocaleString();
  if (mode === "direct") return direct;
  if (mode === "subtree") return subtree;
  return `${direct} direct / ${subtree} cumulative`;
}

function formatMatrixCountCell(entry, mode) {
  const direct = Number(entry?.direct || 0).toLocaleString();
  const subtree = Number(entry?.subtree || 0).toLocaleString();
  if (mode === "direct") return direct;
  if (mode === "subtree") return subtree;
  return `${direct} / ${subtree}`;
}

async function fetchComputedBarplotSpec(taxids, countMode) {
  const activeRequestContext = getActiveBackendRequestContext();
  const datasetColors = Object.fromEntries(
    state.series.map((source, index) => [source.label, source.color || SOURCE_COLORS[index % SOURCE_COLORS.length]]),
  );
  const payload = {
    taxids: Array.isArray(taxids) ? taxids.map((value) => Number(value)) : [],
    files: activeRequestContext?.dataset_names?.length ? activeRequestContext.dataset_names.slice() : getSelectedRemoteDatasets(),
    nodes_file: activeRequestContext?.nodes_file ?? getActiveNodesFilename(),
    names_file: activeRequestContext?.names_file ?? getActiveNamesFilename(),
    min_reads: activeRequestContext && Number.isFinite(activeRequestContext.min_reads)
      ? Number(activeRequestContext.min_reads)
      : getMinReadsValue(),
    count_mode: countMode,
    dataset_colors: datasetColors,
  };
  const response = await fetch("http://localhost:8000/compute/barplot", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
    },
    body: JSON.stringify(payload),
  });
  const responsePayload = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(responsePayload?.detail?.message || `Remote compute/barplot request failed with HTTP ${response.status}`);
  }
  return responsePayload?.spec || null;
}

async function fetchComputedPcoaSpec(taxids, countMode, distanceMetric) {
  const activeRequestContext = getActiveBackendRequestContext();
  const datasetColors = Object.fromEntries(
    state.series.map((source, index) => [source.label, source.color || SOURCE_COLORS[index % SOURCE_COLORS.length]]),
  );
  const payload = {
    taxids: Array.isArray(taxids) ? taxids.map((value) => Number(value)) : [],
    files: activeRequestContext?.dataset_names?.length ? activeRequestContext.dataset_names.slice() : getSelectedRemoteDatasets(),
    nodes_file: activeRequestContext?.nodes_file ?? getActiveNodesFilename(),
    names_file: activeRequestContext?.names_file ?? getActiveNamesFilename(),
    min_reads: activeRequestContext && Number.isFinite(activeRequestContext.min_reads)
      ? Number(activeRequestContext.min_reads)
      : getMinReadsValue(),
    count_mode: countMode,
    distance_metric: distanceMetric,
    dataset_colors: datasetColors,
  };
  const response = await fetch("http://localhost:8000/compute/pcoa", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
    },
    body: JSON.stringify(payload),
  });
  const responsePayload = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(responsePayload?.detail?.message || `Remote compute/pcoa request failed with HTTP ${response.status}`);
  }
  return responsePayload?.spec || null;
}

async function openCurrentCountMatrixBarplot() {
  const active = state.remote.currentReport;
  if (!active || active.type !== "subtree") {
    setStatus("Open a count matrix first, then use Barplot.");
    return;
  }
  if (getCountViewMode() === "both") {
    setStatus("Barplot is available only in Direct or Cumulative view. Choose one count view first.");
    return;
  }
  const taxids = Array.isArray(active.report?.summary?.selected_taxids)
    ? active.report.summary.selected_taxids.map((value) => Number(value))
    : [];
  const spec = await fetchComputedBarplotSpec(taxids, getCountViewMode());
  if (!spec) {
    setStatus("No count matrix data is currently available for plotting.");
    return;
  }
  const popup = window.open("", "unicornCountMatrixBarplot", "popup=yes,width=1180,height=760,resizable=yes,scrollbars=yes");
  if (!popup) {
    setStatus("Could not open the barplot popup. Check whether your browser blocked popups for this page.");
    return;
  }
  const traces = Array.isArray(spec.traces)
    ? spec.traces.map((trace) => ({
      type: "bar",
      name: String(trace.name || ""),
      x: Array.isArray(spec.x) ? spec.x : [],
      y: Array.isArray(trace.y) ? trace.y.map((value) => Number(value || 0)) : [],
      marker: {
        color: trace.color || "#9cad9f",
      },
    }))
    : [];
  const layout = {
    title: String(spec.title || "Unicorn count matrix barplot"),
    barmode: "group",
    paper_bgcolor: "#ffffff",
    plot_bgcolor: "#ffffff",
    font: {
      family: "system-ui, sans-serif",
      size: 13,
      color: "#172026",
    },
    xaxis: {
      title: "Selected nodes",
      tickangle: -30,
      automargin: true,
    },
    yaxis: {
      title: `${getCountViewLabel()} reads`,
      automargin: true,
      separatethousands: true,
    },
    legend: {
      orientation: "h",
      y: 1.12,
    },
    margin: {
      l: 72,
      r: 24,
      t: 88,
      b: 140,
    },
  };
  const config = {
    responsive: true,
    displaylogo: false,
  };
  const tracesJson = JSON.stringify(traces).replace(/<\/script/gi, "<\\/script");
  const layoutJson = JSON.stringify(layout).replace(/<\/script/gi, "<\\/script");
  const configJson = JSON.stringify(config).replace(/<\/script/gi, "<\\/script");
  popup.document.open();
  popup.document.write(`<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Unicorn Count Matrix Barplot</title>
    <script src="https://cdn.plot.ly/plotly-3.6.0.min.js" charset="utf-8"></script>
    <style>
      html, body {
        margin: 0;
        padding: 0;
        width: 100%;
        height: 100%;
        background: #f4f0e8;
        color: #172026;
        font-family: system-ui, sans-serif;
      }
      #plot {
        width: 100%;
        height: 100vh;
      }
    </style>
  </head>
  <body>
    <div id="plot"></div>
    <script>
      const traces = ${tracesJson};
      const layout = ${layoutJson};
      const config = ${configJson};
      Plotly.newPlot("plot", traces, layout, config);
    </script>
  </body>
</html>`);
  popup.document.close();
  popup.focus();
  setStatus("Opened count matrix barplot in a popup window.");
}

async function openCurrentCountMatrixPcoa() {
  const active = state.remote.currentReport;
  if (!active || active.type !== "subtree") {
    setStatus("Open a count matrix first, then use PCoA.");
    return;
  }
  if (getCountViewMode() === "both") {
    setStatus("PCoA is available only in Direct or Cumulative view. Choose one count view first.");
    return;
  }
  const taxids = Array.isArray(active.report?.summary?.selected_taxids)
    ? active.report.summary.selected_taxids.map((value) => Number(value))
    : [];
  const metric = String(els.pcoaMetric?.value || "jaccard");
  const spec = await fetchComputedPcoaSpec(taxids, getCountViewMode(), metric);
  if (!spec) {
    setStatus("No count matrix data is currently available for PCoA.");
    return;
  }
  const popup = window.open("", "unicornCountMatrixPcoa", "popup=yes,width=1180,height=760,resizable=yes,scrollbars=yes");
  if (!popup) {
    setStatus("Could not open the PCoA popup. Check whether your browser blocked popups for this page.");
    return;
  }
  const points = Array.isArray(spec.points) ? spec.points : [];
  const axes = Array.isArray(spec.axes) ? spec.axes : [];
  const pc1 = axes[0] || { id: "PC1", explained_fraction: 0 };
  const pc2 = axes[1] || { id: "PC2", explained_fraction: 0 };
  const trace = {
    type: "scatter",
    mode: "markers+text",
    textposition: "top center",
    textfont: {
      family: "system-ui, sans-serif",
      size: 11,
      color: "#172026",
    },
    x: points.map((point) => Number(point.x || 0)),
    y: points.map((point) => Number(point.y || 0)),
    text: points.map((point) => String(point.label || point.dataset || "")),
    hovertemplate: [
      "<b>%{text}</b>",
      `${pc1.id}: %{x:.5f}`,
      `${pc2.id}: %{y:.5f}`,
      `<extra>${String(spec.distance_metric || "").replaceAll("_", "-")}</extra>`,
    ].join("<br>"),
    marker: {
      size: 14,
      color: points.map((point) => point.color || "#9cad9f"),
      line: {
        color: "#172026",
        width: 1,
      },
    },
  };
  const diagnostics = spec.diagnostics && typeof spec.diagnostics === "object" ? spec.diagnostics : {};
  const correctionText = diagnostics.correction_applied
    ? `Correction: ${diagnostics.correction_applied} (c=${Number(diagnostics.correction_constant || 0).toPrecision(4)})`
    : diagnostics.correction_required
      ? "Correction still required"
      : "No correction needed";
  const layout = {
    title: String(spec.title || "Unicorn count matrix PCoA"),
    paper_bgcolor: "#ffffff",
    plot_bgcolor: "#ffffff",
    font: {
      family: "system-ui, sans-serif",
      size: 13,
      color: "#172026",
    },
    xaxis: {
      title: `${pc1.id} (${(Number(pc1.explained_fraction || 0) * 100).toFixed(2)}%)`,
      zeroline: true,
      automargin: true,
    },
    yaxis: {
      title: `${pc2.id} (${(Number(pc2.explained_fraction || 0) * 100).toFixed(2)}%)`,
      zeroline: true,
      automargin: true,
    },
    annotations: [
      {
        xref: "paper",
        yref: "paper",
        x: 0,
        y: 1.12,
        xanchor: "left",
        yanchor: "bottom",
        showarrow: false,
        font: {
          family: "system-ui, sans-serif",
          size: 11,
          color: "#5b6570",
        },
        text: correctionText,
      },
    ],
    margin: {
      l: 72,
      r: 32,
      t: 104,
      b: 72,
    },
  };
  const config = {
    responsive: true,
    displaylogo: false,
  };
  const traceJson = JSON.stringify(trace).replace(/<\/script/gi, "<\\/script");
  const layoutJson = JSON.stringify(layout).replace(/<\/script/gi, "<\\/script");
  const configJson = JSON.stringify(config).replace(/<\/script/gi, "<\\/script");
  popup.document.open();
  popup.document.write(`<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Unicorn Count Matrix PCoA</title>
    <script src="https://cdn.plot.ly/plotly-3.6.0.min.js" charset="utf-8"></script>
    <style>
      html, body {
        margin: 0;
        padding: 0;
        width: 100%;
        height: 100%;
        background: #f4f0e8;
        color: #172026;
        font-family: system-ui, sans-serif;
      }
      #plot {
        width: 100%;
        height: 100vh;
      }
    </style>
  </head>
  <body>
    <div id="plot"></div>
    <script>
      const trace = ${traceJson};
      const layout = ${layoutJson};
      const config = ${configJson};
      Plotly.newPlot("plot", [trace], layout, config);
    </script>
  </body>
</html>`);
  popup.document.close();
  popup.focus();
  setStatus(`Opened count matrix PCoA (${metric.replaceAll("_", "-")}) in a popup window.`);
}

function exportCurrentSubtreeMatrix() {
  const active = state.remote.currentReport;
  if (!active) {
    setStatus("No report matrix is currently available to export.");
    return;
  }
  if (active.type === "rank") {
    exportCurrentRankMatrix(active.report);
    return;
  }
  const report = active.report;
  const matrix = report?.matrix;
  const rows = Array.isArray(matrix?.rows) ? matrix.rows : [];
  const datasetNames = Array.isArray(matrix?.dataset_names) ? matrix.dataset_names : [];
  const countViewMode = getCountViewMode();
  if (!report || !rows.length || !datasetNames.length) {
    setStatus("No count matrix is currently available to export.");
    return;
  }
  const header = ["taxid", "name", "rank"];
  if (countViewMode === "direct" || countViewMode === "both") {
    header.push("direct");
  }
  if (countViewMode === "subtree" || countViewMode === "both") {
    header.push("subtree");
  }
  datasetNames.forEach((datasetName) => {
    if (countViewMode === "direct" || countViewMode === "both") {
      header.push(countViewMode === "both" ? `${datasetName}_direct` : datasetName);
    }
    if (countViewMode === "subtree" || countViewMode === "both") {
      header.push(countViewMode === "both" ? `${datasetName}_subtree` : datasetName);
    }
  });
  const lines = [header.join("\t")];
  for (const row of rows) {
    const values = [
      String(row.taxid ?? ""),
      String(row.name ?? ""),
      String(row.rank ?? ""),
    ];
    if (countViewMode === "direct" || countViewMode === "both") {
      values.push(String(Number(row.direct || 0)));
    }
    if (countViewMode === "subtree" || countViewMode === "both") {
      values.push(String(Number(row.subtree || 0)));
    }
    datasetNames.forEach((datasetName, index) => {
      const entry = Array.isArray(row.datasets) ? row.datasets[index] : null;
      if (countViewMode === "direct" || countViewMode === "both") {
        values.push(String(Number(entry?.direct || 0)));
      }
      if (countViewMode === "subtree" || countViewMode === "both") {
        values.push(String(Number(entry?.subtree || 0)));
      }
    });
    lines.push(values.join("\t"));
  }
  const blob = new Blob([lines.join("\n") + "\n"], { type: "text/tab-separated-values;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  const safeName = "selected_nodes";
  anchor.href = url;
  anchor.download = `${safeName}_${getCountViewMode()}_matrix.tsv`;
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  URL.revokeObjectURL(url);
  setStatus(`Exported ${getCountViewLabel()} count matrix for the current selection.`);
}

function exportCurrentRankMatrix(report) {
  const rows = Array.isArray(report?.rows) ? report.rows : [];
  const datasetNames = Array.isArray(report?.summary?.dataset_names) ? report.summary.dataset_names : [];
  if (!rows.length || !datasetNames.length) {
    setStatus("No rank count matrix is currently available to export.");
    return;
  }
  const header = ["rank", "node_count", "total_direct", ...datasetNames];
  const lines = [header.join("\t")];
  for (const row of rows) {
    const values = [
      String(row.rank ?? ""),
      String(Number(row.node_count || 0)),
      String(Number(row.direct || 0)),
      ...datasetNames.map((_, index) => {
        const entry = Array.isArray(row.datasets) ? row.datasets[index] : null;
        return String(Number(entry?.direct || 0));
      }),
    ];
    lines.push(values.join("\t"));
  }
  const blob = new Blob([lines.join("\n") + "\n"], { type: "text/tab-separated-values;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = `selected_nodes_rank_direct_matrix.tsv`;
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  URL.revokeObjectURL(url);
  setStatus("Exported rank-by-dataset direct count matrix.");
}

async function renderBackendTopTable() {
  const requestId = ++state.remote.tableRequestId;
  els.topTable.innerHTML = `
    <tr>
      <td colspan="5">Loading server table...</td>
    </tr>
  `;
  try {
    const payload = await fetchRemoteTableView({
      scope: "root",
      sort: "direct",
      limit: 40,
    });
    if (requestId !== state.remote.tableRequestId) return;
    state.remote.currentTable = {
      scope: String(payload.scope || "root"),
      target: payload.target && typeof payload.target === "object"
        ? {
          taxid: Number(payload.target.taxid || 0),
          name: String(payload.target.name || ""),
          rank: String(payload.target.rank || ""),
        }
        : null,
      row_count: Number(payload.row_count || 0),
      request_context: normalizeProviderRequestContext(payload.request_context),
    };
    if (els.tablePanelTitle) {
      const targetName = state.remote.currentTable.target?.name;
      els.tablePanelTitle.textContent = targetName
        ? `Top direct placements: ${targetName}`
        : "Top direct placements";
    }
    const rows = Array.isArray(payload.rows) ? payload.rows : [];
    els.topTable.innerHTML = rows.map((row) => `
      <tr>
        <td>${Number(row.taxid).toLocaleString()}</td>
        <td>${escapeHtml(row.name || "")}</td>
        <td>${escapeHtml(row.rank || "NA")}</td>
        <td>${Number(row.direct || 0).toLocaleString()}</td>
        <td>${Number(row.subtree || 0).toLocaleString()}</td>
      </tr>
    `).join("");
    if (!rows.length) {
      els.topTable.innerHTML = `
        <tr>
          <td colspan="5">No rows passed the current server filters.</td>
        </tr>
      `;
    }
  } catch (error) {
    if (requestId !== state.remote.tableRequestId) return;
    els.topTable.innerHTML = `
      <tr>
        <td colspan="5">${escapeHtml(error.message || "Could not load server table.")}</td>
      </tr>
    `;
  }
}

function setStatus(message) {
  els.status.textContent = message;
}

function addClientLog(level, stage, message, detail = "") {
  const entry = {
    timestamp: new Date().toISOString(),
    level: String(level || "info"),
    stage: String(stage || "general"),
    message: String(message || ""),
    detail: String(detail || ""),
  };
  state.clientLog.unshift(entry);
  if (state.clientLog.length > 200) {
    state.clientLog.length = 200;
  }
  renderClientLog();
}

function clearClientLog() {
  state.clientLog = [];
  renderClientLog();
  setStatus("Cleared client log.");
}

function renderClientLog() {
  if (!els.clientLog || !els.clientLogMeta) return;
  const entries = state.clientLog;
  els.clientLogMeta.textContent = entries.length
    ? `${entries.length.toLocaleString()} recent client event${entries.length === 1 ? "" : "s"}`
    : "No client log entries yet";
  if (!entries.length) {
    els.clientLog.innerHTML = `<div class="remote-datasets-empty">Client-side upload, dataset, and tree requests will appear here.</div>`;
    return;
  }
  els.clientLog.innerHTML = entries.map((entry) => `
    <div class="client-log-entry">
      <div class="client-log-entry-head">
        <div class="client-log-badges">
          <span class="client-log-badge stage-${escapeHtml(entry.stage)}">${escapeHtml(entry.stage)}</span>
          <span class="client-log-badge level-${escapeHtml(entry.level)}">${escapeHtml(entry.level)}</span>
        </div>
        <span class="client-log-time">${escapeHtml(formatLogTime(entry.timestamp))}</span>
      </div>
      <div class="client-log-message">${escapeHtml(entry.message)}</div>
      ${entry.detail ? `<pre class="client-log-detail">${escapeHtml(entry.detail)}</pre>` : ""}
    </div>
  `).join("");
}

function initChartPan() {
  let pointerId = null;
  let startX = 0;
  let startY = 0;
  let startLeft = 0;
  let startTop = 0;
  let moved = false;

  els.chartWrap.addEventListener("click", (event) => {
    if (performance.now() < state.suppressClicksUntil) return;
    if (!(event.ctrlKey || event.metaKey)) return;
    if (event.target.closest(".node")) return;
    if (!state.selected.size) return;
    event.preventDefault();
    state.selected.clear();
    redraw();
  });

  els.chartWrap.addEventListener("pointerdown", (event) => {
    if (event.button !== 0) return;
    if (event.target.closest(".tooltip")) return;
    pointerId = event.pointerId;
    startX = event.clientX;
    startY = event.clientY;
    startLeft = els.chartWrap.scrollLeft;
    startTop = els.chartWrap.scrollTop;
    moved = false;
  });

  window.addEventListener("pointermove", (event) => {
    if (pointerId !== event.pointerId) return;
    const dx = event.clientX - startX;
    const dy = event.clientY - startY;
    if (!moved && (Math.abs(dx) > 3 || Math.abs(dy) > 3)) {
      moved = true;
      hideTooltip();
      els.chartWrap.classList.add("dragging");
      document.body.style.userSelect = "none";
    }
    if (!moved) return;
    els.chartWrap.scrollLeft = startLeft - dx;
    els.chartWrap.scrollTop = startTop - dy;
  });

  const stopPan = (event) => {
    if (pointerId !== event.pointerId) return;
    if (moved) state.suppressClicksUntil = performance.now() + 120;
    pointerId = null;
    moved = false;
    els.chartWrap.classList.remove("dragging");
    document.body.style.userSelect = "";
  };

  window.addEventListener("pointerup", stopPan);
  window.addEventListener("pointercancel", stopPan);
}
