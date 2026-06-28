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
const agent = unicornGraphEngine.agent;

if (!state || !els || !core || !treeModel || !backend || !treeRender || !ui || !selection || !reports || !agent) {
  throw new Error("Unicorn graphengine expected state, DOM, core, tree model, backend, tree render, UI, selection, reports, and agent modules to load before graphengine.js.");
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

const {
  initAgentControls,
  clearAgentTranscript,
  pushAgentEntry,
  renderAgentTranscript,
  buildAgentContext,
  createUnicornAgentRegistry,
  executeAgentProviderTurn,
  extractTaxidFromPrompt,
  handleAgentSend,
  syncAgentRuntimeProvider,
  getConfiguredAgentProvider,
  getAgentRuntimeConfig,
} = agent;

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
window.handleAgentSend = handleAgentSend;

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
