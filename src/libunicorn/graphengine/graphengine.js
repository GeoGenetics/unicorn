"use strict";

// Modularization transition note:
// `graphengine.js` is currently being split into smaller browser-side modules.
// During Pass 1 it still owns most runtime logic, but `state`, `els`, and boot
// wiring now live in dedicated files.
const unicornGraphEngine = window.UnicornGraphEngine = window.UnicornGraphEngine || {};
const state = unicornGraphEngine.state;
const els = unicornGraphEngine.els;
const core = unicornGraphEngine.core;
const treeModel = unicornGraphEngine.treeModel;
const backend = unicornGraphEngine.backend;
const treeRender = unicornGraphEngine.treeRender;
const ui = unicornGraphEngine.ui;
const selection = unicornGraphEngine.selection;
const reports = unicornGraphEngine.reports;

if (!state || !els || !core || !treeModel || !backend || !treeRender || !ui || !selection || !reports) {
  throw new Error("Unicorn graphengine expected state, DOM, core, tree model, backend, tree render, UI, selection, and reports modules to load before graphengine.js.");
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
  collectVisible,
  layoutVisible,
  walkTree,
  findNodeByTaxid,
  collectExpandableTaxids,
  nodeHasChildren,
  getVisibleChildCount,
  summarizeNodeForAgent,
} = treeModel;

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
  redraw,
  formatNodeCountSummary,
  renderSvg,
  renderNodePie,
  getNodeSeriesValues,
  pieSlicePath,
  requestCenterRoot,
  centerRoot,
  centerNode,
  radiusFor,
  fillFor,
  labelFor,
  positionTooltip,
  scheduleTooltip,
  updateTooltipPosition,
  showTooltip,
  showBackendTooltip,
  hideTooltip,
  renderSummary,
  initChartPan,
} = treeRender;

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

const {
  getSelectedNodes,
  getSingleSelectedNode,
  hasSelection,
  isTaxidSelected,
  getFocusedNode,
  clearFocus,
  toggleSelection,
  clearSelection,
  toggleFocus,
  selectDescendants,
  uncollapseSelected,
  uncollapseSelectedToTips,
  selectToRank,
  updateSelectToRankOptions,
} = selection;

const {
  renderTopTable,
  clearSubtreeReportView,
  openSelectedSubtreeReport,
  openSelectedRankReport,
  refreshCurrentReportIfNeeded,
  renderCurrentReportView,
  renderSubtreeReport,
  renderRankReport,
  renderRankMatrix,
  renderDatasetBreakdownTable,
  renderSubtreeMatrix,
  getDatasetColor,
  formatCountSummaryLabel,
  formatCountSummaryValue,
  formatMatrixCountCell,
  openCurrentCountMatrixBarplot,
  openCurrentCountMatrixPcoa,
  exportCurrentSubtreeMatrix,
  renderBackendTopTable,
} = reports;

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
  els.renderBtn.onclick = async (event) => {
    event.preventDefault();
    await window.loadAndRender();
  };
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
    clearFocus({ centerRoot: true });
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
        clearSelection();
      }
      if (!isTaxidSelected(node.taxid)) {
        toggleSelection(node);
      }
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
      const focusedNode = getFocusedNode();
      if (!focusedNode || focusedNode.taxid !== node.taxid) {
        toggleFocus(node);
      }
      redraw();
      return {
        ok: true,
        mode: "backend",
        focused: summarizeNodeForAgent(node),
      };
    },
    center_root: async () => {
      requireAgentBackendTree();
      const focusedNode = getFocusedNode();
      if (focusedNode) {
        toggleFocus(focusedNode);
      } else {
        clearFocus({ centerRoot: true });
      }
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
  const selectedTaxids = backendTreeReady
    ? getSelectedNodes().map((node) => Number(node.taxid))
    : [];
  const focusNode = backendTreeReady ? getFocusedNode() : null;
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
