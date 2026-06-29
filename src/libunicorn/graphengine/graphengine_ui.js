"use strict";

(function initUnicornGraphEngineUi(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const state = namespace.state;
  const els = namespace.els;
  const core = namespace.core;

  if (!state || !els || !core) {
    throw new Error("Unicorn graphengine UI expected state, DOM, and core modules to load first.");
  }

  function initRemotePanel() {
    const savedUser = localStorage.getItem("unicorn.remoteUser") || "";
    const savedHost = localStorage.getItem("unicorn.remoteHost") || "";
    els.remoteUser.value = savedUser;
    els.remoteHost.value = savedHost;
    globalObject.updateTunnelHint();
    globalObject.updateConnectionState(false, "Not connected");
    globalObject.renderRemoteDatasets();
    if (typeof globalObject.initMetadataControls === "function") {
      globalObject.initMetadataControls();
    }
    if (typeof globalObject.renderMetadataSummary === "function") {
      globalObject.renderMetadataSummary();
    }
    globalObject.syncBackendRuntimeUiState();
    renderClientLog();
  }

  function initFileInputs() {
    ensureFileRowControls();
    els.nodesFile.addEventListener("change", globalObject.updateRenderAvailability);
    els.namesFile.addEventListener("change", globalObject.updateRenderAvailability);
    if (els.metadataFile) {
      els.metadataFile.addEventListener("change", globalObject.updateRenderAvailability);
    }
    els.lcaInputs.addEventListener("change", (event) => {
      if (event.target && event.target.matches(".lca-file-input")) {
        globalObject.updateRenderAvailability();
      }
    });
  }

  function initMinReadsControls() {
    const savedValue = Number(localStorage.getItem("unicorn.minReadsActual"));
    const savedScale = localStorage.getItem("unicorn.minReadsScale");
    const savedMax = Number(localStorage.getItem("unicorn.minReadsMax"));
    state.minReadsActual = Number.isFinite(savedValue) && savedValue >= 0 ? Math.round(savedValue) : 0;
    if (els.minReadsScale && (savedScale === "log" || savedScale === "linear")) {
      els.minReadsScale.value = savedScale;
    }
    if (els.minReadsMax && Number.isFinite(savedMax) && savedMax >= 1) {
      els.minReadsMax.value = String(Math.round(savedMax));
    }
    syncMinReadsControl();
  }

  function getMinReadsMax() {
    if (els.minReadsMax) {
      const configured = Number(els.minReadsMax.value || 0);
      if (Number.isFinite(configured) && configured >= 1) {
        return Math.round(configured);
      }
    }
    return 1000;
  }

  function getMinReadsScale() {
    return els.minReadsScale && els.minReadsScale.value === "log" ? "log" : "linear";
  }

  function getMinReadsValue() {
    return Math.max(0, Math.round(state.minReadsActual || 0));
  }

  function setMinReadsValue(value) {
    const maxValue = getMinReadsMax();
    state.minReadsActual = Math.max(0, Math.min(maxValue, Math.round(Number(value) || 0)));
    localStorage.setItem("unicorn.minReadsActual", String(state.minReadsActual));
    syncMinReadsControl();
  }

  function valueFromSliderPosition(position) {
    const sliderValue = Math.max(0, Math.min(1000, Number(position) || 0));
    const maxValue = getMinReadsMax();
    if (getMinReadsScale() === "log") {
      if (sliderValue <= 0) return 0;
      return Math.round(Math.exp((sliderValue / 1000) * Math.log(maxValue + 1)) - 1);
    }
    return Math.round((sliderValue / 1000) * maxValue);
  }

  function sliderPositionFromValue(value) {
    const clampedValue = Math.max(0, Math.min(getMinReadsMax(), Number(value) || 0));
    const maxValue = getMinReadsMax();
    if (getMinReadsScale() === "log") {
      if (clampedValue <= 0) return 0;
      return Math.round((Math.log(clampedValue + 1) / Math.log(maxValue + 1)) * 1000);
    }
    return Math.round((clampedValue / maxValue) * 1000);
  }

  function syncMinReadsControl() {
    state.minReadsActual = Math.max(0, Math.min(getMinReadsMax(), getMinReadsValue()));
    els.minReads.value = String(sliderPositionFromValue(state.minReadsActual));
    if (els.minReadsValue) {
      els.minReadsValue.textContent = state.minReadsActual.toLocaleString();
    }
  }

  async function handleMinReadsScaleChange() {
    localStorage.setItem("unicorn.minReadsScale", getMinReadsScale());
    syncMinReadsControl();
    if (globalObject.hasBackendTree()) {
      try {
        const payload = await globalObject.fetchRemoteVisibleTree({
          minReads: getMinReadsValue(),
          expandedTaxids: Array.from(state.remote.expandedTaxids),
        });
        globalObject.applyRemoteVisiblePayload(payload);
        await globalObject.refreshCurrentReportIfNeeded();
        globalObject.redraw();
        return;
      } catch (error) {
        globalObject.setStatus(`Could not refresh backend tree after changing the read-scale mode: ${error.message || error}`);
        return;
      }
    }
    globalObject.redraw();
  }

  async function handleMinReadsMaxChange() {
    if (els.minReadsMax) {
      const numeric = Number(els.minReadsMax.value || 0);
      els.minReadsMax.value = String(Math.max(1, Math.round(Number.isFinite(numeric) ? numeric : 1)));
      localStorage.setItem("unicorn.minReadsMax", els.minReadsMax.value);
    }
    syncMinReadsControl();
    if (globalObject.hasBackendTree()) {
      try {
        const payload = await globalObject.fetchRemoteVisibleTree({
          minReads: getMinReadsValue(),
          expandedTaxids: Array.from(state.remote.expandedTaxids),
        });
        globalObject.applyRemoteVisiblePayload(payload);
        await globalObject.refreshCurrentReportIfNeeded();
        globalObject.redraw();
        return;
      } catch (error) {
        globalObject.setStatus(`Could not refresh backend tree after changing the slider range: ${error.message || error}`);
        return;
      }
    }
    globalObject.redraw();
  }

  function ensureFileRowControls() {
    els.lcaInputs.querySelectorAll(".file-row").forEach((row) => {
      let removeBtn = row.querySelector(".remove-file-btn");
      if (!removeBtn) {
        removeBtn = document.createElement("button");
        removeBtn.type = "button";
        removeBtn.className = "secondary remove-file-btn";
        removeBtn.setAttribute("aria-label", "Remove input file");
        removeBtn.textContent = "Remove";
        row.appendChild(removeBtn);
      }
      if (removeBtn.dataset.bound === "1") return;
      removeBtn.dataset.bound = "1";
      removeBtn.addEventListener("click", () => {
        const inputs = els.lcaInputs.querySelectorAll(".file-row");
        if (inputs.length <= 1) {
          const fileInput = row.querySelector(".lca-file-input");
          if (fileInput) fileInput.value = "";
          return;
        }
        row.remove();
      });
    });
  }

  function initSidebarPanel() {
    const savedCollapsed = localStorage.getItem("unicorn.sidebarCollapsed") === "true";
    setSidebarCollapsed(savedCollapsed);
    initSectionToggle("remoteSection", els.toggleRemoteSection, els.remoteSectionBody);
    initSectionToggle("filesSection", els.toggleFilesSection, els.filesSectionBody);
    initSectionToggle("metadataSection", els.toggleMetadataSection, els.metadataSectionBody);
    initSectionToggle("optionsSection", els.toggleOptionsSection, els.optionsSectionBody);
    initSectionToggle("reportsSection", els.toggleReportsSection, els.reportsSectionBody);
    initSectionToggle("agentSection", els.toggleAgentSection, els.agentSectionBody);
    initSectionToggle("logSection", els.toggleLogSection, els.logSectionBody);
  }

  function initSectionToggle(key, button, body) {
    const collapsed = localStorage.getItem(`unicorn.${key}.collapsed`) === "true";
    setSectionCollapsed(button, body, collapsed);
    button.addEventListener("click", () => {
      const next = button.getAttribute("aria-expanded") !== "true";
      setSectionCollapsed(button, body, !next);
      localStorage.setItem(`unicorn.${key}.collapsed`, String(!next));
    });
  }

  function setSectionCollapsed(button, body, collapsed) {
    const section = button.closest(".panel-section");
    if (section) section.classList.toggle("collapsed", collapsed);
    body.hidden = collapsed;
    button.textContent = collapsed ? "Show" : "Hide";
    button.setAttribute("aria-expanded", String(!collapsed));
  }

  function toggleSidebar() {
    const collapsed = !els.app.classList.contains("sidebar-collapsed");
    setSidebarCollapsed(collapsed);
    localStorage.setItem("unicorn.sidebarCollapsed", String(collapsed));
    requestAnimationFrame(() => {
      if (state.tree) {
        globalObject.centerNode(state.focusTaxid
          ? state.flat.find((node) => node.taxid === state.focusTaxid) || state.tree
          : state.tree);
      }
    });
  }

  function setSidebarCollapsed(collapsed) {
    els.app.classList.toggle("sidebar-collapsed", collapsed);
    els.sidebarToggle.textContent = collapsed ? "Show Panel" : "Hide Panel";
    els.sidebarToggle.setAttribute("aria-expanded", String(!collapsed));
  }

  function getCountViewMode() {
    const mode = String(els.countViewMode?.value || "both");
    return mode === "direct" || mode === "subtree" ? mode : "both";
  }

  function getCountViewLabel() {
    const mode = getCountViewMode();
    if (mode === "direct") return "direct";
    if (mode === "subtree") return "cumulative";
    return "direct + cumulative";
  }

  function initControlsResize() {
    const saved = Number(localStorage.getItem("unicorn.controlsWidth"));
    if (Number.isFinite(saved) && saved > 0) setControlsWidth(saved);
    let startX = 0;
    let startWidth = 0;
    els.controlsResize.addEventListener("pointerdown", (event) => {
      if (els.app.classList.contains("sidebar-collapsed")) return;
      startX = event.clientX;
      startWidth = document.documentElement.style.getPropertyValue("--controls-width")
        ? Number.parseFloat(document.documentElement.style.getPropertyValue("--controls-width"))
        : els.controls.getBoundingClientRect().width;
      els.controlsResize.setPointerCapture(event.pointerId);
      document.body.style.userSelect = "none";
      document.body.style.cursor = "col-resize";
    });
    els.controlsResize.addEventListener("pointermove", (event) => {
      if (!els.controlsResize.hasPointerCapture(event.pointerId)) return;
      setControlsWidth(startWidth + event.clientX - startX);
    });
    els.controlsResize.addEventListener("pointerup", (event) => {
      if (els.controlsResize.hasPointerCapture(event.pointerId)) {
        els.controlsResize.releasePointerCapture(event.pointerId);
      }
      document.body.style.userSelect = "";
      document.body.style.cursor = "";
      localStorage.setItem("unicorn.controlsWidth", String(Math.round(els.controls.getBoundingClientRect().width)));
      requestAnimationFrame(() => {
        if (state.tree) {
          globalObject.centerNode(state.focusTaxid ? state.flat.find((node) => node.taxid === state.focusTaxid) || state.tree : state.tree);
        }
      });
    });
  }

  function setControlsWidth(value) {
    const width = Math.max(220, Math.min(540, value));
    document.documentElement.style.setProperty("--controls-width", `${width}px`);
  }

  function initSummaryResize() {
    const saved = Number(localStorage.getItem("unicorn.summaryHeight"));
    if (Number.isFinite(saved) && saved > 0) setSummaryHeight(saved);
    let startY = 0;
    let startHeight = 0;
    els.summaryResize.addEventListener("pointerdown", (event) => {
      startY = event.clientY;
      startHeight = els.summaryPanel.getBoundingClientRect().height;
      els.summaryResize.setPointerCapture(event.pointerId);
      document.body.style.userSelect = "none";
    });
    els.summaryResize.addEventListener("pointermove", (event) => {
      if (!els.summaryResize.hasPointerCapture(event.pointerId)) return;
      setSummaryHeight(startHeight + event.clientY - startY);
      requestAnimationFrame(() => {
        if (state.tree) globalObject.centerRoot();
      });
    });
    els.summaryResize.addEventListener("pointerup", (event) => {
      if (els.summaryResize.hasPointerCapture(event.pointerId)) {
        els.summaryResize.releasePointerCapture(event.pointerId);
      }
      document.body.style.userSelect = "";
      localStorage.setItem("unicorn.summaryHeight", String(Math.round(els.summaryPanel.getBoundingClientRect().height)));
    });
  }

  function setSummaryHeight(value) {
    const height = Math.max(48, Math.min(180, value));
    els.summaryPanel.style.setProperty("--summary-height", `${height}px`);
  }

  function initTablePanel() {
    const saved = localStorage.getItem("unicorn.tableVisible");
    setTablePanelVisible(saved === "1");
  }

  function initTableResize() {
    if (!els.tableResize) return;
    const saved = Number(localStorage.getItem("unicorn.tableHeight"));
    if (Number.isFinite(saved) && saved > 0) setTableHeight(saved);
    let startY = 0;
    let startHeight = 0;
    els.tableResize.addEventListener("pointerdown", (event) => {
      startY = event.clientY;
      startHeight = els.tablePanel.getBoundingClientRect().height;
      els.tableResize.setPointerCapture(event.pointerId);
      document.body.style.userSelect = "none";
      document.body.style.cursor = "row-resize";
    });
    els.tableResize.addEventListener("pointermove", (event) => {
      if (!els.tableResize.hasPointerCapture(event.pointerId)) return;
      setTableHeight(startHeight - (event.clientY - startY));
    });
    els.tableResize.addEventListener("pointerup", (event) => {
      if (els.tableResize.hasPointerCapture(event.pointerId)) {
        els.tableResize.releasePointerCapture(event.pointerId);
      }
      document.body.style.userSelect = "";
      document.body.style.cursor = "";
      localStorage.setItem("unicorn.tableHeight", String(Math.round(els.tablePanel.getBoundingClientRect().height)));
    });
  }

  function toggleTablePanel() {
    setTablePanelVisible(els.tablePanel.hidden);
    localStorage.setItem("unicorn.tableVisible", els.tablePanel.hidden ? "0" : "1");
    requestAnimationFrame(() => {
      if (state.tree) {
        globalObject.centerNode(state.focusTaxid ? state.flat.find((node) => node.taxid === state.focusTaxid) || state.tree : state.tree);
      }
    });
  }

  function setTablePanelVisible(visible) {
    els.tablePanel.hidden = !visible;
    els.tablePanel.classList.toggle("hidden", !visible);
    els.toggleTableBtn.textContent = visible ? "Hide Counts" : "Show Counts";
    els.toggleTableBtn.setAttribute("aria-expanded", visible ? "true" : "false");
  }

  function setTableHeight(value) {
    const height = Math.max(140, Math.min(window.innerHeight * 0.7, value));
    document.documentElement.style.setProperty("--table-height", `${height}px`);
  }

  function addLcaInput() {
    const row = document.createElement("div");
    row.className = "file-row";
    row.innerHTML = `
      <input class="lca-file-input" type="file" accept=".txt,.tsv,.bdamage,.lca">
      <button class="secondary remove-file-btn" type="button" aria-label="Remove input file">Remove</button>
    `;
    els.lcaInputs.appendChild(row);
    ensureFileRowControls();
  }

  function clearLcaListFile() {
    els.lcaListFile.value = "";
    setStatus("Cleared input file list selection.");
  }

  function colorForSource(index) {
    return core.SOURCE_COLORS[index % core.SOURCE_COLORS.length];
  }

  function renderSourceLegend() {
    if (!state.series.length) {
      els.sourceLegend.hidden = true;
      els.sourceLegend.innerHTML = "";
      return;
    }
    els.sourceLegend.hidden = false;
    els.sourceLegend.innerHTML = state.series.map((source, index) => `
      <label class="legend-item${source.visible ? "" : " is-muted"}" data-series-index="${index}">
        <input class="legend-toggle" type="checkbox" ${source.visible ? "checked" : ""} aria-label="Toggle ${core.escapeHtml(source.label)}">
        <span class="legend-swatch" style="background:${source.color}"></span>
        <span class="legend-label" title="${core.escapeHtml(source.label)}">${core.escapeHtml(source.label)}</span>
      </label>
    `).join("");

    els.sourceLegend.querySelectorAll(".legend-item").forEach((item) => {
      item.addEventListener("change", (event) => {
        const target = event.target;
        if (!(target instanceof HTMLInputElement)) return;
        const index = Number(item.getAttribute("data-series-index"));
        if (!Number.isInteger(index) || !state.series[index]) return;
        state.series[index].visible = target.checked;
        item.classList.toggle("is-muted", !target.checked);
        globalObject.redraw();
      });
    });
  }

  function getVisibleSeries() {
    return state.series.filter((source) => source.visible);
  }

  function setStatus(message) {
    els.status.textContent = String(message || "");
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
            <span class="client-log-badge stage-${core.escapeHtml(entry.stage)}">${core.escapeHtml(entry.stage)}</span>
            <span class="client-log-badge level-${core.escapeHtml(entry.level)}">${core.escapeHtml(entry.level)}</span>
          </div>
          <span class="client-log-time">${core.escapeHtml(core.formatLogTime(entry.timestamp))}</span>
        </div>
        <div class="client-log-message">${core.escapeHtml(entry.message)}</div>
        ${entry.detail ? `<pre class="client-log-detail">${core.escapeHtml(entry.detail)}</pre>` : ""}
      </div>
    `).join("");
  }

  namespace.ui = {
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
    setStatus,
    addClientLog,
    clearClientLog,
    renderClientLog,
  };

  Object.assign(globalObject, {
    setStatus,
    addClientLog,
    clearClientLog,
    renderClientLog,
  });
})(window);
