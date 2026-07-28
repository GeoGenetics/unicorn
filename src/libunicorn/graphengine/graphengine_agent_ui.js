"use strict";

(function initUnicornGraphEngineAgentUi(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const state = namespace.state;
  const els = namespace.els;
  const core = namespace.core;
  const backend = namespace.backend;
  const ui = namespace.ui;

  if (!state || !els || !core || !backend || !ui) {
    throw new Error("Unicorn Agent UI expected state, DOM, core, backend, and UI modules to load first.");
  }

  const BACKEND_BASE_URL = "http://localhost:8000";
  const PROVIDER_API_KEY_HEADER = "X-Unicorn-Provider-API-Key";
  const MAX_CONVERSATION_MESSAGES = 8;
  const PROVIDERS = {
    openai: {
      label: "OpenAI Responses",
      defaultModel: "gpt-4.1",
      defaultBaseUrl: "",
      models: ["gpt-4.1", "gpt-4.1-mini", "gpt-5-mini"],
      apiKeyRequired: true,
    },
    google: {
      label: "Google Gemini",
      defaultModel: "gemini-2.5-flash",
      defaultBaseUrl: "",
      models: [
        "gemini-2.5-flash",
        "gemini-2.5-flash-lite",
        "gemini-2.5-pro",
      ],
      apiKeyRequired: true,
    },
    local_openai_compat: {
      label: "Local OpenAI-compatible",
      defaultModel: "deepseek-ai/DeepSeek-R1-Distill-Qwen-14B",
      defaultBaseUrl: "http://localhost:8000",
      models: ["deepseek-ai/DeepSeek-R1-Distill-Qwen-14B"],
      apiKeyRequired: false,
    },
  };

  const agentState = {
    initialized: false,
    pending: false,
    sessionId: createIdentifier("session"),
    conversation: [],
    entries: [],
    lastTurnId: null,
    lastTraceId: null,
  };

  function init() {
    if (agentState.initialized) return;
    resolveAgentDomAdditions();

    const storedProvider = localStorage.getItem("unicorn.agent.provider");
    els.agentProviderSelect.value = PROVIDERS[storedProvider]
      ? storedProvider
      : els.agentProviderSelect.value;
    setConfigControlsDisabled(false);
    restoreProviderConfiguration();

    els.agentProviderSelect.addEventListener("change", handleProviderChange);
    els.agentModelSelect.addEventListener("input", handleModelInput);
    els.agentApiKey.addEventListener("input", syncAvailability);
    els.agentBaseUrl.addEventListener("input", handleBaseUrlInput);
    els.agentPrompt.addEventListener("input", syncAvailability);
    els.agentPrompt.addEventListener("keydown", handlePromptKeydown);
    els.agentSendBtn.addEventListener("click", sendPrompt);
    els.agentClearBtn.addEventListener("click", clearTranscript);
    els.agentTraceOpenBtn?.addEventListener("click", openLastTrace);
    els.agentTraceDownloadBtn?.addEventListener("click", downloadLastTrace);

    els.agentPrompt.disabled = false;
    els.agentPrompt.placeholder = "Ask about the current backend-authoritative graph state";
    renderTranscript();
    syncProviderState();
    agentState.initialized = true;
    syncAvailability();
  }

  function resolveAgentDomAdditions() {
    els.agentModelOptions = els.agentModelOptions
      || document.getElementById("agentModelOptions");
    els.agentTraceOpenBtn = els.agentTraceOpenBtn
      || document.getElementById("agentTraceOpenBtn");
    els.agentTraceDownloadBtn = els.agentTraceDownloadBtn
      || document.getElementById("agentTraceDownloadBtn");
  }

  function setConfigControlsDisabled(disabled) {
    els.agentProviderSelect.disabled = disabled;
    els.agentModelSelect.disabled = disabled;
    els.agentApiKey.disabled = disabled;
    els.agentBaseUrl.disabled = disabled;
  }

  function currentProviderName() {
    const provider = String(els.agentProviderSelect.value || "");
    return PROVIDERS[provider] ? provider : "openai";
  }

  function providerStorageKey(field, provider = currentProviderName()) {
    return `unicorn.agent.${field}.${provider}`;
  }

  function restoreProviderConfiguration() {
    const provider = currentProviderName();
    const config = PROVIDERS[provider];
    const storedModel = localStorage.getItem(providerStorageKey("model", provider));
    const storedBaseUrl = localStorage.getItem(providerStorageKey("baseUrl", provider));
    els.agentModelSelect.value = storedModel || config.defaultModel;
    els.agentBaseUrl.value = storedBaseUrl == null
      ? config.defaultBaseUrl
      : storedBaseUrl;
    els.agentApiKey.value = "";
    renderModelSuggestions(config.models);
  }

  function renderModelSuggestions(models) {
    if (!els.agentModelOptions) return;
    els.agentModelOptions.replaceChildren(
      ...models.map((model) => {
        const option = document.createElement("option");
        option.value = model;
        return option;
      }),
    );
  }

  function handleProviderChange() {
    localStorage.setItem("unicorn.agent.provider", currentProviderName());
    restoreProviderConfiguration();
    syncProviderState();
    syncAvailability();
  }

  function handleModelInput() {
    localStorage.setItem(
      providerStorageKey("model"),
      String(els.agentModelSelect.value || "").trim(),
    );
    syncAvailability();
  }

  function handleBaseUrlInput() {
    localStorage.setItem(
      providerStorageKey("baseUrl"),
      String(els.agentBaseUrl.value || "").trim(),
    );
    syncAvailability();
  }

  function handlePromptKeydown(event) {
    if (event.key !== "Enter" || !(event.ctrlKey || event.metaKey)) return;
    event.preventDefault();
    if (!els.agentSendBtn.disabled) sendPrompt();
  }

  function syncProviderState() {
    const config = PROVIDERS[currentProviderName()];
    const credentialNote = config.apiKeyRequired
      ? "An API key is required and is sent only in the transport header."
      : "An API key is optional for this target.";
    els.agentProviderState.textContent =
      `${config.label}. ${credentialNote} Provider calls execute through the Graphengine backend.`;
  }

  function syncAvailability() {
    if (!agentState.initialized) return;
    const scope = collectGraphScope({ validate: false });
    const prompt = String(els.agentPrompt.value || "").trim();
    const model = String(els.agentModelSelect.value || "").trim();
    const provider = PROVIDERS[currentProviderName()];
    const hasRequiredKey = !provider.apiKeyRequired
      || Boolean(String(els.agentApiKey.value || "").trim());
    const ready = Boolean(
      state.remote.connected
      && scope.datasets.length
      && scope.nodes_file
      && scope.names_file
      && prompt
      && model
      && hasRequiredKey
      && !agentState.pending
    );

    els.agentSendBtn.disabled = !ready;
    els.agentPrompt.disabled = agentState.pending;
    setConfigControlsDisabled(agentState.pending);
    els.agentClearBtn.disabled = agentState.pending || !agentState.entries.length;
    if (els.agentTraceOpenBtn) {
      els.agentTraceOpenBtn.disabled = !agentState.lastTraceId;
    }
    if (els.agentTraceDownloadBtn) {
      els.agentTraceDownloadBtn.disabled = !agentState.lastTraceId;
    }

    if (agentState.pending) {
      els.agentMeta.textContent = "Agent turn in progress";
    } else if (agentState.lastTurnId) {
      els.agentMeta.textContent = `Last turn: ${agentState.lastTurnId}`;
    } else if (!state.remote.connected) {
      els.agentMeta.textContent = "Connect to backend to begin";
    } else if (!scope.datasets.length) {
      els.agentMeta.textContent = "Select at least one dataset";
    } else if (!scope.nodes_file || !scope.names_file) {
      els.agentMeta.textContent = "Backend taxonomy is incomplete";
    } else {
      els.agentMeta.textContent = "Backend Agent ready";
    }
  }

  function collectGraphScope({ validate = true } = {}) {
    const requestContext = backend.getActiveBackendRequestContext?.() || {};
    const datasets = backend.getSelectedRemoteDatasets?.() || [];
    const countViewMode = ui.getCountViewMode?.() || "subtree";
    const scope = {
      datasets: datasets.map((filename) => String(filename)),
      nodes_file: String(
        requestContext.nodes_file
        || state.remote.backendNodesFile
        || "",
      ),
      names_file: String(
        requestContext.names_file
        || state.remote.backendNamesFile
        || "",
      ),
      min_reads: Math.max(
        0,
        Math.round(Number(ui.getMinReadsValue?.() || 0)),
      ),
      count_mode: countViewMode === "direct" ? "direct" : "subtree",
      selected_taxids: Array.from(state.selected)
        .map((taxid) => Number(taxid))
        .filter((taxid) => Number.isInteger(taxid) && taxid > 0)
        .sort((left, right) => left - right),
      focused_taxid: Number.isInteger(Number(state.focusTaxid))
        && Number(state.focusTaxid) > 0
        ? Number(state.focusTaxid)
        : null,
    };

    if (validate) {
      if (!state.remote.connected) {
        throw new Error("Connect to the Graphengine backend before sending an Agent prompt.");
      }
      if (!scope.datasets.length) {
        throw new Error("Select at least one backend dataset before sending an Agent prompt.");
      }
      if (!scope.nodes_file || !scope.names_file) {
        throw new Error("Load both nodes.dmp and names.dmp before sending an Agent prompt.");
      }
    }
    return scope;
  }

  function buildTurnRequest(prompt) {
    const model = String(els.agentModelSelect.value || "").trim();
    if (!model) throw new Error("Choose or enter a provider model first.");
    return {
      schema_version: "unicorn_agent_turn_v1",
      turn_id: createIdentifier("turn"),
      session_id: agentState.sessionId,
      prompt,
      provider: {
        name: currentProviderName(),
        model,
        base_url: String(els.agentBaseUrl.value || "").trim(),
      },
      graph_scope: collectGraphScope(),
      conversation: agentState.conversation
        .slice(-MAX_CONVERSATION_MESSAGES)
        .map((entry) => ({
          role: entry.role,
          content: entry.content,
        })),
    };
  }

  async function sendPrompt() {
    if (agentState.pending) return;
    const prompt = String(els.agentPrompt.value || "").trim();
    if (!prompt) return;

    let request;
    try {
      request = buildTurnRequest(prompt);
    } catch (error) {
      showClientError(error);
      return;
    }

    appendTranscriptEntry({
      role: "user",
      message: prompt,
      turnId: request.turn_id,
    });
    agentState.pending = true;
    agentState.lastTurnId = request.turn_id;
    els.agentPrompt.value = "";
    syncAvailability();
    ui.addClientLog(
      "info",
      "agent",
      `Sending Agent turn ${request.turn_id}.`,
      `${request.provider.name} · ${request.provider.model}`,
    );

    try {
      const headers = {
        "Content-Type": "application/json",
      };
      const apiKey = String(els.agentApiKey.value || "").trim();
      if (apiKey) headers[PROVIDER_API_KEY_HEADER] = apiKey;
      const response = await fetch(`${BACKEND_BASE_URL}/agent/turn`, {
        method: "POST",
        headers,
        body: JSON.stringify(request),
      });
      const payload = await readJsonResponse(response);
      if (!response.ok) {
        throw new Error(formatBackendError(payload, response.status));
      }
      handleTurnResult(request, payload);
    } catch (error) {
      appendTranscriptEntry({
        role: "error",
        message: error.message || String(error),
        turnId: request.turn_id,
      });
      ui.addClientLog(
        "error",
        "agent",
        `Agent turn ${request.turn_id} failed.`,
        core.errorToDetail(error),
      );
      globalObject.setStatus(`Agent turn failed: ${error.message || error}`);
    } finally {
      agentState.pending = false;
      syncAvailability();
    }
  }

  function handleTurnResult(request, result) {
    if (
      !result
      || result.schema_version !== "unicorn_agent_result_v1"
      || result.turn_id !== request.turn_id
    ) {
      throw new Error("Backend returned an Agent response that does not match the current turn.");
    }

    agentState.lastTraceId = String(result.trace_id || "") || null;
    const tools = Array.isArray(result.tools_used) ? result.tools_used : [];
    if (result.status === "completed" && typeof result.answer === "string") {
      appendTranscriptEntry({
        role: "assistant",
        message: result.answer,
        tools,
        turnId: result.turn_id,
        traceId: result.trace_id,
      });
      agentState.conversation.push(
        { role: "user", content: request.prompt },
        { role: "assistant", content: result.answer },
      );
      ui.addClientLog(
        "success",
        "agent",
        `Agent turn ${result.turn_id} completed.`,
        `${tools.length.toLocaleString()} tool call${tools.length === 1 ? "" : "s"} · ${result.trace_id}`,
      );
      globalObject.setStatus(`Agent turn completed. Trace: ${result.trace_id}`);
      return;
    }

    const error = result.error && typeof result.error === "object"
      ? result.error
      : {};
    const message = String(
      error.message
      || `Agent turn stopped with status ${result.status || "unknown"}.`,
    );
    appendTranscriptEntry({
      role: "error",
      message,
      tools,
      turnId: result.turn_id,
      traceId: result.trace_id,
      note: error.code ? `Error code: ${error.code}` : "",
    });
    ui.addClientLog(
      "error",
      "agent",
      `Agent turn ${result.turn_id} ended with ${result.status || "failure"}.`,
      `${error.code || "unknown_error"} · ${result.trace_id}`,
    );
    globalObject.setStatus(`Agent turn failed: ${message}`);
  }

  function appendTranscriptEntry(entry) {
    agentState.entries.push({
      timestamp: new Date().toISOString(),
      tools: [],
      note: "",
      ...entry,
    });
    renderTranscript();
  }

  function renderTranscript() {
    if (!agentState.entries.length) {
      els.agentTranscript.innerHTML = `
        <div class="agent-empty">
          Ask a question about the current graph. Provider transport, tools, and iteration state remain backend-owned.
        </div>
      `;
      return;
    }

    els.agentTranscript.innerHTML = agentState.entries.map((entry) => {
      const roleLabel = entry.role === "error" ? "Agent error" : entry.role;
      const tools = Array.isArray(entry.tools) ? entry.tools : [];
      const traceNote = entry.traceId
        ? `Trace ${entry.traceId}`
        : entry.turnId
          ? `Turn ${entry.turnId}`
          : "";
      return `
        <article class="agent-entry role-${core.escapeHtml(entry.role)}">
          <div class="agent-entry-head">
            <span class="agent-entry-role">${core.escapeHtml(roleLabel)}</span>
            <span class="agent-entry-time">${core.escapeHtml(core.formatLogTime(entry.timestamp))}</span>
          </div>
          <div class="agent-entry-message">${core.escapeHtml(entry.message || "")}</div>
          ${tools.length ? `
            <div class="agent-entry-tools">
              ${tools.map((tool) => `
                <span class="agent-tool-badge${tool.ok ? "" : " is-error"}">
                  ${core.escapeHtml(tool.tool_id || "unknown")} · ${tool.ok ? "ok" : "failed"}
                </span>
              `).join("")}
            </div>
          ` : ""}
          ${traceNote || entry.note ? `
            <div class="agent-entry-note">${core.escapeHtml([traceNote, entry.note].filter(Boolean).join(" · "))}</div>
          ` : ""}
        </article>
      `;
    }).join("");
    els.agentTranscript.scrollTop = els.agentTranscript.scrollHeight;
  }

  function clearTranscript() {
    if (agentState.pending) return;
    agentState.sessionId = createIdentifier("session");
    agentState.conversation = [];
    agentState.entries = [];
    agentState.lastTurnId = null;
    agentState.lastTraceId = null;
    renderTranscript();
    syncAvailability();
    globalObject.setStatus("Cleared Agent transcript and started a new Agent session.");
  }

  function openLastTrace() {
    if (!agentState.lastTraceId) return;
    const link = document.createElement("a");
    link.href = traceUrl(agentState.lastTraceId);
    link.target = "_blank";
    link.rel = "noopener";
    document.body.appendChild(link);
    link.click();
    link.remove();
  }

  function downloadLastTrace() {
    if (!agentState.lastTraceId) return;
    const link = document.createElement("a");
    link.href = traceUrl(agentState.lastTraceId, true);
    link.download = `${agentState.lastTraceId}.jsonl`;
    document.body.appendChild(link);
    link.click();
    link.remove();
  }

  function traceUrl(traceId, download = false) {
    const suffix = download ? "?download=true" : "";
    return `${BACKEND_BASE_URL}/agent/traces/${encodeURIComponent(traceId)}${suffix}`;
  }

  function createIdentifier(prefix) {
    const timestamp = Date.now().toString(36);
    const random = typeof globalObject.crypto?.randomUUID === "function"
      ? globalObject.crypto.randomUUID().replaceAll("-", "").slice(0, 12)
      : Math.random().toString(36).slice(2, 14);
    return `${prefix}_${timestamp}_${random}`;
  }

  async function readJsonResponse(response) {
    const text = await response.text();
    if (!text) return {};
    try {
      return JSON.parse(text);
    } catch (_error) {
      throw new Error(`Backend returned non-JSON Agent output with HTTP ${response.status}.`);
    }
  }

  function formatBackendError(payload, status) {
    const detail = payload && typeof payload.detail === "object"
      ? payload.detail
      : payload;
    const message = detail && typeof detail.message === "string"
      ? detail.message
      : `Backend Agent request failed with HTTP ${status}.`;
    const code = detail && typeof detail.code === "string"
      ? ` (${detail.code})`
      : "";
    return `${message}${code}`;
  }

  function showClientError(error) {
    const message = error.message || String(error);
    appendTranscriptEntry({
      role: "error",
      message,
    });
    ui.addClientLog("error", "agent", "Agent request was not sent.", message);
    globalObject.setStatus(message);
  }

  namespace.agentUi = {
    init,
    syncAvailability,
    collectGraphScope,
    buildTurnRequest,
  };
})(window);
