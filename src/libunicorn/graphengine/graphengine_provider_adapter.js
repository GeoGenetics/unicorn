"use strict";

(function initUnicornAgentProviderModule(globalObject) {
  const OPENAI_INTERNAL_RESPONSE_SCHEMA_NAME = "unicorn_provider_turn_response";

  function createProviderAdapter(options = {}) {
    const getRuntimeConfig = typeof options.getRuntimeConfig === "function"
      ? options.getRuntimeConfig
      : () => ({
        runtime_provider: options.runtimeProviderName === "openai" || options.runtimeProviderName === "google" || options.runtimeProviderName === "local_openai_compat"
          ? options.runtimeProviderName
          : "mock",
        transport_mode: "browser",
        configured_provider: "openai",
        configured_model: "gpt-5",
        api_key: "",
        base_url: "",
      });
    const executeTurn = typeof options.executeTurn === "function"
      ? options.executeTurn
      : null;
    const extractTaxidFromPrompt = typeof options.extractTaxidFromPrompt === "function"
      ? options.extractTaxidFromPrompt
      : defaultExtractTaxidFromPrompt;
    const providers = {
      mock: createMockProvider({ extractTaxidFromPrompt }),
      openai: createOpenAIProvider({ getRuntimeConfig }),
      google: createGoogleProvider({ getRuntimeConfig }),
      local_openai_compat: createLocalOpenAICompatProvider({ getRuntimeConfig }),
    };

    function getRuntimeProviderName() {
      const runtimeProvider = String(getRuntimeConfig().runtime_provider || "mock");
      return providers[runtimeProvider] ? runtimeProvider : "mock";
    }

    return {
      listProviders() {
        return Object.keys(providers);
      },
      getCurrentProviderName() {
        return getRuntimeProviderName();
      },
      getCurrentProviderImplementation() {
        return providers[getRuntimeProviderName()] || null;
      },
      getCurrentProviderMeta() {
        const provider = providers[getRuntimeProviderName()];
        if (!provider) return null;
        return {
          name: provider.name,
          label: provider.label,
          mode: provider.mode,
          client_side: provider.clientSide,
        };
      },
      async runTurn(input) {
        if (typeof executeTurn !== "function") {
          throw new Error("Provider adapter is missing its Unicorn turn executor.");
        }
        return executeTurn(input);
      },
    };
  }

  function createMockProvider(options = {}) {
    const extractTaxidFromPrompt = typeof options.extractTaxidFromPrompt === "function"
      ? options.extractTaxidFromPrompt
      : defaultExtractTaxidFromPrompt;
    return {
      name: "mock",
      label: "Mock",
      mode: "mock",
      clientSide: true,
      async runRequest(payload) {
        return runMockProviderRequest(payload, { extractTaxidFromPrompt });
      },
    };
  }

  function createOpenAIProvider(options = {}) {
    const getRuntimeConfig = typeof options.getRuntimeConfig === "function"
      ? options.getRuntimeConfig
      : () => ({
        configured_model: "gpt-5",
        api_key: "",
        base_url: "",
      });
    return {
      name: "openai",
      label: "OpenAI",
      mode: "responses-json",
      clientSide: false,
      buildRequestBody(payload) {
        return buildOpenAIResponsesRequest(payload, getRuntimeConfig());
      },
      parseResponse(apiResponse) {
        return parseOpenAIResponsesApiResponse(apiResponse);
      },
      async runRequest(payload) {
        const runtimeConfig = getRuntimeConfig();
        const endpoint = resolveBackendProviderTurnEndpoint(runtimeConfig.backend_base_url);
        let response;
        try {
          response = await fetch(endpoint, {
            method: "POST",
            headers: {
              "Content-Type": "application/json",
            },
            body: JSON.stringify({
              provider_payload: payload,
              runtime_config: runtimeConfig,
            }),
          });
        } catch (error) {
          throw new Error(`Backend provider-turn request failed before reaching the backend: ${error.message || error}`);
        }
        let responseJson = null;
        try {
          responseJson = await response.json();
        } catch (error) {
          throw new Error(`Backend provider-turn endpoint returned a non-JSON response (HTTP ${response.status}): ${error.message || error}`);
        }
        if (!response.ok) {
          const detail = String(responseJson?.detail?.message || responseJson?.error?.message || `HTTP ${response.status}`);
          throw new Error(`Backend provider-turn request failed: ${detail}`);
        }
        return normalizeInternalProviderResponse(responseJson);
      },
    };
  }

  function createGoogleProvider(options = {}) {
    const getRuntimeConfig = typeof options.getRuntimeConfig === "function"
      ? options.getRuntimeConfig
      : () => ({
        configured_model: "gemini-3.5-flash",
        api_key: "",
        base_url: "",
      });
    return {
      name: "google",
      label: "Google",
      mode: "interactions-json",
      clientSide: false,
      async runRequest(payload) {
        const runtimeConfig = getRuntimeConfig();
        const endpoint = resolveBackendProviderTurnEndpoint(runtimeConfig.backend_base_url);
        let response;
        try {
          response = await fetch(endpoint, {
            method: "POST",
            headers: {
              "Content-Type": "application/json",
            },
            body: JSON.stringify({
              provider_payload: payload,
              runtime_config: runtimeConfig,
            }),
          });
        } catch (error) {
          throw new Error(`Backend provider-turn request failed before reaching the backend: ${error.message || error}`);
        }
        let responseJson = null;
        try {
          responseJson = await response.json();
        } catch (error) {
          throw new Error(`Backend provider-turn endpoint returned a non-JSON response (HTTP ${response.status}): ${error.message || error}`);
        }
        if (!response.ok) {
          const detail = String(responseJson?.detail?.message || responseJson?.error?.message || `HTTP ${response.status}`);
          throw new Error(`Backend provider-turn request failed: ${detail}`);
        }
        return normalizeInternalProviderResponse(responseJson);
      },
    };
  }

  function createLocalOpenAICompatProvider(options = {}) {
    const getRuntimeConfig = typeof options.getRuntimeConfig === "function"
      ? options.getRuntimeConfig
      : () => ({
        configured_model: "deepseek-ai/DeepSeek-R1-Distill-Qwen-14B",
        api_key: "",
        base_url: "",
      });
    return {
      name: "local_openai_compat",
      label: "Local (OpenAI-compatible)",
      mode: "chat-completions-json",
      clientSide: false,
      async runRequest(payload) {
        const runtimeConfig = getRuntimeConfig();
        const endpoint = resolveBackendProviderTurnEndpoint(runtimeConfig.backend_base_url);
        let response;
        try {
          response = await fetch(endpoint, {
            method: "POST",
            headers: {
              "Content-Type": "application/json",
            },
            body: JSON.stringify({
              provider_payload: payload,
              runtime_config: runtimeConfig,
            }),
          });
        } catch (error) {
          throw new Error(`Backend provider-turn request failed before reaching the backend: ${error.message || error}`);
        }
        let responseJson = null;
        try {
          responseJson = await response.json();
        } catch (error) {
          throw new Error(`Backend provider-turn endpoint returned a non-JSON response (HTTP ${response.status}): ${error.message || error}`);
        }
        if (!response.ok) {
          const detail = String(responseJson?.detail?.message || responseJson?.error?.message || `HTTP ${response.status}`);
          throw new Error(`Backend provider-turn request failed: ${detail}`);
        }
        return normalizeInternalProviderResponse(responseJson);
      },
    };
  }

  function resolveBackendProviderTurnEndpoint(baseUrl) {
    const trimmed = String(baseUrl || "").trim();
    if (!trimmed) {
      return "http://localhost:8000/agent/provider-turn";
    }
    const withoutTrailingSlash = trimmed.replace(/\/+$/, "");
    if (withoutTrailingSlash.endsWith("/agent/provider-turn")) {
      return withoutTrailingSlash;
    }
    return `${withoutTrailingSlash}/agent/provider-turn`;
  }

  function buildOpenAIResponsesRequest(providerPayload, runtimeConfig = {}) {
    const safePayload = providerPayload && typeof providerPayload === "object" ? providerPayload : {};
    const model = String(runtimeConfig.configured_model || safePayload.provider?.model || "gpt-5");
    const input = [];
    const developerContext = buildOpenAIDeveloperContextMessage(safePayload, runtimeConfig);
    if (developerContext) {
      input.push(developerContext);
    }
    const conversation = Array.isArray(safePayload.conversation) ? safePayload.conversation : [];
    conversation.forEach((message) => {
      const normalizedMessage = toOpenAIInputMessage(message.role, message.content);
      if (normalizedMessage) {
        input.push(normalizedMessage);
      }
    });
    const userPrompt = String(safePayload.user_prompt || "");
    if (userPrompt) {
      input.push(toOpenAIInputMessage("user", userPrompt));
    }
    return {
      model,
      instructions: String(safePayload.system_prompt || ""),
      input,
      text: {
        format: buildOpenAIInternalResponseFormat(),
      },
    };
  }

  function buildOpenAIDeveloperContextMessage(providerPayload, runtimeConfig = {}) {
    const developerEnvelope = {
      graph_context: providerPayload.graph_context && typeof providerPayload.graph_context === "object"
        ? providerPayload.graph_context
        : null,
      tools: Array.isArray(providerPayload.tools) ? providerPayload.tools : [],
      tool_results: Array.isArray(providerPayload.tool_results) ? providerPayload.tool_results : [],
      turn_config: providerPayload.turn_config && typeof providerPayload.turn_config === "object"
        ? providerPayload.turn_config
        : {},
      provider_contract: {
        reply_mode: "json_only",
        orchestrator: "unicorn",
        native_provider_tool_calling: false,
      },
      runtime_config: {
        runtime_provider: runtimeConfig.runtime_provider || "openai",
        transport_mode: runtimeConfig.transport_mode || "browser",
        base_url: runtimeConfig.base_url || "",
      },
    };
    return toOpenAIInputMessage(
      "developer",
      [
        "Unicorn provider turn context follows as JSON.",
        "Use only the advertised Unicorn tools.",
        "Return exactly one JSON object that matches the requested schema.",
        JSON.stringify(developerEnvelope, null, 2),
      ].join("\n\n"),
    );
  }

  function buildOpenAIInternalResponseFormat() {
    return {
      type: "json_schema",
      name: OPENAI_INTERNAL_RESPONSE_SCHEMA_NAME,
      strict: true,
      schema: {
        type: "object",
        properties: {
          type: {
            type: "string",
            enum: ["assistant_message", "tool_call", "final_answer", "error"],
          },
          content: {
            type: ["string", "null"],
          },
          tool_name: {
            type: ["string", "null"],
          },
          args: {
            type: ["object", "null"],
            properties: {
              taxid: {
                type: ["integer", "null"],
              },
              scope: {
                type: ["string", "null"],
                enum: ["root", "node", null],
              },
              sort: {
                type: ["string", "null"],
                enum: ["direct", "subtree", null],
              },
              limit: {
                type: ["integer", "null"],
              },
            },
            required: ["taxid", "scope", "sort", "limit"],
            additionalProperties: false,
          },
          tool_summary: {
            type: ["array", "null"],
            items: { type: "string" },
          },
          notes: {
            type: ["string", "null"],
          },
          code: {
            type: ["string", "null"],
          },
          message: {
            type: ["string", "null"],
          },
        },
        required: ["type", "content", "tool_name", "args", "tool_summary", "notes", "code", "message"],
        additionalProperties: false,
      },
    };
  }

  function toOpenAIInputMessage(role, content) {
    const normalizedRole = normalizeOpenAIInputRole(role);
    const contentType = normalizedRole === "assistant" ? "output_text" : "input_text";
    const text = String(content || "").trim();
    if (!text) return null;
    return {
      role: normalizedRole,
      content: [
        {
          type: contentType,
          text,
        },
      ],
    };
  }

  function normalizeOpenAIInputRole(role) {
    if (role === "developer" || role === "assistant" || role === "system") {
      return role;
    }
    return "user";
  }

  function parseOpenAIResponsesApiResponse(apiResponse) {
    const responseObject = apiResponse && typeof apiResponse === "object" ? apiResponse : null;
    if (!responseObject) {
      throw new Error("OpenAI Responses API returned an invalid payload.");
    }
    if (responseObject.error?.message) {
      throw new Error(String(responseObject.error.message));
    }
    const jsonText = extractOpenAIOutputText(responseObject);
    if (!jsonText) {
      throw new Error("OpenAI Responses API returned no assistant JSON output.");
    }
    let parsed;
    try {
      parsed = JSON.parse(jsonText);
    } catch (error) {
      throw new Error(`OpenAI Responses API returned non-JSON output: ${error.message || error}`);
    }
    return normalizeInternalProviderResponse(parsed);
  }

  function extractOpenAIOutputText(apiResponse) {
    const output = Array.isArray(apiResponse.output) ? apiResponse.output : [];
    for (const item of output) {
      if (item?.type !== "message") continue;
      const content = Array.isArray(item.content) ? item.content : [];
      const textParts = content
        .filter((entry) => entry?.type === "output_text" && typeof entry.text === "string")
        .map((entry) => entry.text);
      if (textParts.length) {
        return textParts.join("\n");
      }
    }
    return "";
  }

  function normalizeInternalProviderResponse(response) {
    if (!response || typeof response !== "object") {
      throw new Error("Provider returned an invalid response payload.");
    }
    const type = String(response.type || "");
    if (!type) {
      throw new Error("Provider response is missing a type field.");
    }
    if (type === "assistant_message") {
      return {
        type,
        content: String(response.content || ""),
      };
    }
    if (type === "tool_call") {
      return {
        type,
        tool_name: String(response.tool_name || ""),
        args: response.args && typeof response.args === "object" ? response.args : {},
      };
    }
    if (type === "final_answer") {
      return {
        type,
        content: String(response.content || ""),
        tool_summary: Array.isArray(response.tool_summary)
          ? response.tool_summary.map((toolName) => String(toolName))
          : [],
        notes: response.notes == null ? "" : String(response.notes),
      };
    }
    if (type === "error") {
      return {
        type,
        code: response.code == null ? "" : String(response.code),
        message: String(response.message || "Provider returned an error payload."),
      };
    }
    throw new Error(`Provider returned unsupported response type: ${type}`);
  }

  async function runMockProviderRequest(payload, options = {}) {
    const extractTaxidFromPrompt = typeof options.extractTaxidFromPrompt === "function"
      ? options.extractTaxidFromPrompt
      : defaultExtractTaxidFromPrompt;
    const prompt = String(payload?.user_prompt || "");
    const context = payload?.graph_context && typeof payload.graph_context === "object"
      ? payload.graph_context
      : {};
    const toolResults = Array.isArray(payload?.tool_results) ? payload.tool_results : [];
    const lower = prompt.trim().toLowerCase();

    if (!context.backend?.connected) {
      return normalizeInternalProviderResponse({
        type: "final_answer",
        content: "No Unicorn backend connection is active yet. Start the graphengine backend, connect to it, render a tree, then ask me about the current graph state.",
        tool_summary: ["get_graph_context"],
        notes: "Mock mode only. Agent grounding now assumes a backend-backed Unicorn session.",
      });
    }

    if (!context.tree?.loaded) {
      return normalizeInternalProviderResponse({
        type: "final_answer",
        content: "No active backend-backed Unicorn tree is loaded yet. Render a tree first, then ask me about the current graph state.",
        tool_summary: ["get_graph_context"],
        notes: "Mock mode only. Agent grounding now assumes a backend-backed Unicorn session.",
      });
    }

    if (!toolResults.length) {
      if (lower.includes("selected")) {
        return normalizeInternalProviderResponse({
          type: "tool_call",
          tool_name: "get_selected_nodes",
          args: {},
        });
      }
      if (lower.includes("dataset") || lower.includes("sample")) {
        return normalizeInternalProviderResponse({
          type: "tool_call",
          tool_name: "list_selected_datasets",
          args: {},
        });
      }
      if (lower.includes("damage") || lower.includes("top") || lower.includes("table") || lower.includes("rank")) {
        return normalizeInternalProviderResponse({
          type: "tool_call",
          tool_name: "get_table_view",
          args: {
            scope: "root",
            sort: "direct",
            limit: 5,
          },
        });
      }
      const taxid = extractTaxidFromPrompt(prompt);
      if (taxid != null) {
        return normalizeInternalProviderResponse({
          type: "tool_call",
          tool_name: "get_node_details",
          args: { taxid },
        });
      }
      return normalizeInternalProviderResponse({
        type: "final_answer",
        content: `The current Unicorn session is using ${String(context.mode || "backend")} mode with ${Number(context.datasets?.count || 0).toLocaleString()} active dataset${Number(context.datasets?.count || 0) === 1 ? "" : "s"} and ${Number(context.tree?.visible_node_count || 0).toLocaleString()} visible nodes.`,
        tool_summary: ["get_graph_context"],
        notes: "Mock mode only. This response is grounded in the current backend-backed graph context.",
      });
    }

    const latest = toolResults[toolResults.length - 1];
    if (latest.tool_name === "get_selected_nodes") {
      const selected = latest.result || {};
      const rows = Array.isArray(selected.selected) ? selected.selected : [];
      if (!rows.length) {
        return normalizeInternalProviderResponse({
          type: "final_answer",
          content: "No nodes are currently selected in the graph.",
          tool_summary: ["get_graph_context", "get_selected_nodes"],
          notes: "Try selecting one or more taxa in the tree, then ask again.",
        });
      }
      const top = rows.slice(0, 3).map((node) => `${node.name} (${node.taxid})`).join(", ");
      return normalizeInternalProviderResponse({
        type: "final_answer",
        content: `${Number(selected.count || rows.length)} node${Number(selected.count || rows.length) === 1 ? "" : "s"} ${Number(selected.count || rows.length) === 1 ? "is" : "are"} currently selected. The current selection includes ${top}.`,
        tool_summary: ["get_graph_context", "get_selected_nodes"],
        notes: "Mock mode only. This summary comes from Unicorn's current selection state.",
      });
    }

    if (latest.tool_name === "list_selected_datasets") {
      const datasets = latest.result?.datasets || {};
      const names = Array.isArray(datasets.selected) ? datasets.selected.slice(0, 3).join(", ") : "";
      return normalizeInternalProviderResponse({
        type: "final_answer",
        content: `${Number(datasets.count || 0)} dataset${Number(datasets.count || 0) === 1 ? "" : "s"} ${Number(datasets.count || 0) === 1 ? "is" : "are"} active in the current ${latest.result?.mode || context.mode} session. Total reads: ${Number(datasets.total_reads || 0).toLocaleString()}. Direct taxa: ${Number(datasets.direct_taxa || 0).toLocaleString()}.`,
        tool_summary: ["get_graph_context", "list_selected_datasets"],
        notes: names ? `Active datasets: ${names}${Array.isArray(datasets.selected) && datasets.selected.length > 3 ? " ..." : ""}` : "No active datasets were reported.",
      });
    }

    if (latest.tool_name === "get_table_view") {
      const rows = Array.isArray(latest.result?.rows) ? latest.result.rows : [];
      if (!rows.length) {
        return normalizeInternalProviderResponse({
          type: "final_answer",
          content: "I could not find any ranked rows in the current table scope.",
          tool_summary: ["get_graph_context", "get_table_view"],
          notes: "Mock mode only. The table query returned no rows.",
        });
      }
      const first = rows[0];
      const preview = rows.slice(0, 3).map((row) => `${row.name} (${Number(row.direct || 0).toLocaleString()} direct)`).join(", ");
      return normalizeInternalProviderResponse({
        type: "final_answer",
        content: `From the current root table scope, ${first.name} is the strongest direct-read row with ${Number(first.direct || 0).toLocaleString()} direct reads and ${Number(first.subtree || 0).toLocaleString()} subtree reads.`,
        tool_summary: ["get_graph_context", "get_table_view"],
        notes: `Top rows preview: ${preview}`,
      });
    }

    if (latest.tool_name === "get_node_details") {
      const node = latest.result?.node || {};
      const taxid = Number(node.taxid || extractTaxidFromPrompt(prompt) || 0);
      return normalizeInternalProviderResponse({
        type: "final_answer",
        content: `${node.name || taxid} (${taxid}) is currently visible with ${Number(node.direct || 0).toLocaleString()} direct reads and ${Number(node.subtree || 0).toLocaleString()} subtree reads.`,
        tool_summary: ["get_graph_context", "get_node_details"],
        notes: `Rank: ${node.rank || "NA"}. Filtered child count: ${Number(node.child_count || 0).toLocaleString()}.`,
      });
    }

    return normalizeInternalProviderResponse({
      type: "error",
      code: "provider_response_invalid",
      message: "Mock provider could not interpret the latest tool result.",
    });
  }

  function defaultExtractTaxidFromPrompt(prompt) {
    const match = String(prompt || "").match(/\b(\d{1,12})\b/);
    return match ? Number(match[1]) : null;
  }

  globalObject.UnicornAgentProviderModule = {
    createProviderAdapter,
    normalizeInternalProviderResponse,
    buildOpenAIResponsesRequest,
    parseOpenAIResponsesApiResponse,
  };
})(window);
