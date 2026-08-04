"use strict";

(function initUnicornGraphEngineTreeRender(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const state = namespace.state;
  const els = namespace.els;
  const core = namespace.core;
  const treeModel = namespace.treeModel;
  const treeViewport = namespace.treeViewport;

  if (!state || !els || !core || !treeModel || !treeViewport) {
    throw new Error("Unicorn graphengine tree render expected state, DOM, core, tree model, and tree viewport modules to load first.");
  }

  const {
    collectVisible,
    layoutVisible,
    nodeHasChildren,
  } = treeModel;

  function getUi() {
    return namespace.ui || null;
  }

  function getCountViewModeSafe() {
    return typeof getUi()?.getCountViewMode === "function"
      ? getUi().getCountViewMode()
      : "both";
  }

  function getVisibleSeriesSafe() {
    return typeof getUi()?.getVisibleSeries === "function"
      ? getUi().getVisibleSeries()
      : [];
  }

  function redraw() {
    if (!state.tree) return;
    const minReads = 0;
    const search = els.searchBox.value.trim().toLowerCase();
    const visible = [];
    const links = [];
    const leaves = { count: 0 };
    collectVisible(state.tree, null, visible, links, leaves, minReads);
    layoutVisible(state.tree, new Set(visible));
    state.flat = visible;
    globalObject.updateSelectToRankOptions();
    renderSvg(visible, links, search);
    renderSummary(visible);
    globalObject.renderTopTable();
    globalObject.syncBackendRuntimeUiState();
    const activeSeries = getVisibleSeriesSafe().length;
    const directTaxa = Number(state.remote.directTaxa || 0);
    globalObject.setStatus(`Rendered ${visible.length.toLocaleString()} visible nodes from a backend tree with ${directTaxa.toLocaleString()} direct taxa across ${activeSeries.toLocaleString()} active dataset${activeSeries === 1 ? "" : "s"}.`);
  }

  function formatNodeCountSummary(node) {
    const direct = Number(node?.direct || 0).toLocaleString();
    const subtree = Number((node?.subtree ?? node?.total) || 0).toLocaleString();
    const mode = getCountViewModeSafe();
    if (mode === "direct") return `${direct} direct`;
    if (mode === "subtree") return `${subtree} cumulative`;
    return `${direct} direct / ${subtree} cumulative`;
  }

  function renderSvg(nodes, links, search) {
    const maxDepth = nodes.reduce((m, n) => Math.max(m, n.depth), 0);
    const maxY = nodes.reduce((m, n) => Math.max(m, n.y || 0), 0);
    const width = Math.max(980, maxDepth * 230 + 420);
    const height = Math.max(520, maxY + 80);
    const mode = els.countMode.value;
    const maxValue = Math.max(1, ...nodes.map((n) => mode === "direct" ? n.direct : n.total));

    els.svg.setAttribute("width", width);
    els.svg.setAttribute("height", height);
    els.svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
    treeViewport.setBaseDimensions(width, height);
    els.svg.innerHTML = "";

    const edgeLayer = core.svgEl("g", { class: "edges" });
    for (const [a, b] of links) {
      edgeLayer.appendChild(core.svgEl("path", {
        class: "edge",
        d: `M ${a.x} ${a.y} C ${a.x + 95} ${a.y}, ${b.x - 95} ${b.y}, ${b.x} ${b.y}`,
      }));
    }
    els.svg.appendChild(edgeLayer);

    const nodeLayer = core.svgEl("g", { class: "nodes" });
    for (const node of nodes) {
      const value = mode === "direct" ? node.direct : node.total;
      const match = search && (`${node.taxid} ${node.name}`.toLowerCase().includes(search));
      const dim = search && !match;
      const selected = state.selected.has(node.taxid);
      const group = core.svgEl("g", {
        class: `node${match ? " match" : ""}${dim ? " dim" : ""}${selected ? " selected" : ""}`,
        transform: `translate(${node.x}, ${node.y})`,
      });
      const radius = radiusFor(value, maxValue);
      group.appendChild(core.svgEl("circle", {
        class: "node-hit",
        r: Math.max(radius + 7, 12),
      }));
      renderNodePie(group, node, radius);
      group.appendChild(core.svgEl("text", {
        x: radius + 8,
        y: -5,
      }, labelFor(node)));
      group.appendChild(core.svgEl("text", {
        class: "count",
        x: radius + 8,
        y: 10,
      }, formatNodeCountSummary(node)));
      group.addEventListener("click", (event) => {
        if (performance.now() < state.suppressClicksUntil) return;
        if (event.ctrlKey || event.metaKey) {
          if (state.clickTimer) {
            clearTimeout(state.clickTimer);
            state.clickTimer = null;
          }
          globalObject.toggleSelection(node);
          return;
        }
        if (state.clickTimer) clearTimeout(state.clickTimer);
        state.clickTimer = setTimeout(() => {
          state.clickTimer = null;
          globalObject.toggleCollapse(node);
        }, 220);
      });
      group.addEventListener("dblclick", (event) => {
        if (performance.now() < state.suppressClicksUntil) return;
        event.preventDefault();
        if (state.clickTimer) {
          clearTimeout(state.clickTimer);
          state.clickTimer = null;
        }
        globalObject.toggleFocus(node);
      });
      group.addEventListener("mouseenter", (event) => scheduleTooltip(event, node));
      group.addEventListener("mousemove", (event) => updateTooltipPosition(event));
      group.addEventListener("mouseleave", hideTooltip);
      nodeLayer.appendChild(group);
    }
    els.svg.appendChild(nodeLayer);
    if (state.centerOnNextRender) {
      requestCenterRoot();
      state.centerOnNextRender = false;
    }
  }

  function renderNodePie(group, node, radius) {
    const values = getNodeSeriesValues(node);
    const total = values.reduce((sum, value) => sum + value, 0);
    if (total <= 0) {
      group.appendChild(core.svgEl("circle", {
        r: radius,
        fill: fillFor(node),
      }));
      return;
    }
    const origin = -Math.PI / 2;
    let start = origin;
    let drawn = 0;
    for (let i = 0; i < values.length; i++) {
      const value = values[i];
      if (!value) continue;
      const fraction = value / total;
      const end = drawn + fraction >= 0.999999
        ? origin + (Math.PI * 2)
        : start + (Math.PI * 2 * fraction);
      group.appendChild(core.svgEl("path", {
        d: pieSlicePath(radius, start, end),
        fill: state.series[i]?.color || fillFor(node),
      }));
      start = end;
      drawn += fraction;
    }
    group.appendChild(core.svgEl("circle", {
      r: radius,
      fill: "none",
      stroke: "#1f2b2e",
      "stroke-width": "1.3",
    }));
  }

  function getNodeSeriesValues(node) {
    const values = els.countMode.value === "direct" ? node.directBySource : node.totalBySource;
    return values.map((value, index) => state.series[index]?.visible ? value : 0);
  }

  function pieSlicePath(radius, startAngle, endAngle) {
    if (Math.abs(endAngle - startAngle) >= Math.PI * 2 - 0.0001) {
      return [
        `M 0 ${-radius}`,
        `A ${radius} ${radius} 0 1 1 0 ${radius}`,
        `A ${radius} ${radius} 0 1 1 0 ${-radius}`,
        "Z",
      ].join(" ");
    }
    const x1 = Math.cos(startAngle) * radius;
    const y1 = Math.sin(startAngle) * radius;
    const x2 = Math.cos(endAngle) * radius;
    const y2 = Math.sin(endAngle) * radius;
    const largeArc = endAngle - startAngle > Math.PI ? 1 : 0;
    return `M 0 0 L ${x1} ${y1} A ${radius} ${radius} 0 ${largeArc} 1 ${x2} ${y2} Z`;
  }

  function requestCenterRoot() {
    requestAnimationFrame(() => {
      requestAnimationFrame(centerRoot);
    });
  }

  function centerRoot() {
    centerNode(state.tree);
  }

  function centerNode(node) {
    if (!node || !els.chartWrap) return;
    const rootX = node.x || 0;
    const rootY = node.y || 0;
    els.chartWrap.scrollTo({
      left: Math.max(0, rootX - 80),
      top: Math.max(0, rootY - (els.chartWrap.clientHeight / 2)),
      behavior: "auto",
    });
  }

  function radiusFor(value, maxValue) {
    if (!value) return 4;
    const t = els.scaleMode.value === "linear" ? value / maxValue : Math.sqrt(value / maxValue);
    return 4 + t * 24;
  }

  function fillFor(node) {
    if (node.taxid === 0) return "#6f7a80";
    if (node.direct > 0 && nodeHasChildren(node)) return "#e2a44e";
    if (node.direct > 0) return "#c85f43";
    if (node.depth === 0) return "#255f75";
    return "#9cad9f";
  }

  function labelFor(node) {
    const name = node.name || String(node.taxid);
    return name.length > 34 ? `${name.slice(0, 31)}...` : name;
  }

  function positionTooltip(event) {
    els.tooltip.style.left = `${event.clientX + 14}px`;
    els.tooltip.style.top = `${event.clientY + 14}px`;
  }

  function scheduleTooltip(event, node) {
    if (state.tooltipTimer) clearTimeout(state.tooltipTimer);
    state.tooltipNode = node;
    state.tooltipPoint = { clientX: event.clientX, clientY: event.clientY };
    state.tooltipTimer = setTimeout(() => {
      state.tooltipTimer = null;
      if (state.tooltipNode !== node) return;
      showTooltip(state.tooltipPoint || event, node);
    }, 360);
  }

  function updateTooltipPosition(event) {
    state.tooltipPoint = { clientX: event.clientX, clientY: event.clientY };
    if (els.tooltip.hidden) return;
    positionTooltip(event);
  }

  function showTooltip(event, node) {
    showBackendTooltip(event, node);
  }

  async function showBackendTooltip(event, node) {
    const requestId = ++state.remote.tooltipRequestId;
    els.tooltip.hidden = false;
    positionTooltip(event);
    els.tooltip.innerHTML = `
      <strong>${core.escapeHtml(node.name)}</strong>
      taxid: ${node.taxid}<br>
      loading tooltip...
    `;
    try {
      const payload = await globalObject.fetchRemoteNodeTooltip(node.taxid);
      if (requestId !== state.remote.tooltipRequestId) return;
      if (state.tooltipNode !== node) return;
      const remoteNode = payload.node || {};
      const countMode = getCountViewModeSafe();
      const breakdown = Array.isArray(remoteNode.datasets)
        ? remoteNode.datasets
          .filter((entry) => Number(entry.subtree || 0) > 0 || Number(entry.direct || 0) > 0)
          .sort((a, b) => {
            if (countMode === "direct") return Number(b.direct || 0) - Number(a.direct || 0);
            return Number(b.subtree || 0) - Number(a.subtree || 0);
          })
          .map((entry, index) => `
            <div class="tooltip-source">
              <span class="tooltip-swatch" style="background:${globalObject.getDatasetColor(entry.dataset, index)}"></span>
              <span>${core.escapeHtml(entry.dataset)}: ${formatNodeCountSummary(entry)}</span>
            </div>
          `)
          .join("")
        : "";
      els.tooltip.hidden = false;
      positionTooltip(state.tooltipPoint || event);
      els.tooltip.innerHTML = `
        <strong>${core.escapeHtml(remoteNode.name || node.name)}</strong>
        taxid: ${Number(remoteNode.taxid ?? node.taxid)}<br>
        rank: ${core.escapeHtml(remoteNode.rank || node.rank || "NA")}<br>
        counts: ${core.escapeHtml(formatNodeCountSummary(remoteNode))}<br>
        children: ${Number(remoteNode.child_count || 0).toLocaleString()}<br>
        ${breakdown ? `<div class="tooltip-breakdown"><em>per dataset</em>${breakdown}</div>` : ""}
      `;
    } catch (error) {
      if (requestId !== state.remote.tooltipRequestId) return;
      if (state.tooltipNode !== node) return;
      els.tooltip.hidden = false;
      positionTooltip(state.tooltipPoint || event);
      els.tooltip.innerHTML = `
        <strong>${core.escapeHtml(node.name)}</strong>
        taxid: ${node.taxid}<br>
        ${core.escapeHtml(error.message || "Could not load tooltip.")}
      `;
    }
  }

  function hideTooltip() {
    if (state.tooltipTimer) {
      clearTimeout(state.tooltipTimer);
      state.tooltipTimer = null;
    }
    state.remote.tooltipRequestId++;
    state.tooltipNode = null;
    state.tooltipPoint = null;
    els.tooltip.hidden = true;
  }

  function renderSummary(visible) {
    const totalReads = globalObject.hasBackendTree() ? state.remote.totalReads : 0;
    const directTaxa = globalObject.hasBackendTree() ? state.remote.directTaxa : 0;
    els.readCount.textContent = totalReads.toLocaleString();
    els.taxonCount.textContent = directTaxa.toLocaleString();
    els.visibleCount.textContent = visible.length.toLocaleString();
    els.missingCount.textContent = state.missingTaxids.size.toLocaleString();
    els.selectedCount.textContent = state.selected.size.toLocaleString();
  }

  function initChartSelectionClear() {
    els.chartWrap.addEventListener("click", (event) => {
      if (performance.now() < state.suppressClicksUntil) return;
      if (!(event.ctrlKey || event.metaKey)) return;
      if (event.target.closest(".node")) return;
      if (!state.selected.size) return;
      event.preventDefault();
      state.selected.clear();
      redraw();
    });
  }

  treeViewport.setTooltipHider(hideTooltip);

  namespace.treeRender = {
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
    initChartSelectionClear,
  };

  Object.assign(globalObject, namespace.treeRender);
})(window);
