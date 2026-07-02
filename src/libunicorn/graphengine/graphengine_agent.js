"use strict";

(function initUnicornGraphEngineAgent(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const state = namespace.state;
  const els = namespace.els;
  const core = namespace.core;
  const metadataResolver = namespace.metadata;
  const treeModel = namespace.treeModel;
  const backend = namespace.backend;
  const selection = namespace.selection;
  const ui = namespace.ui;

  if (!state || !els || !core || !metadataResolver || !treeModel || !backend || !selection || !ui) {
    throw new Error("Unicorn graphengine agent expected state, DOM, core, metadata, tree model, backend, selection, and UI modules to load first.");
  }

  const {
    AGENT_PROVIDER_MODELS,
    AGENT_SUPPORTED_PROVIDER_TARGETS,
    escapeHtml,
    formatLogTime,
  } = core;

  const {
    summarizeNodeForAgent,
    findNodeByTaxid,
  } = treeModel;

  const {
    getActiveBackendRequestContext,
    normalizeProviderRequestContext,
    getSelectedRemoteDatasets,
    fetchRemoteNodeTooltip,
    fetchRemoteTableView,
  } = backend;

  const {
    getSelectedNodes,
    getFocusedNode,
    clearSelection,
    isTaxidSelected,
    toggleSelection,
    toggleFocus,
    clearFocus,
  } = selection;

  const {
    getMinReadsValue,
  } = ui;

  // Public ownership during modularization transition:
  // agent UI state helpers, transcript rendering, context building, tool
  // registry exposure, and turn execution orchestration should converge here.

  function initAgentControls() {
    if (!els.agentProviderSelect || !els.agentModelSelect || !els.agentApiKey || !els.agentBaseUrl) return;
    els.agentProviderSelect.value = state.agent.configuredProvider;
    populateAgentModelOptions(state.agent.configuredProvider, state.agent.configuredModel);
    els.agentApiKey.value = state.agent.apiKey;
    els.agentBaseUrl.value = state.agent.baseUrl;
    syncAgentRuntimeProvider();
    renderAgentProviderState();

    if (els.agentSendBtn) {
      els.agentSendBtn.addEventListener("click", () => {
        if (typeof globalObject.handleAgentSend === "function") {
          globalObject.handleAgentSend();
        }
      });
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
          if (typeof globalObject.handleAgentSend === "function") {
            globalObject.handleAgentSend();
          }
        }
      });
    }
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

  function getKnownAgentProviderTargets() {
    const fromModels = Object.keys(AGENT_PROVIDER_MODELS || {}).filter(Boolean);
    const fromCore = Array.isArray(AGENT_SUPPORTED_PROVIDER_TARGETS)
      ? AGENT_SUPPORTED_PROVIDER_TARGETS.filter(Boolean)
      : [];
    const fromDom = els.agentProviderSelect
      ? Array.from(els.agentProviderSelect.options || [])
        .map((option) => String(option.value || "").trim())
        .filter(Boolean)
      : [];
    return Array.from(new Set([
      ...fromModels,
      ...fromCore,
      ...fromDom,
      "local_openai_compat",
    ]));
  }

  function handleAgentProviderChange() {
    if (!els.agentProviderSelect) return;
    const normalizedProvider = normalizeConfiguredAgentProvider(els.agentProviderSelect.value);
    state.agent.configuredProvider = normalizedProvider;
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
    const normalized = String(provider || "").trim();
    if (normalized === "local") {
      return "local_openai_compat";
    }
    if (normalized === "local_openai_compat") {
      return "local_openai_compat";
    }
    const knownTargets = getKnownAgentProviderTargets();
    return knownTargets.includes(normalized)
      ? normalized
      : knownTargets[0] || "openai";
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

  function shouldUseLocalOpenAICompatRuntime() {
    return getConfiguredAgentProvider() === "local_openai_compat";
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
    if (shouldUseLocalOpenAICompatRuntime()) {
      state.agent.runtimeProvider = "local_openai_compat";
      return;
    }
    state.agent.runtimeProvider = "mock";
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
    const providerMeta = globalObject.unicornAgentProviderAdapter?.getCurrentProviderMeta?.();
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
    } else if (runtimeConfig.runtime_provider === "local_openai_compat") {
      runtimeHint = "Backend-side local OpenAI-compatible transport is active through the graphengine server. API key is optional; base URL defaults to http://localhost:8542.";
    } else if (configuredProvider === "local_openai_compat") {
      runtimeHint = "Local OpenAI-compatible runtime is selected. Prompts will go through the graphengine backend to the configured base URL, or default to http://localhost:8542.";
    } else if (configuredProvider === "google") {
      runtimeHint = "Enter a Google API key to switch the runtime adapter to backend-side Gemini transport.";
    } else {
      runtimeHint = "Enter an OpenAI API key to switch the runtime adapter to backend-side OpenAI transport.";
    }
    els.agentProviderState.textContent = `Configured target: ${configuredProvider} · model: ${configuredModel} · API key ${hasKey ? "entered" : "not entered"} · base URL: ${baseUrlLabel} · runtime adapter: ${runtimeConfig.runtime_provider} · transport: ${runtimeConfig.transport_mode}. ${runtimeHint}`;
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
    const selectedTaxids = backendTreeReady
      ? getSelectedNodes().map((node) => Number(node.taxid))
      : [];
    const focusNode = backendTreeReady ? getFocusedNode() : null;
    const activeMetadataField = typeof metadataResolver.getActiveMetadataVisualizationField === "function"
      ? metadataResolver.getActiveMetadataVisualizationField()
      : null;
    const visibleRoot = backendTreeReady
      ? {
          taxid: Number(state.tree.taxid),
          name: String(state.tree.name || ""),
          rank: String(state.tree.rank || ""),
          subtree: Number(state.tree.total || 0),
          child_count: Array.isArray(state.tree.children) ? state.tree.children.length : 0,
        }
      : null;
    const focusedNode = focusNode
      ? {
          taxid: Number(focusNode.taxid),
          name: String(focusNode.name || ""),
          rank: String(focusNode.rank || ""),
          direct: Number(focusNode.direct || 0),
          subtree: Number(focusNode.total || 0),
          child_count: Number(focusNode.childCount || 0),
        }
      : null;
    const currentReport = backendTreeReady && state.remote.currentReport
      ? summarizeCurrentAgentReport(state.remote.currentReport)
      : null;
    const selectedDatasetNames = selectedDatasets.length > 0 && selectedDatasets.length <= 3
      ? selectedDatasets.slice()
      : [];

    return {
      captured_at: new Date().toISOString(),
      context_version: "agentic-v2-tiny",
      session: {
        backend_connected: backendConnected,
        runtime_provider: String(state.agent.runtimeProvider || "mock"),
        transport_mode: String(state.agent.transportMode || "backend"),
      },
      datasets: {
        count: selectedDatasets.length,
        names: selectedDatasetNames,
      },
      filters: {
        min_reads: activeRequestContext && Number.isFinite(activeRequestContext.min_reads)
          ? Number(activeRequestContext.min_reads)
          : getMinReadsValue(),
        count_mode: String(els.countMode?.value || "total"),
        scale_mode: String(els.scaleMode?.value || "sqrt"),
      },
      tree: {
        visible_root: visibleRoot,
        visible_node_count: backendTreeReady ? state.flat.length : 0,
        selected_node_count: selectedTaxids.length,
        focused_node: focusedNode,
      },
      metadata: activeMetadataField ? {
        active_field: String(activeMetadataField),
      } : null,
      report_state: currentReport ? {
        type: String(currentReport.type || "unknown"),
        selected_node_count: Number(currentReport.selected_node_count || currentReport.rank_count || 0),
      } : null,
    };
  }

  function buildSelectedDatasetsToolPayload(graphContext) {
    const selectedDatasetNames = getSelectedRemoteDatasets();
    const selectedDatasetSummaries = typeof globalObject.getSelectedRemoteDatasetSummaries === "function"
      ? globalObject.getSelectedRemoteDatasetSummaries()
      : [];
    return {
      ok: true,
      mode: "backend",
      tool: "list_selected_datasets",
      request: {},
      datasets: {
        selected: selectedDatasetNames,
        selected_summaries: selectedDatasetSummaries.map((dataset) => ({
          filename: String(dataset?.filename || ""),
          color: String(dataset?.color || ""),
          metadata: dataset?.metadata && typeof dataset.metadata === "object"
            ? dataset.metadata
            : null,
        })),
        count: selectedDatasetNames.length,
        total_reads: Number(state.remote.totalReads || 0),
        direct_taxa: Number(state.remote.directTaxa || 0),
      },
      context: normalizeGraphContextForProviderContext(graphContext),
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
      dataset_count: Number(graphContext.datasets?.count || 0),
      min_reads: Number(graphContext.filters?.min_reads || 0),
      visible_node_count: Number(graphContext.tree?.visible_node_count || 0),
      selected_node_count: Number(graphContext.tree?.selected_node_count || 0),
      active_metadata_field: graphContext.metadata?.active_field ? String(graphContext.metadata.active_field) : null,
      report_type: graphContext.report_state?.type ? String(graphContext.report_state.type) : null,
    };
  }

  function normalizeSelectedDatasetsForProvider(graphContext) {
    return buildSelectedDatasetsToolPayload(graphContext);
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

  function normalizeVisibleNodeMatchesForProvider(matches, request = {}, graphContext) {
    const rows = Array.isArray(matches) ? matches : [];
    const query = normalizeToolQuery(request.query);
    const limit = normalizeToolLimit(request.limit, 10);
    return {
      ok: true,
      mode: "backend",
      tool: "find_visible_nodes",
      request: {
        query,
        limit,
      },
      count: rows.length,
      matches: rows.map((row) => ({
        taxid: Number(row?.taxid || 0),
        name: String(row?.name || ""),
        rank: String(row?.rank || ""),
        direct: Number(row?.direct || 0),
        subtree: Number(row?.subtree || 0),
        child_count: Number(row?.child_count || 0),
        match_type: String(row?.match_type || ""),
      })),
      context: normalizeGraphContextForProviderContext(graphContext),
    };
  }

  function normalizeNodeDetailsForProvider(payload, request = {}) {
    const node = payload?.node && typeof payload.node === "object" ? payload.node : {};
    const nodes = Array.isArray(payload?.nodes) ? payload.nodes : [];
    const normalizedTaxids = Array.isArray(request.taxids)
      ? request.taxids.map((value) => Number(value)).filter((value) => Number.isFinite(value))
      : [];
    const requestTaxid = normalizedTaxids.length
      ? null
      : (request.taxid == null ? null : Number(request.taxid));
    const normalizedNodes = nodes.map((entry) => ({
      taxid: Number(entry?.taxid || 0),
      name: String(entry?.name || ""),
      rank: String(entry?.rank || ""),
      parent: entry?.parent == null ? null : Number(entry.parent),
      depth: Number(entry?.depth || 0),
      direct: Number(entry?.direct || 0),
      subtree: Number(entry?.subtree || 0),
      child_count: Number(entry?.child_count || 0),
      lineage: Array.isArray(entry?.lineage)
        ? entry.lineage.map((lineageEntry) => ({
          taxid: Number(lineageEntry?.taxid || 0),
          name: String(lineageEntry?.name || ""),
          rank: String(lineageEntry?.rank || ""),
        }))
        : [],
      datasets: Array.isArray(entry?.datasets)
        ? entry.datasets.map((datasetEntry) => ({
          dataset: String(datasetEntry?.dataset || ""),
          direct: Number(datasetEntry?.direct || 0),
          subtree: Number(datasetEntry?.subtree || 0),
        }))
        : [],
    }));
    return {
      ok: Boolean(payload?.ok),
      mode: "backend",
      tool: "get_node_details",
      request: {
        taxid: requestTaxid,
        taxids: normalizedTaxids,
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
      count: Number(
        payload?.count ?? (normalizedNodes.length || (payload?.node ? 1 : 0))
      ),
      nodes: normalizedNodes,
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

  function normalizeToolTaxid(value) {
    const taxid = Number(value);
    return Number.isFinite(taxid) ? taxid : null;
  }

  function normalizeToolTaxids(value) {
    const rawValues = Array.isArray(value)
      ? value
      : value == null
        ? []
        : [value];
    const seen = new Set();
    const taxids = [];
    rawValues.forEach((entry) => {
      const normalized = normalizeToolTaxid(entry);
      if (normalized == null || seen.has(normalized)) return;
      seen.add(normalized);
      taxids.push(normalized);
    });
    return taxids;
  }

  function normalizeToolLimit(value, fallback) {
    const limit = Number(value);
    if (!Number.isFinite(limit) || limit <= 0) return fallback;
    return Math.max(1, Math.min(200, Math.round(limit)));
  }

  function normalizeToolQuery(value) {
    return String(value || "").trim();
  }

  function findVisibleNodesByName(query, limit = 10) {
    const normalizedQuery = normalizeToolQuery(query).toLowerCase();
    if (!normalizedQuery) return [];
    const rows = [];
    const pushMatches = (matchType, predicate) => {
      const seen = new Set(rows.map((row) => row.taxid));
      (Array.isArray(state.flat) ? state.flat : []).forEach((node) => {
        const name = String(node?.name || "");
        const lowered = name.toLowerCase();
        if (!predicate(lowered)) return;
        const taxid = Number(node?.taxid || 0);
        if (seen.has(taxid)) return;
        seen.add(taxid);
        rows.push({
          ...summarizeNodeForAgent(node),
          match_type: matchType,
        });
      });
    };
    pushMatches("exact", (lowered) => lowered === normalizedQuery);
    pushMatches("prefix", (lowered) => lowered.startsWith(normalizedQuery));
    pushMatches("substring", (lowered) => lowered.includes(normalizedQuery));
    return rows.slice(0, normalizeToolLimit(limit, 10));
  }

  function createUnicornAgentRegistry() {
    const tools = {
      get_graph_context: async () => buildAgentContext(),
      list_selected_datasets: async () => {
        requireAgentBackendConnection();
        return buildSelectedDatasetsToolPayload(buildAgentContext());
      },
      get_selected_nodes: async () => {
        requireAgentBackendTree();
        return normalizeSelectedNodesForProvider(
          getSelectedNodes().map((node) => summarizeNodeForAgent(node)),
          buildAgentContext(),
        );
      },
      find_visible_nodes: async (args = {}) => {
        requireAgentBackendTree();
        const query = normalizeToolQuery(args.query);
        if (!query) {
          throw new Error("find_visible_nodes requires a non-empty query string.");
        }
        const limit = normalizeToolLimit(args.limit, 10);
        const matches = findVisibleNodesByName(query, limit);
        return normalizeVisibleNodeMatchesForProvider(matches, { query, limit }, buildAgentContext());
      },
      get_node_details: async (args = {}) => {
        requireAgentBackendTree();
        const taxidsFromTaxid = Array.isArray(args.taxid) ? normalizeToolTaxids(args.taxid) : [];
        const taxids = taxidsFromTaxid.length
          ? taxidsFromTaxid
          : normalizeToolTaxids(args.taxids);
        const taxid = taxids.length ? null : normalizeToolTaxid(args.taxid);
        if (taxid == null && !taxids.length) {
          throw new Error("get_node_details requires a numeric taxid or a non-empty taxids array.");
        }
        if (taxids.length > 1) {
          const payloads = await Promise.all(taxids.map((value) => fetchRemoteNodeTooltip(value)));
          return normalizeNodeDetailsForProvider({
            ok: payloads.every((payload) => Boolean(payload?.ok)),
            count: payloads.length,
            nodes: payloads.map((payload) => payload?.node).filter((payloadNode) => payloadNode && typeof payloadNode === "object"),
            request_context: payloads[0]?.request_context || null,
          }, { taxids });
        }
        const resolvedTaxid = taxids.length ? taxids[0] : taxid;
        const payload = await fetchRemoteNodeTooltip(resolvedTaxid);
        return normalizeNodeDetailsForProvider(payload, { taxid: resolvedTaxid });
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
        globalObject.redraw();
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
        globalObject.redraw();
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

  function getProviderExposedToolNames(providerName = "openai") {
    const normalizedProvider = AGENT_SUPPORTED_PROVIDER_TARGETS.includes(providerName)
      ? providerName
      : AGENT_SUPPORTED_PROVIDER_TARGETS[0];
    if (AGENT_SUPPORTED_PROVIDER_TARGETS.includes(normalizedProvider)) {
      return core.AGENT_V1_READ_ONLY_TOOL_NAMES.slice();
    }
    return core.AGENT_V1_READ_ONLY_TOOL_NAMES.slice();
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
      find_visible_nodes: "Finds visible nodes in the current graph by name and returns grounded taxid matches.",
      get_node_details: "Returns detailed information for one taxon or several taxa in the current graph context.",
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
        max_tool_iterations: core.AGENT_MAX_TOOL_ITERATIONS,
        iteration,
      },
    };
  }

  async function executeAgentProviderTurn(input) {
    const providerAdapter = getProviderAdapter();
    const provider = providerAdapter.getCurrentProviderMeta();
    if (!provider) {
      throw new Error("No active agent provider is configured.");
    }
    const implementation = providerAdapter.getCurrentProviderImplementation();
    if (!implementation || typeof implementation.runRequest !== "function") {
      throw new Error("Active agent provider does not implement runRequest().");
    }

    const registry = getAgentRegistry();
    const toolsUsed = [];
    const toolResults = [];
    let latestContext = input?.context || buildAgentContext();
    addClientLog("info", "agent", `Provider turn started for ${provider.label}.`);

    for (let iteration = 0; iteration < core.AGENT_MAX_TOOL_ITERATIONS; iteration++) {
      const requestPayload = buildProviderRequestPayload(provider, {
        ...input,
        context: latestContext,
      }, toolResults, iteration);
      const allowedToolNames = new Set(getProviderExposedToolNames(requestPayload.provider?.name));
      addClientLog("info", "agent", `Provider request payload prepared for iteration ${iteration + 1}.`, JSON.stringify(requestPayload, null, 2));

      const rawResponse = await implementation.runRequest(requestPayload);
      const response = globalObject.UnicornAgentProviderModule.normalizeInternalProviderResponse(rawResponse);
      addClientLog("info", "agent", `Provider responded with ${response?.type || "unknown"} on iteration ${iteration + 1}.`, JSON.stringify(response, null, 2));

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
        if (!registry.listTools().includes(toolName)) {
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
        const result = await registry.invokeTool(toolName, args);
        toolResults.push({
          tool_name: toolName,
          args,
          result,
        });
        if (!toolsUsed.includes(toolName)) {
          toolsUsed.push(toolName);
        }
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
    throw new Error(`Provider turn exceeded the maximum of ${core.AGENT_MAX_TOOL_ITERATIONS} tool iterations.`);
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

    if (!context.session?.backend_connected) {
      return {
        answer: "No Unicorn backend connection is active yet. Start the graphengine backend, connect to it, render a tree, then ask me about the current graph state.",
        toolsUsed,
        note: "Mock mode only. Agent grounding now assumes a backend-backed Unicorn session.",
      };
    }

    if (!context.tree?.visible_root) {
      return {
        answer: "No active backend-backed Unicorn tree is loaded yet. Render a tree first, then ask me about the current graph state.",
        toolsUsed,
        note: "Mock mode only. Agent grounding now assumes a backend-backed Unicorn session.",
      };
    }

    if (lower.includes("selected")) {
      toolsUsed.push("get_selected_nodes");
      const selected = await getAgentRegistry().invokeTool("get_selected_nodes", {});
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
      const datasets = await getAgentRegistry().invokeTool("list_selected_datasets", {});
      const names = datasets.datasets.selected.slice(0, 3).join(", ");
      return {
        answer: `${datasets.datasets.count} dataset${datasets.datasets.count === 1 ? "" : "s"} ${datasets.datasets.count === 1 ? "is" : "are"} active in the current ${datasets.mode} session. Total reads: ${Number(datasets.datasets.total_reads || 0).toLocaleString()}. Direct taxa: ${Number(datasets.datasets.direct_taxa || 0).toLocaleString()}.`,
        toolsUsed,
        note: names ? `Active datasets: ${names}${datasets.datasets.selected.length > 3 ? " ..." : ""}` : "No active datasets were reported.",
      };
    }

    if (lower.includes("damage") || lower.includes("top") || lower.includes("table") || lower.includes("rank")) {
      toolsUsed.push("get_table_view");
      const table = await getAgentRegistry().invokeTool("get_table_view", {
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
      const details = await getAgentRegistry().invokeTool("get_node_details", { taxid });
      const node = details.node || {};
      return {
        answer: `${node.name || taxid} (${taxid}) is currently visible with ${Number(node.direct || 0).toLocaleString()} direct reads and ${Number(node.subtree || 0).toLocaleString()} subtree reads.`,
        toolsUsed,
        note: `Rank: ${node.rank || "NA"}. Filtered child count: ${Number(node.child_count || 0).toLocaleString()}.`,
      };
    }

    return {
      answer: `The current Unicorn session is using backend mode with ${Number(context.datasets.count || 0).toLocaleString()} active dataset${Number(context.datasets.count || 0) === 1 ? "" : "s"} and ${Number(context.tree.visible_node_count || 0).toLocaleString()} visible nodes.`,
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

    if (toolName === "get_graph_context") {
      const datasets = result.datasets && typeof result.datasets === "object" ? result.datasets : {};
      const tree = result.tree && typeof result.tree === "object" ? result.tree : {};
      const filters = result.filters && typeof result.filters === "object" ? result.filters : {};
      const selectedCount = Number(tree.selected_node_count || 0);
      const root = tree.visible_root && typeof tree.visible_root === "object" ? tree.visible_root : null;
      const answerParts = [
        `${Number(datasets.count || 0).toLocaleString()} dataset${Number(datasets.count || 0) === 1 ? "" : "s"} ${Number(datasets.count || 0) === 1 ? "is" : "are"} active`,
        `${selectedCount.toLocaleString()} node${selectedCount === 1 ? "" : "s"} ${selectedCount === 1 ? "is" : "are"} selected`,
        `min_reads is ${Number(filters.min_reads || 0).toLocaleString()}`,
      ];
      if (root) {
        answerParts.push(`the visible root is ${root.name} (${root.taxid})`);
      }
      return {
        answer: `Current Unicorn tree state summary: ${answerParts.join(", ")}.`,
        note: "Repeated provider tool request was stopped after Unicorn had already returned the current graph context.",
        toolsUsed: ["get_graph_context"],
      };
    }

    if (toolName === "get_node_details") {
      const nodes = Array.isArray(result.nodes) ? result.nodes : [];
      if (nodes.length > 1) {
        const summary = nodes
          .map((entry) => `${entry.name} (${entry.taxid})`)
          .join(", ");
        return {
          answer: `${nodes.length} nodes were returned: ${summary}.`,
          note: "Repeated provider tool request was stopped after Unicorn had already returned multi-node detail payloads.",
          toolsUsed: ["get_node_details"],
        };
      }
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

    if (toolName === "find_visible_nodes") {
      const rows = Array.isArray(result.matches) ? result.matches : [];
      if (!rows.length) {
        return {
          answer: "No visible nodes matched that query.",
          note: "Repeated provider tool request was stopped after Unicorn had already returned the current visible-node matches.",
          toolsUsed: ["find_visible_nodes"],
        };
      }
      const preview = rows.map((row) => `${row.name} (${row.taxid})`).join(", ");
      return {
        answer: `${rows.length} visible node${rows.length === 1 ? "" : "s"} matched the query: ${preview}.`,
        note: "Repeated provider tool request was stopped after Unicorn had already returned grounded visible-node matches.",
        toolsUsed: ["find_visible_nodes"],
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
    const providerMeta = getProviderAdapter().getCurrentProviderMeta();
    const context = buildAgentContext();
    setStatus(`${providerMeta?.label || "Agent"} is drafting a reply from Unicorn backend state...`);
    getProviderAdapter().runTurn({
      prompt,
      context,
      registry: getAgentRegistry(),
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
              : state.agent.runtimeProvider === "local_openai_compat"
                ? "Backend-side local OpenAI-compatible transport is active. Check backend reachability, the local base URL, and provider response details in the client log."
                : getConfiguredAgentProvider() === "local_openai_compat"
                  ? "Local OpenAI-compatible target is selected, but the runtime adapter did not switch out of mock mode. Refresh the page once, then check the provider-state line for runtime adapter and base URL."
                : getConfiguredAgentProvider() === "google"
                  ? "Mock runtime is still active. Enter a Google API key to switch the runtime adapter to backend-side Gemini transport."
                  : "Mock runtime is still active. Enter an OpenAI API key to switch the runtime adapter to backend-side OpenAI transport.",
        });
        setStatus(`Agent provider reply failed: ${error.message || error}`);
      });
  }

  function setStatus(message) {
    if (typeof globalObject.setStatus === "function") {
      globalObject.setStatus(message);
    }
  }

  function addClientLog(level, stage, message, detail = "") {
    if (typeof globalObject.addClientLog === "function") {
      globalObject.addClientLog(level, stage, message, detail);
    }
  }

  function errorToDetail(error) {
    if (typeof globalObject.errorToDetail === "function") {
      return globalObject.errorToDetail(error);
    }
    return String(error?.message || error || "");
  }

  function getAgentRegistry() {
    const registry = globalObject.unicornAgentRegistry;
    if (!registry) {
      throw new Error("Unicorn agent registry is not available.");
    }
    return registry;
  }

  function getProviderAdapter() {
    const providerAdapter = globalObject.unicornAgentProviderAdapter;
    if (!providerAdapter) {
      throw new Error("Unicorn agent provider adapter is not available.");
    }
    return providerAdapter;
  }

  namespace.agent = {
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
    summarizeCurrentAgentReport,
  };

  Object.assign(globalObject, namespace.agent);
})(window);
