"use strict";

(function initUnicornGraphEngineDamage(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const state = namespace.state;
  const els = namespace.els;
  const core = namespace.core;

  if (!state || !els || !core) {
    throw new Error("Unicorn graphengine damage expected state, DOM, and core modules to load first.");
  }

  const {
    SOURCE_COLORS,
    escapeHtml,
  } = core;

  const MAX_SELECTED_DAMAGE_TAXIDS = 250;
  const MAX_SELECTED_DAMAGE_PROFILES = 5000;

  function getBackend() {
    return namespace.backend || null;
  }

  function getSelection() {
    return namespace.selection || null;
  }

  function setStatus(message) {
    if (typeof globalObject.setStatus === "function") {
      globalObject.setStatus(message);
    }
  }

  function addClientLog(level, stage, message, detail = "") {
    if (typeof globalObject.addClientLog === "function") {
      globalObject.addClientLog(level, stage, message, detail);
    }
  }

  function safeColor(value, fallback) {
    const color = String(value || "").trim();
    return /^(#[0-9a-f]{3,8}|[a-z]+)$/i.test(color) ? color : fallback;
  }

  function finiteNumber(value) {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : null;
  }

  function formatInteger(value) {
    const parsed = finiteNumber(value);
    return parsed == null ? "NA" : Math.round(parsed).toLocaleString();
  }

  function formatParameter(value) {
    const parsed = finiteNumber(value);
    if (parsed == null) return "NA";
    const absolute = Math.abs(parsed);
    if (absolute !== 0 && (absolute < 0.001 || absolute >= 10000)) {
      return parsed.toExponential(3);
    }
    return parsed.toLocaleString(undefined, {
      maximumFractionDigits: 5,
    });
  }

  function responseErrorMessage(payload, response) {
    if (typeof payload?.detail === "string") return payload.detail;
    if (typeof payload?.detail?.message === "string") return payload.detail.message;
    if (typeof payload?.message === "string") return payload.message;
    return `Remote damage request failed with HTTP ${response.status}`;
  }

  function resolvedDisplayMap(datasets) {
    const backend = getBackend();
    const resolved = typeof backend?.getSelectedResolvedMetadataDisplays === "function"
      ? backend.getSelectedResolvedMetadataDisplays()
      : [];
    const displayByFilename = new Map(
      (Array.isArray(resolved) ? resolved : []).map((entry) => [
        String(entry?.filename || ""),
        {
          filename: String(entry?.filename || ""),
          metadataField: entry?.metadata_field == null ? null : String(entry.metadata_field),
          metadataValue: entry?.metadata_value == null ? null : String(entry.metadata_value),
          color: entry?.color == null ? null : String(entry.color),
          label: entry?.label == null ? null : String(entry.label),
          missing: Boolean(entry?.missing),
        },
      ]).filter(([filename]) => filename),
    );

    return new Map((Array.isArray(datasets) ? datasets : []).map((dataset, index) => {
      const filename = String(dataset?.dataset || "");
      const existing = displayByFilename.get(filename);
      const series = state.series.find((entry) => entry?.label === filename);
      const fallbackColor = series?.color || SOURCE_COLORS[index % SOURCE_COLORS.length];
      return [
        filename,
        {
          filename,
          metadataField: existing?.metadataField || null,
          metadataValue: existing?.metadataValue || null,
          color: safeColor(existing?.color, fallbackColor),
          label: existing?.label || filename,
          missing: Boolean(existing?.missing),
        },
      ];
    }));
  }

  function buildDamageScopeRequest() {
    const backend = getBackend();
    const requestContext = backend?.getActiveBackendRequestContext?.();
    const files = requestContext?.dataset_names?.length
      ? requestContext.dataset_names.slice()
      : backend?.getSelectedRemoteDatasets?.() || [];
    if (!files.length) {
      throw new Error("Select at least one backend dataset before opening a damage profile.");
    }
    return {
      files,
      nodes_file: requestContext?.nodes_file || state.remote.backendNodesFile || null,
      names_file: requestContext?.names_file || state.remote.backendNamesFile || null,
    };
  }

  function buildDamageRequest(node) {
    return {
      ...buildDamageScopeRequest(),
      taxid: Number(node.taxid),
    };
  }

  function buildSelectedDamageRequest(nodes) {
    return {
      ...buildDamageScopeRequest(),
      taxids: nodes.map((node) => Number(node.taxid)),
    };
  }

  async function postDamageRequest(endpoint, payload) {
    const response = await fetch(`http://localhost:8000${endpoint}`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
      },
      body: JSON.stringify(payload),
    });
    const responsePayload = await response.json().catch(() => ({}));
    if (!response.ok) {
      throw new Error(responseErrorMessage(responsePayload, response));
    }
    return responsePayload;
  }

  async function fetchNodeDamage(node) {
    return postDamageRequest("/damage/node", buildDamageRequest(node));
  }

  async function fetchSelectedDamage(nodes) {
    return postDamageRequest(
      "/damage/selected",
      buildSelectedDamageRequest(nodes),
    );
  }

  function positionSeries(positions, valueKey, evidenceKey) {
    const points = (Array.isArray(positions) ? positions : [])
      .map((entry) => ({
        position: finiteNumber(entry?.position),
        value: finiteNumber(entry?.[valueKey]),
        evidence: finiteNumber(entry?.[evidenceKey]),
      }))
      .filter((entry) => entry.position != null && entry.value != null);
    return {
      x: points.map((entry) => entry.position + 1),
      y: points.map((entry) => entry.value),
      evidence: points.map((entry) => entry.evidence),
    };
  }

  function fittedSeries(positions, valueKey) {
    const points = (Array.isArray(positions) ? positions : [])
      .map((entry) => ({
        position: finiteNumber(entry?.position),
        value: finiteNumber(entry?.[valueKey]),
      }))
      .filter((entry) => entry.position != null && entry.value != null);
    return {
      x: points.map((entry) => entry.position + 1),
      y: points.map((entry) => entry.value),
    };
  }

  function observedTrace({
    display,
    dataset,
    endLabel,
    positions,
    valueKey,
    evidenceKey,
    symbol,
    showLegend,
  }) {
    const series = positionSeries(positions, valueKey, evidenceKey);
    if (!series.x.length) return null;
    return {
      type: "scatter",
      mode: "markers",
      name: display.label,
      legendgroup: display.filename,
      showlegend: showLegend,
      x: series.x,
      y: series.y,
      customdata: series.evidence.map((evidence) => [
        display.label,
        endLabel,
        evidence,
        dataset.profile_status,
      ]),
      marker: {
        color: display.color,
        size: 9,
        symbol,
        line: {
          color: "#fffdf7",
          width: 1,
        },
      },
      hovertemplate: [
        "<b>%{customdata[0]}</b>",
        `${endLabel} observed`,
        "Position %{x}",
        "Frequency %{y:.4f}",
        "Evidence depth %{customdata[2]:,.0f}",
        "Profile %{customdata[3]}",
        "<extra></extra>",
      ].join("<br>"),
    };
  }

  function fittedTrace({
    display,
    endLabel,
    positions,
    valueKey,
    dash,
  }) {
    const series = fittedSeries(positions, valueKey);
    if (!series.x.length) return null;
    return {
      type: "scatter",
      mode: "lines",
      name: `${display.label} ${endLabel} fit`,
      legendgroup: display.filename,
      showlegend: false,
      x: series.x,
      y: series.y,
      customdata: series.x.map(() => [display.label, endLabel]),
      line: {
        color: display.color,
        width: 2.5,
        dash,
      },
      hovertemplate: [
        "<b>%{customdata[0]}</b>",
        `${endLabel} fitted`,
        "Position %{x}",
        "Probability %{y:.4f}",
        "<extra></extra>",
      ].join("<br>"),
    };
  }

  function buildTraces(payload, displayByFilename) {
    const traces = [];
    for (const dataset of payload.datasets || []) {
      if (!dataset?.profile_present || !dataset.observed) continue;
      const filename = String(dataset.dataset || "");
      const display = displayByFilename.get(filename) || {
        filename,
        label: filename,
        color: SOURCE_COLORS[traces.length % SOURCE_COLORS.length],
      };
      const observedPositions = dataset.observed.positions || [];
      const fitPositions = dataset.fit?.positions || [];
      const ctTrace = observedTrace({
        display,
        dataset,
        endLabel: "5' C->T",
        positions: observedPositions,
        valueKey: "ct_frequency",
        evidenceKey: "n5",
        symbol: "circle",
        showLegend: true,
      });
      const gaTrace = observedTrace({
        display,
        dataset,
        endLabel: "3' G->A",
        positions: observedPositions,
        valueKey: "ga_frequency",
        evidenceKey: "n3",
        symbol: "diamond-open",
        showLegend: ctTrace == null,
      });
      if (ctTrace) traces.push(ctTrace);
      if (gaTrace) traces.push(gaTrace);
      if (dataset.fit_valid) {
        const ctFit = fittedTrace({
          display,
          endLabel: "5' C->T",
          positions: fitPositions,
          valueKey: "dx5",
          dash: "solid",
        });
        const gaFit = fittedTrace({
          display,
          endLabel: "3' G->A",
          positions: fitPositions,
          valueKey: "dx3",
          dash: "dash",
        });
        if (ctFit) traces.push(ctFit);
        if (gaFit) traces.push(gaFit);
      }
    }
    return traces;
  }

  function profileStatusLabel(dataset) {
    if (!dataset?.profile_present) return "Missing";
    return dataset.fit_valid ? "Valid fit" : "Invalid fit";
  }

  function profileStatusClass(dataset) {
    if (!dataset?.profile_present) return "missing";
    return dataset.fit_valid ? "valid" : "invalid";
  }

  function metadataDetail(display) {
    if (!display?.metadataField) return "";
    const value = display.missing ? "missing" : display.metadataValue;
    return `<span class="metadata-tag">${escapeHtml(display.metadataField)}: ${escapeHtml(value || "missing")}</span>`;
  }

  function parameterCell(label, value) {
    return `
      <div class="parameter">
        <span>${escapeHtml(label)}</span>
        <strong>${escapeHtml(formatParameter(value))}</strong>
      </div>
    `;
  }

  function datasetCard(dataset, display) {
    const status = profileStatusLabel(dataset);
    const statusClass = profileStatusClass(dataset);
    const fit = dataset?.fit || {};
    const warning = !dataset?.profile_present
      ? "No damage profile was reported for this taxon in this dataset."
      : dataset.fit_valid
        ? ""
        : `Observed evidence is shown, but fitted curves are hidden.${dataset.missing_fields?.length ? ` Missing or invalid: ${dataset.missing_fields.join(", ")}.` : ""}`;
    return `
      <article class="dataset-card" data-profile-status="${statusClass}">
        <header>
          <span class="swatch" style="background:${safeColor(display.color, "#687c72")}"></span>
          <div class="dataset-identity">
            <h3>${escapeHtml(display.label)}</h3>
            <p>${escapeHtml(dataset.dataset)}</p>
          </div>
          <span class="status ${statusClass}">${escapeHtml(status)}</span>
        </header>
        <div class="dataset-facts">
          <div><span>Direct assignments</span><strong>${escapeHtml(formatInteger(dataset.direct_count))}</strong></div>
          <div><span>Subtree assignments</span><strong>${escapeHtml(formatInteger(dataset.subtree_count))}</strong></div>
          ${metadataDetail(display)}
        </div>
        ${dataset.profile_present ? `
          <div class="parameters">
            ${parameterCell("A", fit.A)}
            ${parameterCell("q", fit.q)}
            ${parameterCell("c", fit.c)}
            ${parameterCell("phi", fit.phi)}
            ${parameterCell("Zfit", fit.zfit)}
            ${parameterCell("nll", fit.nll)}
          </div>
        ` : ""}
        ${warning ? `<p class="dataset-warning">${escapeHtml(warning)}</p>` : ""}
      </article>
    `;
  }

  function buildSummary(payload, displayByFilename) {
    const datasets = Array.isArray(payload.datasets) ? payload.datasets : [];
    const counts = datasets.reduce((summary, dataset) => {
      summary[profileStatusClass(dataset)] += 1;
      return summary;
    }, { valid: 0, invalid: 0, missing: 0 });
    const cards = datasets.map((dataset, index) => {
      const filename = String(dataset.dataset || "");
      const display = displayByFilename.get(filename) || {
        label: filename,
        color: SOURCE_COLORS[index % SOURCE_COLORS.length],
      };
      return datasetCard(dataset, display);
    }).join("");
    return {
      counts,
      html: `
        <section class="summary" id="damageSummary">
          <div class="scope-row">
            <span class="scope direct">Direct count: exact-LCA assignments</span>
            <span class="scope cumulative">Subtree count: taxid plus descendants</span>
            <span class="scope cumulative">Damage metrics: subtree evidence</span>
          </div>
          <p class="scope-warning">
            Each dataset profile includes both producer count scopes. Direct assignments build the tree; subtree assignments and damage evidence cover this taxid plus represented descendants.
          </p>
          <div class="status-summary">
            <span><strong>${counts.valid}</strong> valid</span>
            <span><strong>${counts.invalid}</strong> invalid</span>
            <span><strong>${counts.missing}</strong> missing</span>
          </div>
          <div class="dataset-grid">${cards}</div>
        </section>
      `,
    };
  }

  function payloadNodes(payload) {
    if (Array.isArray(payload?.nodes)) return payload.nodes;
    return payload?.taxid == null ? [] : [payload];
  }

  function payloadDatasets(payload) {
    const byFilename = new Map();
    for (const node of payloadNodes(payload)) {
      for (const dataset of node.datasets || []) {
        const filename = String(dataset?.dataset || "");
        if (filename && !byFilename.has(filename)) {
          byFilename.set(filename, dataset);
        }
      }
    }
    return Array.from(byFilename.values());
  }

  function tsvCell(value) {
    if (value == null) return "";
    return String(value).replace(/[\t\r\n]+/g, " ");
  }

  function buildDamageTsv(payload) {
    const displayByFilename = resolvedDisplayMap(payloadDatasets(payload));
    const header = [
      "taxid",
      "name",
      "dataset",
      "dataset_label",
      "metadata_field",
      "metadata_value",
      "direct_count",
      "direct_count_scope",
      "subtree_count",
      "subtree_count_scope",
      "damage_scope",
      "profile_status",
      "fit_valid",
      "missing_fields",
      "CTfreq",
      "GAfreq",
      "A",
      "q",
      "c",
      "phi",
      "Zfit",
      "fitCT0",
      "fitGA0",
      "nll",
      "position",
      "K5",
      "N5",
      "CT_position_frequency",
      "K3",
      "N3",
      "GA_position_frequency",
      "Dx5",
      "Dx3",
    ];
    const rows = [header];

    for (const node of payloadNodes(payload)) {
      for (const dataset of node.datasets || []) {
        const filename = String(dataset?.dataset || "");
        const display = displayByFilename.get(filename) || {
          label: filename,
          metadataField: null,
          metadataValue: null,
          missing: false,
        };
        const observedByPosition = new Map(
          (dataset.observed?.positions || []).map((position) => [
            Number(position.position),
            position,
          ]),
        );
        const fitByPosition = new Map(
          (dataset.fit?.positions || []).map((position) => [
            Number(position.position),
            position,
          ]),
        );
        const positions = Array.from(new Set([
          ...observedByPosition.keys(),
          ...fitByPosition.keys(),
        ])).filter(Number.isFinite).sort((left, right) => left - right);
        const exportPositions = positions.length ? positions : [null];

        for (const position of exportPositions) {
          const observed = position == null
            ? {}
            : observedByPosition.get(position) || {};
          const fitPosition = position == null
            ? {}
            : fitByPosition.get(position) || {};
          rows.push([
            node.taxid,
            node.name,
            filename,
            display.label,
            display.metadataField,
            display.missing ? "__missing__" : display.metadataValue,
            dataset.direct_count,
            node.direct_count_scope || payload.direct_count_scope,
            dataset.subtree_count,
            node.subtree_count_scope || payload.subtree_count_scope,
            node.damage_scope || payload.damage_scope,
            dataset.profile_status,
            dataset.fit_valid,
            (dataset.missing_fields || []).join(","),
            dataset.observed?.ct_frequency,
            dataset.observed?.ga_frequency,
            dataset.fit?.A,
            dataset.fit?.q,
            dataset.fit?.c,
            dataset.fit?.phi,
            dataset.fit?.zfit,
            dataset.fit?.fit_ct0,
            dataset.fit?.fit_ga0,
            dataset.fit?.nll,
            position,
            observed.k5,
            observed.n5,
            observed.ct_frequency,
            observed.k3,
            observed.n3,
            observed.ga_frequency,
            fitPosition.dx5,
            fitPosition.dx3,
          ]);
        }
      }
    }
    return rows
      .map((row) => row.map(tsvCell).join("\t"))
      .join("\n") + "\n";
  }

  function damageExportFilename(payload) {
    const nodes = payloadNodes(payload);
    if (nodes.length === 1) {
      return `unicorn_damage_taxid_${Number(nodes[0].taxid)}.tsv`;
    }
    return `unicorn_damage_selected_${nodes.length}_nodes.tsv`;
  }

  function downloadDamageTsv(payload, targetDocument = document) {
    const blob = new Blob([buildDamageTsv(payload)], {
      type: "text/tab-separated-values;charset=utf-8",
    });
    const url = URL.createObjectURL(blob);
    const link = targetDocument.createElement("a");
    link.href = url;
    link.download = damageExportFilename(payload);
    targetDocument.body.appendChild(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 0);
  }

  function attachExportButton(popup, payload) {
    popup.document.getElementById("exportDamageBtn")?.addEventListener(
      "click",
      () => downloadDamageTsv(payload, popup.document),
    );
  }

  function plotLayout(payload, traces) {
    return {
      title: {
        text: `${payload.name} (${payload.taxid})`,
        x: 0.02,
        xanchor: "left",
        font: {
          family: "Iowan Old Style, Georgia, serif",
          size: 23,
          color: "#173b36",
        },
      },
      paper_bgcolor: "#fffdf7",
      plot_bgcolor: "#fffdf7",
      font: {
        family: "Avenir Next, Gill Sans, sans-serif",
        size: 13,
        color: "#263b37",
      },
      hovermode: "closest",
      xaxis: {
        title: "Terminal position (1 = read end)",
        dtick: 1,
        automargin: true,
        gridcolor: "#e8e0d4",
        zeroline: false,
      },
      yaxis: {
        title: "Mismatch frequency",
        rangemode: "tozero",
        tickformat: ".1%",
        automargin: true,
        gridcolor: "#e8e0d4",
        zeroline: false,
      },
      legend: {
        title: { text: "Datasets / active metadata groups" },
        orientation: "h",
        y: 1.18,
        groupclick: "togglegroup",
      },
      annotations: [
        {
          text: "Circle + solid: 5' C->T &nbsp;&nbsp; Diamond + dashed: 3' G->A",
          xref: "paper",
          yref: "paper",
          x: 0,
          y: -0.2,
          xanchor: "left",
          showarrow: false,
          font: { size: 12, color: "#66756e" },
        },
        ...(traces.length ? [] : [{
          text: "No observed damage evidence is available for this node.",
          xref: "paper",
          yref: "paper",
          x: 0.5,
          y: 0.5,
          showarrow: false,
          font: { size: 17, color: "#8a5b45" },
        }]),
      ],
      margin: {
        l: 78,
        r: 34,
        t: 126,
        b: 100,
      },
    };
  }

  function openDamagePopup(payload) {
    const popup = window.open("", "unicornDamageProfile", "popup=yes,width=1280,height=860,resizable=yes,scrollbars=yes");
    if (!popup) {
      throw new Error("Could not open the damage popup. Check whether your browser blocked popups for this page.");
    }
    const displayByFilename = resolvedDisplayMap(payload.datasets);
    const traces = buildTraces(payload, displayByFilename);
    const summary = buildSummary(payload, displayByFilename);
    const layout = plotLayout(payload, traces);
    const config = {
      responsive: true,
      displaylogo: false,
      modeBarButtonsToRemove: ["lasso2d", "select2d"],
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
    <title>Unicorn Damage Profile</title>
    <script src="https://cdn.plot.ly/plotly-3.6.0.min.js" charset="utf-8"></script>
    <style>
      :root {
        --ink: #263b37;
        --forest: #173b36;
        --paper: #fffdf7;
        --sand: #f1e8da;
        --rust: #a34f31;
        --sage: #47796d;
      }
      * { box-sizing: border-box; }
      body {
        margin: 0;
        color: var(--ink);
        background:
          radial-gradient(circle at 10% 0%, rgba(71, 121, 109, 0.14), transparent 35%),
          linear-gradient(145deg, #f7f0e5, #efe5d5);
        font-family: "Avenir Next", "Gill Sans", sans-serif;
      }
      .page-head {
        padding: 28px 34px 18px;
        border-bottom: 1px solid rgba(23, 59, 54, 0.18);
      }
      .page-head-row {
        display: flex;
        justify-content: space-between;
        gap: 20px;
        align-items: end;
      }
      .eyebrow {
        margin: 0 0 5px;
        color: var(--rust);
        font-size: 12px;
        font-weight: 800;
        letter-spacing: 0.16em;
        text-transform: uppercase;
      }
      h1 {
        margin: 0;
        color: var(--forest);
        font-family: "Iowan Old Style", Georgia, serif;
        font-size: clamp(30px, 4vw, 48px);
        font-weight: 600;
      }
      .export-btn {
        flex: 0 0 auto;
        border: 0;
        border-radius: 999px;
        padding: 10px 16px;
        color: #fffdf7;
        background: var(--forest);
        font: 750 12px "Avenir Next", "Gill Sans", sans-serif;
        cursor: pointer;
      }
      .export-btn:hover { background: var(--sage); }
      .taxid { color: #75847e; font-size: 0.62em; }
      .scope-row, .status-summary {
        display: flex;
        flex-wrap: wrap;
        gap: 9px;
      }
      .scope, .status-summary span, .metadata-tag {
        display: inline-flex;
        align-items: center;
        border-radius: 999px;
        padding: 6px 10px;
        font-size: 12px;
        font-weight: 750;
      }
      .scope.direct { color: #70412d; background: #f5d8c8; }
      .scope.cumulative { color: #23584f; background: #cfe5dd; }
      .scope-warning {
        margin: 10px 0 14px;
        max-width: 850px;
        color: #5e6864;
        line-height: 1.45;
      }
      .status-summary span { background: rgba(255, 253, 247, 0.78); }
      .status-summary strong { margin-right: 5px; color: var(--forest); }
      .plot-shell {
        margin: 20px 24px;
        min-height: 500px;
        border: 1px solid rgba(23, 59, 54, 0.16);
        border-radius: 18px;
        overflow: hidden;
        background: var(--paper);
        box-shadow: 0 18px 50px rgba(63, 47, 31, 0.12);
      }
      #plot { width: 100%; height: 560px; }
      .summary { padding: 4px 24px 34px; }
      .dataset-grid {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(310px, 1fr));
        gap: 14px;
        margin-top: 18px;
      }
      .dataset-card {
        border: 1px solid rgba(23, 59, 54, 0.14);
        border-radius: 14px;
        padding: 16px;
        background: rgba(255, 253, 247, 0.9);
      }
      .dataset-card[data-profile-status="invalid"] { border-color: rgba(163, 79, 49, 0.45); }
      .dataset-card[data-profile-status="missing"] { opacity: 0.78; }
      .dataset-card header {
        display: grid;
        grid-template-columns: 12px 1fr auto;
        gap: 10px;
        align-items: start;
      }
      .swatch {
        width: 12px;
        height: 34px;
        border-radius: 6px;
      }
      .dataset-identity h3 {
        margin: 0;
        color: var(--forest);
        font-size: 15px;
        overflow-wrap: anywhere;
      }
      .dataset-identity p {
        margin: 3px 0 0;
        color: #76817d;
        font-size: 11px;
        overflow-wrap: anywhere;
      }
      .status {
        border-radius: 999px;
        padding: 5px 8px;
        font-size: 11px;
        font-weight: 800;
      }
      .status.valid { color: #23584f; background: #cfe5dd; }
      .status.invalid { color: #70412d; background: #f5d8c8; }
      .status.missing { color: #5f6562; background: #e6e3dc; }
      .dataset-facts {
        display: flex;
        flex-wrap: wrap;
        gap: 8px 14px;
        align-items: center;
        margin: 15px 0;
      }
      .dataset-facts div { display: flex; flex-direction: column; }
      .dataset-facts span { color: #76817d; font-size: 11px; }
      .dataset-facts strong { color: var(--forest); font-size: 20px; }
      .metadata-tag { color: #23584f !important; background: #e1eee9; }
      .parameters {
        display: grid;
        grid-template-columns: repeat(3, minmax(0, 1fr));
        gap: 6px;
      }
      .parameter {
        padding: 8px;
        border-radius: 8px;
        background: #f3eee5;
      }
      .parameter span {
        display: block;
        color: #7d827f;
        font-size: 10px;
        text-transform: uppercase;
      }
      .parameter strong { font-size: 13px; }
      .dataset-warning {
        margin: 12px 0 0;
        color: #8a4b34;
        font-size: 12px;
        line-height: 1.4;
      }
      @media (max-width: 700px) {
        .page-head { padding: 20px; }
        .plot-shell { margin: 14px 10px; }
        .summary { padding: 4px 10px 24px; }
        #plot { height: 480px; }
      }
    </style>
  </head>
  <body>
    <header class="page-head">
      <p class="eyebrow">Ancient DNA evidence</p>
      <div class="page-head-row">
        <h1>${escapeHtml(payload.name)} <span class="taxid">taxid ${escapeHtml(payload.taxid)}</span></h1>
        <button id="exportDamageBtn" class="export-btn" type="button">Export normalized TSV</button>
      </div>
    </header>
    <main>
      <div class="plot-shell"><div id="plot"></div></div>
      ${summary.html}
    </main>
    <script>
      const traces = ${tracesJson};
      const layout = ${layoutJson};
      const config = ${configJson};
      if (typeof Plotly === "undefined") {
        document.getElementById("plot").textContent = "Plotly could not be loaded.";
      } else {
        Plotly.newPlot("plot", traces, layout, config);
      }
    </script>
  </body>
</html>`);
    popup.document.close();
    attachExportButton(popup, payload);
    popup.focus();
    return summary.counts;
  }

  function multiNodeTableRows(payload, displayByFilename) {
    return payloadNodes(payload).flatMap((node) => (
      (node.datasets || []).map((dataset, datasetIndex) => {
        const filename = String(dataset.dataset || "");
        const display = displayByFilename.get(filename) || {
          label: filename,
          color: SOURCE_COLORS[datasetIndex % SOURCE_COLORS.length],
          metadataField: null,
          metadataValue: null,
          missing: false,
        };
        const fit = dataset.fit || {};
        return `
          <tr data-profile-status="${profileStatusClass(dataset)}">
            <td class="taxon-cell">
              <strong>${escapeHtml(node.name)}</strong>
              <span>taxid ${escapeHtml(node.taxid)}</span>
            </td>
            <td class="dataset-cell">
              <span class="table-swatch" style="background:${safeColor(display.color, "#687c72")}"></span>
              <div>
                <strong>${escapeHtml(display.label)}</strong>
                <span>${escapeHtml(filename)}</span>
              </div>
            </td>
            <td class="numeric">${escapeHtml(formatInteger(dataset.direct_count))}</td>
            <td class="numeric">${escapeHtml(formatInteger(dataset.subtree_count))}</td>
            <td>${escapeHtml(profileStatusLabel(dataset))}</td>
            <td class="numeric">${escapeHtml(formatParameter(dataset.observed?.ct_frequency))}</td>
            <td class="numeric">${escapeHtml(formatParameter(dataset.observed?.ga_frequency))}</td>
            <td class="numeric">${escapeHtml(formatParameter(fit.A))}</td>
            <td class="numeric">${escapeHtml(formatParameter(fit.q))}</td>
            <td class="numeric">${escapeHtml(formatParameter(fit.c))}</td>
            <td class="numeric">${escapeHtml(formatParameter(fit.phi))}</td>
            <td class="numeric">${escapeHtml(formatParameter(fit.zfit))}</td>
            <td class="numeric">${escapeHtml(formatParameter(fit.nll))}</td>
          </tr>
        `;
      })
    )).join("");
  }

  function openSelectedDamageTablePopup(payload) {
    const nodes = payloadNodes(payload);
    const popup = window.open("", "unicornSelectedDamage", "popup=yes,width=1480,height=860,resizable=yes,scrollbars=yes");
    if (!popup) {
      throw new Error("Could not open the selected-node damage table. Check whether your browser blocked popups for this page.");
    }
    const datasets = payloadDatasets(payload);
    const displayByFilename = resolvedDisplayMap(datasets);
    const rows = multiNodeTableRows(payload, displayByFilename);
    const profileCounts = nodes.flatMap((node) => node.datasets || [])
      .reduce((summary, dataset) => {
        summary[profileStatusClass(dataset)] += 1;
        return summary;
      }, { valid: 0, invalid: 0, missing: 0 });

    popup.document.open();
    popup.document.write(`<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Unicorn Selected Damage</title>
    <style>
      :root {
        --ink: #263b37;
        --forest: #173b36;
        --paper: #fffdf7;
        --sand: #f1e8da;
        --rust: #a34f31;
        --sage: #47796d;
      }
      * { box-sizing: border-box; }
      body {
        margin: 0;
        color: var(--ink);
        background:
          radial-gradient(circle at 8% 0%, rgba(71, 121, 109, 0.16), transparent 32%),
          linear-gradient(145deg, #f7f0e5, #efe5d5);
        font-family: "Avenir Next", "Gill Sans", sans-serif;
      }
      .page-head {
        display: flex;
        justify-content: space-between;
        align-items: end;
        gap: 24px;
        padding: 28px 32px 20px;
        border-bottom: 1px solid rgba(23, 59, 54, 0.18);
      }
      .eyebrow {
        margin: 0 0 5px;
        color: var(--rust);
        font-size: 12px;
        font-weight: 800;
        letter-spacing: 0.16em;
        text-transform: uppercase;
      }
      h1 {
        margin: 0;
        color: var(--forest);
        font-family: "Iowan Old Style", Georgia, serif;
        font-size: clamp(30px, 4vw, 46px);
        font-weight: 600;
      }
      .subtitle {
        margin: 7px 0 0;
        color: #65736e;
      }
      .export-btn {
        flex: 0 0 auto;
        border: 0;
        border-radius: 999px;
        padding: 11px 17px;
        color: var(--paper);
        background: var(--forest);
        font: 750 12px "Avenir Next", "Gill Sans", sans-serif;
        cursor: pointer;
      }
      .export-btn:hover { background: var(--sage); }
      .scope-shell {
        display: flex;
        flex-wrap: wrap;
        gap: 9px;
        padding: 18px 32px 0;
      }
      .badge {
        border-radius: 999px;
        padding: 6px 10px;
        font-size: 12px;
        font-weight: 750;
        background: rgba(255, 253, 247, 0.8);
      }
      .badge.direct { color: #70412d; background: #f5d8c8; }
      .badge.cumulative { color: #23584f; background: #cfe5dd; }
      .scope-note {
        margin: 10px 32px 18px;
        color: #5e6864;
        line-height: 1.45;
      }
      .table-shell {
        margin: 0 22px 28px;
        overflow: auto;
        max-height: calc(100vh - 230px);
        border: 1px solid rgba(23, 59, 54, 0.16);
        border-radius: 16px;
        background: var(--paper);
        box-shadow: 0 18px 50px rgba(63, 47, 31, 0.12);
      }
      table {
        width: 100%;
        border-collapse: collapse;
        font-size: 12px;
      }
      th {
        position: sticky;
        top: 0;
        z-index: 1;
        padding: 11px 10px;
        color: #f9f4eb;
        background: var(--forest);
        text-align: left;
        white-space: nowrap;
      }
      td {
        padding: 10px;
        border-bottom: 1px solid #e7dfd2;
        vertical-align: top;
      }
      tbody tr:hover { background: #f6f0e6; }
      tr[data-profile-status="invalid"] { background: #fff6f0; }
      tr[data-profile-status="missing"] { color: #787e7b; background: #f2efea; }
      .taxon-cell strong, .dataset-cell strong {
        display: block;
        color: var(--forest);
      }
      .taxon-cell span, .dataset-cell span {
        display: block;
        margin-top: 2px;
        color: #7a837f;
        font-size: 10px;
      }
      .dataset-cell {
        display: grid;
        grid-template-columns: 9px minmax(180px, 1fr);
        gap: 8px;
      }
      .table-swatch {
        width: 9px;
        height: 30px;
        border-radius: 5px;
      }
      .numeric {
        font-variant-numeric: tabular-nums;
        text-align: right;
      }
      @media (max-width: 700px) {
        .page-head { align-items: start; flex-direction: column; }
        .scope-shell, .scope-note { margin-left: 14px; margin-right: 14px; padding-left: 0; }
        .table-shell { margin: 0 8px 20px; }
      }
    </style>
  </head>
  <body>
    <header class="page-head">
      <div>
        <p class="eyebrow">Cross-taxon evidence</p>
        <h1>Selected-node damage</h1>
        <p class="subtitle">${nodes.length.toLocaleString()} selected nodes across ${datasets.length.toLocaleString()} datasets</p>
      </div>
      <button id="exportDamageBtn" class="export-btn" type="button">Export normalized TSV</button>
    </header>
    <main>
      <div class="scope-shell">
        <span class="badge direct">Direct count: exact-LCA assignments</span>
        <span class="badge cumulative">Subtree count: taxid plus descendants</span>
        <span class="badge cumulative">Damage metrics: subtree evidence</span>
        <span class="badge">${profileCounts.valid} valid</span>
        <span class="badge">${profileCounts.invalid} invalid</span>
        <span class="badge">${profileCounts.missing} missing</span>
      </div>
      <p class="scope-note">
        Each row keeps one taxon and dataset separate. Direct assignments build the tree. Subtree assignments and damage evidence cover that taxid plus represented descendants.
      </p>
      <div class="table-shell">
        <table id="damageSelectedTable">
          <thead>
            <tr>
              <th>Taxon</th>
              <th>Dataset / metadata label</th>
              <th>Direct assignments</th>
              <th>Subtree assignments</th>
              <th>Profile</th>
              <th>CTfreq</th>
              <th>GAfreq</th>
              <th>A</th>
              <th>q</th>
              <th>c</th>
              <th>phi</th>
              <th>Zfit</th>
              <th>nll</th>
            </tr>
          </thead>
          <tbody>${rows}</tbody>
        </table>
      </div>
    </main>
  </body>
</html>`);
    popup.document.close();
    attachExportButton(popup, payload);
    popup.focus();
    return {
      nodes: nodes.length,
      datasets: datasets.length,
      profiles: profileCounts,
    };
  }

  async function openSelectedDamage() {
    const backend = getBackend();
    const selectedNodes = getSelection()?.getSelectedNodes?.() || [];
    if (!backend?.hasBackendTree?.()) {
      setStatus("Render a backend tree before opening a damage profile.");
      return;
    }
    if (!selectedNodes.length) {
      setStatus("Select one or more nodes to inspect damage.");
      return;
    }
    if (selectedNodes.length > MAX_SELECTED_DAMAGE_TAXIDS) {
      setStatus(
        `Damage tables support up to ${MAX_SELECTED_DAMAGE_TAXIDS} selected nodes. Narrow the current ${selectedNodes.length.toLocaleString()}-node selection.`,
      );
      return;
    }
    const backendContext = backend?.getActiveBackendRequestContext?.();
    const datasetCount = backendContext?.dataset_names?.length
      || backend?.getSelectedRemoteDatasets?.()?.length
      || 0;
    const requestedProfiles = selectedNodes.length * datasetCount;
    if (requestedProfiles > MAX_SELECTED_DAMAGE_PROFILES) {
      setStatus(
        `The damage table would contain ${requestedProfiles.toLocaleString()} node-dataset profiles. Narrow the selected nodes or datasets below ${MAX_SELECTED_DAMAGE_PROFILES.toLocaleString()} profiles.`,
      );
      return;
    }

    const isSingleNode = selectedNodes.length === 1;
    const node = isSingleNode ? selectedNodes[0] : null;
    setStatus(
      isSingleNode
        ? `Loading damage profile for ${node.name} (${node.taxid})...`
        : `Loading damage table for ${selectedNodes.length.toLocaleString()} selected nodes...`,
    );
    addClientLog(
      "info",
      "damage",
      isSingleNode
        ? `Loading damage profile for taxid ${node.taxid}.`
        : `Loading damage table for ${selectedNodes.length.toLocaleString()} selected nodes.`,
      isSingleNode
        ? `Selected node: ${node.name}`
        : `Selected taxids: ${selectedNodes.map((entry) => entry.taxid).join(", ")}`,
    );
    if (els.damageBtn) els.damageBtn.disabled = true;
    try {
      const payload = isSingleNode
        ? await fetchNodeDamage(node)
        : await fetchSelectedDamage(selectedNodes);
      const result = isSingleNode
        ? openDamagePopup(payload)
        : openSelectedDamageTablePopup(payload);
      setStatus(
        isSingleNode
          ? `Opened damage profile for ${payload.name} (${payload.taxid}).`
          : `Opened damage table for ${result.nodes.toLocaleString()} selected nodes.`,
      );
      addClientLog(
        "success",
        "damage",
        isSingleNode
          ? `Opened damage profile for taxid ${payload.taxid}.`
          : `Opened damage table for ${result.nodes.toLocaleString()} selected nodes.`,
        isSingleNode
          ? `${result.valid} valid, ${result.invalid} invalid, ${result.missing} missing dataset profile(s).`
          : `${result.profiles.valid} valid, ${result.profiles.invalid} invalid, ${result.profiles.missing} missing node-dataset profile(s).`,
      );
    } catch (error) {
      const message = error?.message || String(error);
      setStatus(`Could not open damage profile: ${message}`);
      addClientLog("error", "damage", "Damage profile request failed.", message);
    } finally {
      backend?.syncBackendRuntimeUiState?.();
    }
  }

  namespace.damage = {
    openSelectedDamage,
    openSelectedNodeDamage: openSelectedDamage,
  };
})(window);
