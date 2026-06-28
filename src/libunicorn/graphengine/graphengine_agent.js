"use strict";

(function initUnicornGraphEngineAgent(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const state = namespace.state;
  const els = namespace.els;
  const core = namespace.core;
  const treeModel = namespace.treeModel;
  const backend = namespace.backend;
  const selection = namespace.selection;
  const ui = namespace.ui;

  if (!state || !els || !core || !treeModel || !backend || !selection || !ui) {
    throw new Error("Unicorn graphengine agent expected state, DOM, core, tree model, backend, selection, and UI modules to load first.");
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

  function normalizeToolTaxid(value) {
    const taxid = Number(value);
    return Number.isFinite(taxid) ? taxid : null;
  }

  function normalizeToolLimit(value, fallback) {
    const limit = Number(value);
    if (!Number.isFinite(limit) || limit <= 0) return fallback;
    return Math.max(1, Math.min(200, Math.round(limit)));
  }

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

  function handleAgentSend() {
    if (typeof globalObject.__handleAgentSendImpl === "function") {
      return globalObject.__handleAgentSendImpl();
    }
    throw new Error("Unicorn agent send handler is not wired yet.");
  }

  function setStatus(message) {
    if (typeof globalObject.setStatus === "function") {
      globalObject.setStatus(message);
    }
  }

  namespace.agent = {
    initAgentControls,
    clearAgentTranscript,
    pushAgentEntry,
    renderAgentTranscript,
    buildAgentContext,
    createUnicornAgentRegistry,
    handleAgentSend,
    syncAgentRuntimeProvider,
    getConfiguredAgentProvider,
    getAgentRuntimeConfig,
    summarizeCurrentAgentReport,
  };

  Object.assign(globalObject, namespace.agent);
})(window);
