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
window.__handleAgentSendImpl = handleAgentSend;
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

function getProviderExposedToolNames(providerName = "openai") {
  const normalizedProvider = AGENT_SUPPORTED_PROVIDER_TARGETS.includes(providerName)
    ? providerName
    : AGENT_SUPPORTED_PROVIDER_TARGETS[0];
  if (AGENT_SUPPORTED_PROVIDER_TARGETS.includes(normalizedProvider)) {
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
