"use strict";

(function initUnicornGraphEngineAgent(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const state = namespace.state;
  const els = namespace.els;
  const core = namespace.core;

  if (!state || !els || !core) {
    throw new Error("Unicorn graphengine agent expected state, DOM, and core modules to load first.");
  }

  const {
    AGENT_PROVIDER_MODELS,
    AGENT_SUPPORTED_PROVIDER_TARGETS,
    escapeHtml,
    formatLogTime,
  } = core;

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
    if (typeof globalObject.__buildAgentContextImpl === "function") {
      return globalObject.__buildAgentContextImpl();
    }
    throw new Error("Unicorn agent context builder is not wired yet.");
  }

  function createUnicornAgentRegistry() {
    if (typeof globalObject.__createUnicornAgentRegistryImpl === "function") {
      return globalObject.__createUnicornAgentRegistryImpl();
    }
    throw new Error("Unicorn agent registry is not wired yet.");
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
  };

  Object.assign(globalObject, namespace.agent);
})(window);
