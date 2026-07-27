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
    formatBytes,
    svgEl,
    formatLogTime,
    errorToDetail,
    escapeHtml,
  };
})(window);
