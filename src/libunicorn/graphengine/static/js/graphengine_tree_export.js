"use strict";

(function initUnicornGraphEngineTreeExport(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const els = namespace.els;

  if (!els) {
    throw new Error("Unicorn graphengine tree export expected the DOM module to load first.");
  }

  const EXPORT_STYLE = `
    text { font-family: "Avenir Next", "Segoe UI", sans-serif; fill: #1f2b2e; }
    .edge { fill: none; stroke: rgba(91, 107, 95, 0.44); stroke-width: 1.4; }
    .node .node-hit { fill: transparent; stroke: none; }
    .node circle { stroke: #1f2b2e; stroke-width: 1.3; }
    .node path { stroke: none; }
    .node text { dominant-baseline: middle; font-size: 12px; paint-order: stroke; stroke: rgba(255, 253, 247, 0.85); stroke-width: 3px; stroke-linejoin: round; }
    .node text.count { fill: #5b6b5f; font-size: 10px; font-weight: 700; }
    .node.selected circle { stroke: #14211c; stroke-width: 2.5; }
    .node.dim { opacity: 0.22; }
    .node.match text { fill: #a84f31; font-weight: 900; }
  `;
  let initialized = false;

  function hasRenderedTree() {
    return Boolean(els.svg?.querySelector(".nodes .node"));
  }

  function hideMenu() {
    if (!els.treeExportMenu) return;
    els.treeExportMenu.hidden = true;
  }

  function positionMenu(clientX, clientY) {
    const menu = els.treeExportMenu;
    if (!menu) return;
    menu.hidden = false;
    const margin = 8;
    const left = Math.min(clientX, window.innerWidth - menu.offsetWidth - margin);
    const top = Math.min(clientY, window.innerHeight - menu.offsetHeight - margin);
    menu.style.left = `${Math.max(margin, left)}px`;
    menu.style.top = `${Math.max(margin, top)}px`;
  }

  function createExportSvg() {
    if (!els.svg || !hasRenderedTree()) return null;
    const clone = els.svg.cloneNode(true);
    const viewBox = clone.getAttribute("viewBox") || "0 0 1 1";
    const [, , width, height] = viewBox.trim().split(/\s+/);
    clone.setAttribute("xmlns", "http://www.w3.org/2000/svg");
    clone.setAttribute("width", width || "1");
    clone.setAttribute("height", height || "1");
    clone.setAttribute("preserveAspectRatio", "xMinYMin meet");
    clone.removeAttribute("role");
    clone.removeAttribute("aria-label");

    const style = document.createElementNS("http://www.w3.org/2000/svg", "style");
    style.textContent = EXPORT_STYLE;
    clone.insertBefore(style, clone.firstChild);
    return new XMLSerializer().serializeToString(clone);
  }

  function exportFilename() {
    const today = new Date().toISOString().slice(0, 10);
    return `unicorn_tree_${today}.svg`;
  }

  function downloadCurrentTree() {
    const svgText = createExportSvg();
    hideMenu();
    if (!svgText) return;
    const blob = new Blob([`<?xml version="1.0" encoding="UTF-8"?>\n${svgText}`], {
      type: "image/svg+xml;charset=utf-8",
    });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = exportFilename();
    document.body.appendChild(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(url);
  }

  function init() {
    if (initialized || !els.chartWrap || !els.treeExportMenu || !els.treeExportSvgBtn) return;
    initialized = true;

    els.chartWrap.addEventListener("contextmenu", (event) => {
      if (!hasRenderedTree()) return;
      event.preventDefault();
      namespace.treeRender?.hideTooltip?.();
      positionMenu(event.clientX, event.clientY);
      els.treeExportSvgBtn.focus();
    });
    els.treeExportSvgBtn.addEventListener("click", downloadCurrentTree);
    document.addEventListener("pointerdown", (event) => {
      if (!els.treeExportMenu.contains(event.target)) hideMenu();
    });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") hideMenu();
    });
  }

  namespace.treeExport = {
    init,
    createExportSvg,
    downloadCurrentTree,
  };
})(window);
