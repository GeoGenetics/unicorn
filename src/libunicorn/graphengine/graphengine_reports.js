"use strict";

(function initUnicornGraphEngineReports(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const state = namespace.state;
  const els = namespace.els;
  const core = namespace.core;

  if (!state || !els || !core) {
    throw new Error("Unicorn graphengine reports expected state, DOM, and core modules to load first.");
  }

  const {
    SOURCE_COLORS,
    escapeHtml,
  } = core;

  function getUi() {
    return namespace.ui || null;
  }

  function getSelection() {
    return namespace.selection || null;
  }

  function getCountViewModeSafe() {
    return typeof getUi()?.getCountViewMode === "function"
      ? getUi().getCountViewMode()
      : "both";
  }

  function getCountViewLabelSafe() {
    return typeof getUi()?.getCountViewLabel === "function"
      ? getUi().getCountViewLabel()
      : "Direct / Cumulative";
  }

  function getMinReadsValueSafe() {
    return typeof getUi()?.getMinReadsValue === "function"
      ? getUi().getMinReadsValue()
      : 0;
  }

  function setTablePanelVisibleSafe(visible) {
    if (typeof getUi()?.setTablePanelVisible === "function") {
      getUi().setTablePanelVisible(visible);
    }
  }

  function getSelectedNodesSafe() {
    return typeof getSelection()?.getSelectedNodes === "function"
      ? getSelection().getSelectedNodes()
      : [];
  }

  function hasSelectionSafe() {
    return typeof getSelection()?.hasSelection === "function"
      ? getSelection().hasSelection()
      : false;
  }

  function getActiveNodesFilename() {
    const requestContext = globalObject.getActiveBackendRequestContext?.();
    if (requestContext?.nodes_file) return requestContext.nodes_file;
    if (els.nodesFile?.files?.[0]) return els.nodesFile.files[0].name;
    if (state.remote.backendNodesFile) return state.remote.backendNodesFile;
    return null;
  }

  function getActiveNamesFilename() {
    const requestContext = globalObject.getActiveBackendRequestContext?.();
    if (requestContext?.names_file) return requestContext.names_file;
    if (els.namesFile?.files?.[0]) return els.namesFile.files[0].name;
    if (state.remote.backendNamesFile) return state.remote.backendNamesFile;
    return null;
  }

  function renderTopTable() {
    if (state.remote.currentReport) {
      renderCurrentReportView();
      return;
    }
    clearSubtreeReportView({ preserveTableContext: true });
    renderBackendTopTable();
  }

  function clearSubtreeReportView(options = {}) {
    const preserveTableContext = Boolean(options.preserveTableContext);
    state.remote.currentReport = null;
    if (!preserveTableContext) {
      state.remote.currentTable = null;
    }
    if (els.subtreeReport) {
      els.subtreeReport.hidden = true;
      els.subtreeReport.innerHTML = "";
    }
    if (els.tablePanelTitle) {
      const targetName = state.remote.currentTable?.target?.name;
      els.tablePanelTitle.textContent = targetName
        ? `Top direct placements: ${targetName}`
        : "Top direct placements";
    }
    if (els.exportMatrixBtn) {
      els.exportMatrixBtn.hidden = true;
    }
    if (els.pcoaMetric) {
      els.pcoaMetric.hidden = true;
    }
    if (els.pcoaBtn) {
      els.pcoaBtn.hidden = true;
    }
    if (els.barplotBtn) {
      els.barplotBtn.hidden = true;
    }
    if (els.topTableWrap) {
      els.topTableWrap.hidden = false;
    }
  }

  async function openSelectedSubtreeReport() {
    if (!globalObject.hasBackendTree?.()) {
      setStatus("Render a backend-backed tree first, then request a count matrix.");
      return;
    }
    if (!hasSelectionSafe()) {
      setStatus("Select one or more nodes first, then use Count Matrix.");
      return;
    }
    const selectedNodes = getSelectedNodesSafe();
    if (!selectedNodes.length) {
      setStatus("Select one or more nodes first, then use Count Matrix.");
      return;
    }
    const taxids = selectedNodes.map((node) => node.taxid);
    setTablePanelVisibleSafe(true);
    const requestId = ++state.remote.tableRequestId;
    if (!els.subtreeReport || !els.tablePanelTitle) {
      setStatus("Count matrix UI is not available in the current HTML shell. Try a hard refresh.");
      return;
    }
    els.tablePanelTitle.textContent = "Count matrix";
    els.subtreeReport.hidden = false;
    els.subtreeReport.innerHTML = `<div class="subtree-report-card">Loading count matrix for ${taxids.length} selected node(s)...</div>`;
    els.topTable.innerHTML = "";
    try {
      const payload = await globalObject.fetchRemoteSubtreeReport(null, { taxids });
      if (requestId !== state.remote.tableRequestId) return;
      renderSubtreeReport(payload.report || null);
    } catch (error) {
      if (requestId !== state.remote.tableRequestId) return;
      els.subtreeReport.hidden = false;
      els.subtreeReport.innerHTML = `
        <div class="subtree-report-card">
          ${escapeHtml(error.message || "Could not load count matrix.")}
        </div>
      `;
      els.topTable.innerHTML = "";
    }
  }

  async function openSelectedRankReport() {
    if (!globalObject.hasBackendTree?.()) {
      setStatus("Render a backend-backed tree first, then request a rank report.");
      return;
    }
    if (!hasSelectionSafe()) {
      setStatus("Select one or more nodes first, then use Rank Report.");
      return;
    }
    const selectedNodes = getSelectedNodesSafe();
    if (!selectedNodes.length) {
      setStatus("Select one or more nodes first, then use Rank Report.");
      return;
    }
    const taxids = selectedNodes.map((node) => node.taxid);
    setTablePanelVisibleSafe(true);
    const requestId = ++state.remote.tableRequestId;
    if (!els.subtreeReport || !els.tablePanelTitle) {
      setStatus("Rank report UI is not available in the current HTML shell. Try a hard refresh.");
      return;
    }
    els.tablePanelTitle.textContent = "Rank report";
    els.subtreeReport.hidden = false;
    els.subtreeReport.innerHTML = `<div class="subtree-report-card">Loading rank report for ${taxids.length} selected node(s)...</div>`;
    els.topTable.innerHTML = "";
    try {
      const payload = await globalObject.fetchRemoteRankReport(taxids);
      if (requestId !== state.remote.tableRequestId) return;
      renderRankReport(payload.report || null, taxids);
    } catch (error) {
      if (requestId !== state.remote.tableRequestId) return;
      els.subtreeReport.hidden = false;
      els.subtreeReport.innerHTML = `
        <div class="subtree-report-card">
          ${escapeHtml(error.message || "Could not load rank report.")}
        </div>
      `;
      els.topTable.innerHTML = "";
    }
  }

  async function refreshCurrentReportIfNeeded() {
    if (!globalObject.hasBackendTree?.() || !state.remote.currentReport) return;
    if (state.remote.currentReport.type === "subtree") {
      const taxids = Array.isArray(state.remote.currentReport.report?.summary?.selected_taxids)
        ? state.remote.currentReport.report.summary.selected_taxids
        : [];
      if (!taxids.length) return;
      const payload = await globalObject.fetchRemoteSubtreeReport(null, { taxids });
      renderSubtreeReport(payload.report || null);
      return;
    }
    if (state.remote.currentReport.type === "rank") {
      const taxids = Array.isArray(state.remote.currentReport.report?.summary?.selected_taxids)
        ? state.remote.currentReport.report.summary.selected_taxids
        : [];
      if (!taxids.length) return;
      const payload = await globalObject.fetchRemoteRankReport(taxids);
      renderRankReport(payload.report || null, taxids);
    }
  }

  function renderCurrentReportView() {
    const active = state.remote.currentReport;
    if (!active) return;
    if (active.type === "subtree") {
      renderSubtreeReport(active.report);
      return;
    }
    if (active.type === "rank") {
      const taxids = Array.isArray(active.report?.summary?.selected_taxids) ? active.report.summary.selected_taxids : [];
      renderRankReport(active.report, taxids);
    }
  }

  function renderSubtreeReport(report) {
    if (!els.subtreeReport || !els.tablePanelTitle) return;
    if (!report || !report.summary || !report.matrix) {
      els.subtreeReport.hidden = false;
      els.subtreeReport.innerHTML = `<div class="subtree-report-card">No count matrix was returned by the backend.</div>`;
      els.topTable.innerHTML = "";
      return;
    }
    state.remote.currentReport = { type: "subtree", report };
    const countViewMode = getCountViewModeSafe();
    const countViewLabel = getCountViewLabelSafe();
    els.tablePanelTitle.textContent = `Count matrix (${Number(report.summary.selected_node_count || 0).toLocaleString()} selected)`;
    if (els.exportMatrixBtn) {
      els.exportMatrixBtn.hidden = false;
    }
    if (els.pcoaMetric) {
      els.pcoaMetric.hidden = countViewMode === "both";
    }
    if (els.pcoaBtn) {
      els.pcoaBtn.hidden = countViewMode === "both";
    }
    if (els.barplotBtn) {
      els.barplotBtn.hidden = countViewMode === "both";
    }
    if (els.topTableWrap) {
      els.topTableWrap.hidden = true;
    }
    els.subtreeReport.hidden = false;
    els.subtreeReport.innerHTML = `
      <div class="subtree-report-card">
        <h3>Selected-node ${escapeHtml(countViewLabel)} count matrix</h3>
        <div class="subtree-report-meta">
          <span>selected nodes: ${Number(report.summary.selected_node_count || 0).toLocaleString()}</span>
          <span>${escapeHtml(formatCountSummaryLabel(countViewMode))}: ${escapeHtml(formatCountSummaryValue(report.summary, countViewMode))}</span>
          <span>datasets: ${Array.isArray(report.summary.dataset_names) ? report.summary.dataset_names.length.toLocaleString() : "0"}</span>
        </div>
      </div>
      <div class="subtree-report-grid">
        <div class="subtree-report-card subtree-report-matrix">
          <h3>Selected-node ${escapeHtml(countViewLabel)} counts by dataset</h3>
          ${renderSubtreeMatrix(report.matrix || {})}
        </div>
      </div>
    `;
  }

  function renderRankReport(report, taxids) {
    if (!els.subtreeReport || !els.tablePanelTitle) return;
    if (!report || !report.summary) {
      els.subtreeReport.hidden = false;
      els.subtreeReport.innerHTML = `<div class="subtree-report-card">No rank report was returned by the backend.</div>`;
      els.topTable.innerHTML = "";
      return;
    }
    state.remote.currentReport = {
      type: "rank",
      report,
    };
    els.tablePanelTitle.textContent = `Rank report (${report.summary.selected_node_count} selected)`;
    if (els.exportMatrixBtn) {
      els.exportMatrixBtn.hidden = false;
    }
    if (els.pcoaMetric) {
      els.pcoaMetric.hidden = true;
    }
    if (els.pcoaBtn) {
      els.pcoaBtn.hidden = true;
    }
    if (els.barplotBtn) {
      els.barplotBtn.hidden = true;
    }
    if (els.topTableWrap) {
      els.topTableWrap.hidden = true;
    }
    els.subtreeReport.hidden = false;
    els.subtreeReport.innerHTML = `
      <div class="subtree-report-card">
        <h3>Selected-node direct-read count matrix by rank</h3>
        <div class="subtree-report-meta">
          <span>selected nodes: ${Number(report.summary.selected_node_count || 0).toLocaleString()}</span>
          <span>total direct reads: ${Number(report.summary.total_direct || 0).toLocaleString()}</span>
          <span>ranks: ${Array.isArray(report.rows) ? report.rows.length.toLocaleString() : "0"}</span>
        </div>
      </div>
      <div class="subtree-report-card subtree-report-matrix">
        <h3>Rank-by-dataset direct count matrix</h3>
        ${renderRankMatrix(report)}
      </div>
    `;
  }

  function renderRankMatrix(report) {
    const rows = Array.isArray(report.rows) ? report.rows : [];
    const datasetNames = Array.isArray(report.summary?.dataset_names) ? report.summary.dataset_names : [];
    if (!rows.length || !datasetNames.length) {
      return `<div class="subtree-report-empty">No rank-matrix rows available for the current selection.</div>`;
    }
    return `
      <table>
        <thead>
          <tr>
            <th>Rank</th>
            <th>Nodes</th>
            <th>Total direct</th>
            ${datasetNames.map((name) => `<th>${escapeHtml(name)}</th>`).join("")}
          </tr>
        </thead>
        <tbody>
          ${rows.map((row) => `
            <tr>
              <td>${escapeHtml(row.rank || "no rank")}</td>
              <td>${Number(row.node_count || 0).toLocaleString()}</td>
              <td>${Number(row.direct || 0).toLocaleString()}</td>
              ${(Array.isArray(row.datasets) ? row.datasets : []).map((entry) => `
                <td>${Number(entry.direct || 0).toLocaleString()}</td>
              `).join("")}
            </tr>
          `).join("")}
        </tbody>
      </table>
    `;
  }

  function renderDatasetBreakdownTable(rows, directOnly = false) {
    if (!Array.isArray(rows) || !rows.length) {
      return `<div class="subtree-report-empty">No per-dataset summary available.</div>`;
    }
    return `
      <table>
        <thead>
          <tr>
            <th>Dataset</th>
            <th>Direct</th>
            ${directOnly ? "" : "<th>Subtree</th>"}
          </tr>
        </thead>
        <tbody>
          ${rows.map((row) => `
            <tr>
              <td>${escapeHtml(row.dataset || "")}</td>
              <td>${Number(row.direct || 0).toLocaleString()}</td>
              ${directOnly ? "" : `<td>${Number(row.subtree || 0).toLocaleString()}</td>`}
            </tr>
          `).join("")}
        </tbody>
      </table>
    `;
  }

  function renderSubtreeMatrix(matrix) {
    const rows = Array.isArray(matrix.rows) ? matrix.rows : [];
    const datasetNames = Array.isArray(matrix.dataset_names) ? matrix.dataset_names : [];
    const countViewMode = getCountViewModeSafe();
    if (!rows.length || !datasetNames.length) {
      return `<div class="subtree-report-empty">No count matrix rows available for the current selection.</div>`;
    }
    const valueHeader = countViewMode === "direct"
      ? "Direct"
      : countViewMode === "subtree"
        ? "Cumulative"
        : "Direct / Cumulative";
    return `
      <table>
        <thead>
          <tr>
            <th>Node</th>
            <th>Rank</th>
            <th>${valueHeader}</th>
            ${datasetNames.map((name) => `<th>${escapeHtml(name)}</th>`).join("")}
          </tr>
        </thead>
        <tbody>
          ${rows.map((row) => `
            <tr>
              <td>${escapeHtml(row.name || "")}</td>
              <td>${escapeHtml(row.rank || "NA")}</td>
              <td>${escapeHtml(formatMatrixCountCell(row, countViewMode))}</td>
              ${(Array.isArray(row.datasets) ? row.datasets : []).map((entry) => `
                <td>${escapeHtml(formatMatrixCountCell(entry, countViewMode))}</td>
              `).join("")}
            </tr>
          `).join("")}
        </tbody>
      </table>
    `;
  }

  function getDatasetColor(datasetName, index) {
    const matched = state.series.find((source) => source.label === datasetName);
    if (matched?.color) return matched.color;
    return SOURCE_COLORS[index % SOURCE_COLORS.length];
  }

  function formatCountSummaryLabel(mode) {
    if (mode === "direct") return "total direct reads";
    if (mode === "subtree") return "total cumulative reads";
    return "total counts";
  }

  function formatCountSummaryValue(summary, mode) {
    const direct = Number(summary?.total_direct || 0).toLocaleString();
    const subtree = Number(summary?.total_subtree || 0).toLocaleString();
    if (mode === "direct") return direct;
    if (mode === "subtree") return subtree;
    return `${direct} direct / ${subtree} cumulative`;
  }

  function formatMatrixCountCell(entry, mode) {
    const direct = Number(entry?.direct || 0).toLocaleString();
    const subtree = Number(entry?.subtree || 0).toLocaleString();
    if (mode === "direct") return direct;
    if (mode === "subtree") return subtree;
    return `${direct} / ${subtree}`;
  }

  async function fetchComputedBarplotSpec(taxids, countMode) {
    const activeRequestContext = globalObject.getActiveBackendRequestContext?.();
    const datasetColors = Object.fromEntries(
      state.series.map((source, index) => [source.label, source.color || SOURCE_COLORS[index % SOURCE_COLORS.length]]),
    );
    const payload = {
      taxids: Array.isArray(taxids) ? taxids.map((value) => Number(value)) : [],
      files: activeRequestContext?.dataset_names?.length ? activeRequestContext.dataset_names.slice() : globalObject.getSelectedRemoteDatasets?.(),
      nodes_file: activeRequestContext?.nodes_file ?? getActiveNodesFilename(),
      names_file: activeRequestContext?.names_file ?? getActiveNamesFilename(),
      min_reads: activeRequestContext && Number.isFinite(activeRequestContext.min_reads)
        ? Number(activeRequestContext.min_reads)
        : getMinReadsValueSafe(),
      count_mode: countMode,
      dataset_colors: datasetColors,
    };
    const response = await fetch("http://localhost:8000/compute/barplot", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
      },
      body: JSON.stringify(payload),
    });
    const responsePayload = await response.json().catch(() => ({}));
    if (!response.ok) {
      throw new Error(responsePayload?.detail?.message || `Remote compute/barplot request failed with HTTP ${response.status}`);
    }
    return responsePayload?.spec || null;
  }

  async function fetchComputedPcoaSpec(taxids, countMode, distanceMetric) {
    const activeRequestContext = globalObject.getActiveBackendRequestContext?.();
    const datasetColors = Object.fromEntries(
      state.series.map((source, index) => [source.label, source.color || SOURCE_COLORS[index % SOURCE_COLORS.length]]),
    );
    const payload = {
      taxids: Array.isArray(taxids) ? taxids.map((value) => Number(value)) : [],
      files: activeRequestContext?.dataset_names?.length ? activeRequestContext.dataset_names.slice() : globalObject.getSelectedRemoteDatasets?.(),
      nodes_file: activeRequestContext?.nodes_file ?? getActiveNodesFilename(),
      names_file: activeRequestContext?.names_file ?? getActiveNamesFilename(),
      min_reads: activeRequestContext && Number.isFinite(activeRequestContext.min_reads)
        ? Number(activeRequestContext.min_reads)
        : getMinReadsValueSafe(),
      count_mode: countMode,
      distance_metric: distanceMetric,
      dataset_colors: datasetColors,
    };
    const response = await fetch("http://localhost:8000/compute/pcoa", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
      },
      body: JSON.stringify(payload),
    });
    const responsePayload = await response.json().catch(() => ({}));
    if (!response.ok) {
      throw new Error(responsePayload?.detail?.message || `Remote compute/pcoa request failed with HTTP ${response.status}`);
    }
    return responsePayload?.spec || null;
  }

  async function openCurrentCountMatrixBarplot() {
    const active = state.remote.currentReport;
    if (!active || active.type !== "subtree") {
      setStatus("Open a count matrix first, then use Barplot.");
      return;
    }
    if (getCountViewModeSafe() === "both") {
      setStatus("Barplot is available only in Direct or Cumulative view. Choose one count view first.");
      return;
    }
    const taxids = Array.isArray(active.report?.summary?.selected_taxids)
      ? active.report.summary.selected_taxids.map((value) => Number(value))
      : [];
    const spec = await fetchComputedBarplotSpec(taxids, getCountViewModeSafe());
    if (!spec) {
      setStatus("No count matrix data is currently available for plotting.");
      return;
    }
    const popup = window.open("", "unicornCountMatrixBarplot", "popup=yes,width=1180,height=760,resizable=yes,scrollbars=yes");
    if (!popup) {
      setStatus("Could not open the barplot popup. Check whether your browser blocked popups for this page.");
      return;
    }
    const traces = Array.isArray(spec.traces)
      ? spec.traces.map((trace) => ({
        type: "bar",
        name: String(trace.name || ""),
        x: Array.isArray(spec.x) ? spec.x : [],
        y: Array.isArray(trace.y) ? trace.y.map((value) => Number(value || 0)) : [],
        marker: {
          color: trace.color || "#9cad9f",
        },
      }))
      : [];
    const layout = {
      title: String(spec.title || "Unicorn count matrix barplot"),
      barmode: "group",
      paper_bgcolor: "#ffffff",
      plot_bgcolor: "#ffffff",
      font: {
        family: "system-ui, sans-serif",
        size: 13,
        color: "#172026",
      },
      xaxis: {
        title: "Selected nodes",
        tickangle: -30,
        automargin: true,
      },
      yaxis: {
        title: `${getCountViewLabelSafe()} reads`,
        automargin: true,
        separatethousands: true,
      },
      legend: {
        orientation: "h",
        y: 1.12,
      },
      margin: {
        l: 72,
        r: 24,
        t: 88,
        b: 140,
      },
    };
    const config = {
      responsive: true,
      displaylogo: false,
    };
    const tracesJson = JSON.stringify(traces).replace(/<\/script/gi, "<\\/script");
    const layoutJson = JSON.stringify(layout).replace(/<\/script/gi, "<\\/script");
    const configJson = JSON.stringify(config).replace(/<\/script/gi, "<\\/script");
    popup.document.open();
    popup.document.write(`<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Unicorn Count Matrix Barplot</title>
    <script src="https://cdn.plot.ly/plotly-3.6.0.min.js" charset="utf-8"></script>
    <style>
      html, body {
        margin: 0;
        padding: 0;
        width: 100%;
        height: 100%;
        background: #f4f0e8;
        color: #172026;
        font-family: system-ui, sans-serif;
      }
      #plot {
        width: 100%;
        height: 100vh;
      }
    </style>
  </head>
  <body>
    <div id="plot"></div>
    <script>
      const traces = ${tracesJson};
      const layout = ${layoutJson};
      const config = ${configJson};
      Plotly.newPlot("plot", traces, layout, config);
    </script>
  </body>
</html>`);
    popup.document.close();
    popup.focus();
    setStatus("Opened count matrix barplot in a popup window.");
  }

  async function openCurrentCountMatrixPcoa() {
    const active = state.remote.currentReport;
    if (!active || active.type !== "subtree") {
      setStatus("Open a count matrix first, then use PCoA.");
      return;
    }
    if (getCountViewModeSafe() === "both") {
      setStatus("PCoA is available only in Direct or Cumulative view. Choose one count view first.");
      return;
    }
    const taxids = Array.isArray(active.report?.summary?.selected_taxids)
      ? active.report.summary.selected_taxids.map((value) => Number(value))
      : [];
    const metric = String(els.pcoaMetric?.value || "jaccard");
    const spec = await fetchComputedPcoaSpec(taxids, getCountViewModeSafe(), metric);
    if (!spec) {
      setStatus("No count matrix data is currently available for PCoA.");
      return;
    }
    const popup = window.open("", "unicornCountMatrixPcoa", "popup=yes,width=1180,height=760,resizable=yes,scrollbars=yes");
    if (!popup) {
      setStatus("Could not open the PCoA popup. Check whether your browser blocked popups for this page.");
      return;
    }
    const points = Array.isArray(spec.points) ? spec.points : [];
    const axes = Array.isArray(spec.axes) ? spec.axes : [];
    const pc1 = axes[0] || { id: "PC1", explained_fraction: 0 };
    const pc2 = axes[1] || { id: "PC2", explained_fraction: 0 };
    const trace = {
      type: "scatter",
      mode: "markers+text",
      textposition: "top center",
      textfont: {
        family: "system-ui, sans-serif",
        size: 11,
        color: "#172026",
      },
      x: points.map((point) => Number(point.x || 0)),
      y: points.map((point) => Number(point.y || 0)),
      text: points.map((point) => String(point.label || point.dataset || "")),
      hovertemplate: [
        "<b>%{text}</b>",
        `${pc1.id}: %{x:.5f}`,
        `${pc2.id}: %{y:.5f}`,
        `<extra>${String(spec.distance_metric || "").replaceAll("_", "-")}</extra>`,
      ].join("<br>"),
      marker: {
        size: 14,
        color: points.map((point) => point.color || "#9cad9f"),
        line: {
          color: "#172026",
          width: 1,
        },
      },
    };
    const diagnostics = spec.diagnostics && typeof spec.diagnostics === "object" ? spec.diagnostics : {};
    const correctionText = diagnostics.correction_applied
      ? `Correction: ${diagnostics.correction_applied} (c=${Number(diagnostics.correction_constant || 0).toPrecision(4)})`
      : diagnostics.correction_required
        ? "Correction still required"
        : "No correction needed";
    const layout = {
      title: String(spec.title || "Unicorn count matrix PCoA"),
      paper_bgcolor: "#ffffff",
      plot_bgcolor: "#ffffff",
      font: {
        family: "system-ui, sans-serif",
        size: 13,
        color: "#172026",
      },
      xaxis: {
        title: `${pc1.id} (${(Number(pc1.explained_fraction || 0) * 100).toFixed(2)}%)`,
        zeroline: true,
        automargin: true,
      },
      yaxis: {
        title: `${pc2.id} (${(Number(pc2.explained_fraction || 0) * 100).toFixed(2)}%)`,
        zeroline: true,
        automargin: true,
      },
      annotations: [
        {
          xref: "paper",
          yref: "paper",
          x: 0,
          y: 1.12,
          xanchor: "left",
          yanchor: "bottom",
          showarrow: false,
          font: {
            family: "system-ui, sans-serif",
            size: 11,
            color: "#5b6570",
          },
          text: correctionText,
        },
      ],
      margin: {
        l: 72,
        r: 32,
        t: 104,
        b: 72,
      },
    };
    const config = {
      responsive: true,
      displaylogo: false,
    };
    const traceJson = JSON.stringify(trace).replace(/<\/script/gi, "<\\/script");
    const layoutJson = JSON.stringify(layout).replace(/<\/script/gi, "<\\/script");
    const configJson = JSON.stringify(config).replace(/<\/script/gi, "<\\/script");
    popup.document.open();
    popup.document.write(`<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Unicorn Count Matrix PCoA</title>
    <script src="https://cdn.plot.ly/plotly-3.6.0.min.js" charset="utf-8"></script>
    <style>
      html, body {
        margin: 0;
        padding: 0;
        width: 100%;
        height: 100%;
        background: #f4f0e8;
        color: #172026;
        font-family: system-ui, sans-serif;
      }
      #plot {
        width: 100%;
        height: 100vh;
      }
    </style>
  </head>
  <body>
    <div id="plot"></div>
    <script>
      const trace = ${traceJson};
      const layout = ${layoutJson};
      const config = ${configJson};
      Plotly.newPlot("plot", [trace], layout, config);
    </script>
  </body>
</html>`);
    popup.document.close();
    popup.focus();
    setStatus(`Opened count matrix PCoA (${metric.replaceAll("_", "-")}) in a popup window.`);
  }

  function exportCurrentSubtreeMatrix() {
    const active = state.remote.currentReport;
    if (!active) {
      setStatus("No report matrix is currently available to export.");
      return;
    }
    if (active.type === "rank") {
      exportCurrentRankMatrix(active.report);
      return;
    }
    const report = active.report;
    const matrix = report?.matrix;
    const rows = Array.isArray(matrix?.rows) ? matrix.rows : [];
    const datasetNames = Array.isArray(matrix?.dataset_names) ? matrix.dataset_names : [];
    const countViewMode = getCountViewModeSafe();
    if (!report || !rows.length || !datasetNames.length) {
      setStatus("No count matrix is currently available to export.");
      return;
    }
    const header = ["taxid", "name", "rank"];
    if (countViewMode === "direct" || countViewMode === "both") {
      header.push("direct");
    }
    if (countViewMode === "subtree" || countViewMode === "both") {
      header.push("subtree");
    }
    datasetNames.forEach((datasetName) => {
      if (countViewMode === "direct" || countViewMode === "both") {
        header.push(countViewMode === "both" ? `${datasetName}_direct` : datasetName);
      }
      if (countViewMode === "subtree" || countViewMode === "both") {
        header.push(countViewMode === "both" ? `${datasetName}_subtree` : datasetName);
      }
    });
    const lines = [header.join("\t")];
    for (const row of rows) {
      const values = [
        String(row.taxid ?? ""),
        String(row.name ?? ""),
        String(row.rank ?? ""),
      ];
      if (countViewMode === "direct" || countViewMode === "both") {
        values.push(String(Number(row.direct || 0)));
      }
      if (countViewMode === "subtree" || countViewMode === "both") {
        values.push(String(Number(row.subtree || 0)));
      }
      datasetNames.forEach((datasetName, index) => {
        const entry = Array.isArray(row.datasets) ? row.datasets[index] : null;
        if (countViewMode === "direct" || countViewMode === "both") {
          values.push(String(Number(entry?.direct || 0)));
        }
        if (countViewMode === "subtree" || countViewMode === "both") {
          values.push(String(Number(entry?.subtree || 0)));
        }
      });
      lines.push(values.join("\t"));
    }
    const blob = new Blob([lines.join("\n") + "\n"], { type: "text/tab-separated-values;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    const safeName = "selected_nodes";
    anchor.href = url;
    anchor.download = `${safeName}_${getCountViewModeSafe()}_matrix.tsv`;
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    URL.revokeObjectURL(url);
    setStatus(`Exported ${getCountViewLabelSafe()} count matrix for the current selection.`);
  }

  function exportCurrentRankMatrix(report) {
    const rows = Array.isArray(report?.rows) ? report.rows : [];
    const datasetNames = Array.isArray(report?.summary?.dataset_names) ? report.summary.dataset_names : [];
    if (!rows.length || !datasetNames.length) {
      setStatus("No rank count matrix is currently available to export.");
      return;
    }
    const header = ["rank", "node_count", "total_direct", ...datasetNames];
    const lines = [header.join("\t")];
    for (const row of rows) {
      const values = [
        String(row.rank ?? ""),
        String(Number(row.node_count || 0)),
        String(Number(row.direct || 0)),
        ...datasetNames.map((_, index) => {
          const entry = Array.isArray(row.datasets) ? row.datasets[index] : null;
          return String(Number(entry?.direct || 0));
        }),
      ];
      lines.push(values.join("\t"));
    }
    const blob = new Blob([lines.join("\n") + "\n"], { type: "text/tab-separated-values;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = "selected_nodes_rank_direct_matrix.tsv";
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    URL.revokeObjectURL(url);
    setStatus("Exported rank-by-dataset direct count matrix.");
  }

  async function renderBackendTopTable() {
    const requestId = ++state.remote.tableRequestId;
    els.topTable.innerHTML = `
      <tr>
        <td colspan="5">Loading server table...</td>
      </tr>
    `;
    try {
      const payload = await globalObject.fetchRemoteTableView({
        scope: "root",
        sort: "direct",
        limit: 40,
      });
      if (requestId !== state.remote.tableRequestId) return;
      state.remote.currentTable = {
        scope: String(payload.scope || "root"),
        target: payload.target && typeof payload.target === "object"
          ? {
            taxid: Number(payload.target.taxid || 0),
            name: String(payload.target.name || ""),
            rank: String(payload.target.rank || ""),
          }
          : null,
        row_count: Number(payload.row_count || 0),
        request_context: globalObject.normalizeProviderRequestContext?.(payload.request_context),
      };
      if (els.tablePanelTitle) {
        const targetName = state.remote.currentTable.target?.name;
        els.tablePanelTitle.textContent = targetName
          ? `Top direct placements: ${targetName}`
          : "Top direct placements";
      }
      const rows = Array.isArray(payload.rows) ? payload.rows : [];
      els.topTable.innerHTML = rows.map((row) => `
        <tr>
          <td>${Number(row.taxid).toLocaleString()}</td>
          <td>${escapeHtml(row.name || "")}</td>
          <td>${escapeHtml(row.rank || "NA")}</td>
          <td>${Number(row.direct || 0).toLocaleString()}</td>
          <td>${Number(row.subtree || 0).toLocaleString()}</td>
        </tr>
      `).join("");
      if (!rows.length) {
        els.topTable.innerHTML = `
          <tr>
            <td colspan="5">No rows passed the current server filters.</td>
          </tr>
        `;
      }
    } catch (error) {
      if (requestId !== state.remote.tableRequestId) return;
      els.topTable.innerHTML = `
        <tr>
          <td colspan="5">${escapeHtml(error.message || "Could not load server table.")}</td>
        </tr>
      `;
    }
  }

  function setStatus(message) {
    if (typeof globalObject.setStatus === "function") {
      globalObject.setStatus(message);
    }
  }

  namespace.reports = {
    renderTopTable,
    clearSubtreeReportView,
    openSelectedSubtreeReport,
    openSelectedRankReport,
    refreshCurrentReportIfNeeded,
    renderCurrentReportView,
    renderSubtreeReport,
    renderRankReport,
    renderRankMatrix,
    renderDatasetBreakdownTable,
    renderSubtreeMatrix,
    getDatasetColor,
    formatCountSummaryLabel,
    formatCountSummaryValue,
    formatMatrixCountCell,
    openCurrentCountMatrixBarplot,
    openCurrentCountMatrixPcoa,
    exportCurrentSubtreeMatrix,
    renderBackendTopTable,
  };

  Object.assign(globalObject, namespace.reports);
})(window);
