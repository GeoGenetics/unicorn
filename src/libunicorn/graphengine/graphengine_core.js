"use strict";

(function initUnicornGraphEngineCore(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};

  if (namespace.core) {
    return;
  }

  const SOURCE_COLORS = [
    "#c85f43",
    "#255f75",
    "#e2a44e",
    "#5f8c6f",
    "#8a5a99",
    "#d17b2c",
    "#5d6cc1",
    "#b24d6d",
    "#4c9f9b",
    "#7f6a58",
  ];

  const RANK_DISPLAY_ORDER = [
    "superkingdom",
    "kingdom",
    "subkingdom",
    "superphylum",
    "phylum",
    "subphylum",
    "infraphylum",
    "superclass",
    "class",
    "subclass",
    "infraclass",
    "cohort",
    "superorder",
    "order",
    "suborder",
    "infraorder",
    "parvorder",
    "superfamily",
    "family",
    "subfamily",
    "tribe",
    "subtribe",
    "genus",
    "subgenus",
    "section",
    "series",
    "species group",
    "species subgroup",
    "species",
    "subspecies",
    "varietas",
    "variety",
    "subvariety",
    "forma",
    "strain",
    "isolate",
    "clade",
    "no rank",
  ];

  const AGENT_PROVIDER_MODELS = {
    openai: [
      { value: "gpt-5", label: "GPT-5" },
      { value: "gpt-5-mini", label: "GPT-5 mini" },
      { value: "gpt-4.1", label: "GPT-4.1" },
    ],
    google: [
      { value: "gemini-3.5-flash", label: "Gemini 3.5 Flash" },
      { value: "gemini-3.1-pro", label: "Gemini 3.1 Pro" },
      { value: "gemini-3-flash", label: "Gemini 3 Flash" },
      { value: "gemini-2.5-pro", label: "Gemini 2.5 Pro" },
      { value: "gemini-2.5-flash", label: "Gemini 2.5 Flash" },
      { value: "gemini-2.5-flash-lite", label: "Gemini 2.5 Flash-Lite" },
    ],
    local_openai_compat: [
      { value: "deepseek-ai/DeepSeek-R1-Distill-Qwen-14B", label: "DeepSeek-R1-Distill-Qwen-14B" },
      { value: "Qwen/Qwen2.5-14B-Instruct", label: "Qwen2.5-14B-Instruct" },
      { value: "Qwen/QwQ-32B", label: "QwQ-32B" },
    ],
  };

  const AGENT_SUPPORTED_PROVIDER_TARGETS = ["openai", "google", "local_openai_compat"];
  const AGENT_V1_READ_ONLY_TOOL_NAMES = [
    "get_graph_context",
    "list_selected_datasets",
    "get_selected_nodes",
    "get_node_details",
    "get_table_view",
  ];

  const AGENT_MAX_TOOL_ITERATIONS = 4;

  function formatBytes(bytes) {
    if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
    const units = ["B", "KB", "MB", "GB", "TB"];
    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit++;
    }
    const digits = value >= 100 || unit === 0 ? 0 : value >= 10 ? 1 : 2;
    return `${value.toFixed(digits)} ${units[unit]}`;
  }

  function svgEl(name, attrs = {}, text = "") {
    const el = document.createElementNS("http://www.w3.org/2000/svg", name);
    for (const [key, value] of Object.entries(attrs)) el.setAttribute(key, value);
    if (text) el.textContent = text;
    return el;
  }

  function formatLogTime(timestamp) {
    const parsed = new Date(timestamp);
    if (Number.isNaN(parsed.getTime())) return timestamp;
    return parsed.toLocaleTimeString([], {
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
    });
  }

  function errorToDetail(error) {
    if (!error) return "";
    if (typeof error === "string") return error;
    const message = error.message || String(error);
    const stack = typeof error.stack === "string" ? error.stack : "";
    return stack && !stack.startsWith(message) ? `${message}\n${stack}` : message;
  }

  function escapeHtml(value) {
    return String(value)
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;")
      .replaceAll('"', "&quot;");
  }

  namespace.core = {
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
  };
})(window);
