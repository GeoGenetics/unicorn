"use strict";

const state = {
  nodes: new Map(),
  counts: new Map(),
  names: new Map(),
  series: [],
  remote: {
    user: "",
    host: "",
    connected: false,
    datasets: [],
    selectedDatasets: new Set(),
  },
  tree: null,
  flat: [],
  collapsed: new Set(),
  selected: new Set(),
  missingTaxids: new Set(),
  centerOnNextRender: false,
  focusTaxid: null,
  clickTimer: null,
  tooltipTimer: null,
  tooltipNode: null,
  tooltipPoint: null,
  suppressClicksUntil: 0,
};

const els = {
  app: document.getElementById("app"),
  sidebar: document.getElementById("sidebar"),
  controls: document.getElementById("controls"),
  sidebarToggle: document.getElementById("sidebarToggle"),
  toggleRemoteSection: document.getElementById("toggleRemoteSection"),
  remoteSectionBody: document.getElementById("remoteSectionBody"),
  toggleFilesSection: document.getElementById("toggleFilesSection"),
  filesSectionBody: document.getElementById("filesSectionBody"),
  toggleOptionsSection: document.getElementById("toggleOptionsSection"),
  optionsSectionBody: document.getElementById("optionsSectionBody"),
  uncollapseBtn: document.getElementById("uncollapseBtn"),
  uncollapseTipsBtn: document.getElementById("uncollapseTipsBtn"),
  selectDescendantsBtn: document.getElementById("selectDescendantsBtn"),
  clearSelectionBtn: document.getElementById("clearSelectionBtn"),
  lcaInputs: document.getElementById("lcaInputs"),
  lcaListFile: document.getElementById("lcaListFile"),
  addLcaBtn: document.getElementById("addLcaBtn"),
  clearLcaListBtn: document.getElementById("clearLcaListBtn"),
  remoteUser: document.getElementById("remoteUser"),
  remoteHost: document.getElementById("remoteHost"),
  connectBtn: document.getElementById("connectBtn"),
  uploadBtn: document.getElementById("uploadBtn"),
  copyTunnelBtn: document.getElementById("copyTunnelBtn"),
  connectionState: document.getElementById("connectionState"),
  tunnelCommand: document.getElementById("tunnelCommand"),
  tunnelHint: document.getElementById("tunnelHint"),
  remoteDatasetsPanel: document.getElementById("remoteDatasetsPanel"),
  remoteRefreshBtn: document.getElementById("remoteRefreshBtn"),
  remoteSelectAllBtn: document.getElementById("remoteSelectAllBtn"),
  remoteClearAllBtn: document.getElementById("remoteClearAllBtn"),
  remoteDatasetsMeta: document.getElementById("remoteDatasetsMeta"),
  remoteDatasetsList: document.getElementById("remoteDatasetsList"),
  nodesFile: document.getElementById("nodesFile"),
  namesFile: document.getElementById("namesFile"),
  renderBtn: document.getElementById("renderBtn"),
  centerBtn: document.getElementById("centerBtn"),
  toggleTableBtn: document.getElementById("toggleTableBtn"),
  countMode: document.getElementById("countMode"),
  scaleMode: document.getElementById("scaleMode"),
  minReads: document.getElementById("minReads"),
  searchBox: document.getElementById("searchBox"),
  controlsResize: document.getElementById("controlsResize"),
  summaryPanel: document.getElementById("summaryPanel"),
  summaryResize: document.getElementById("summaryResize"),
  status: document.getElementById("status"),
  chartWrap: document.getElementById("chartWrap"),
  svg: document.getElementById("treeSvg"),
  tooltip: document.getElementById("tooltip"),
  readCount: document.getElementById("readCount"),
  taxonCount: document.getElementById("taxonCount"),
  visibleCount: document.getElementById("visibleCount"),
  missingCount: document.getElementById("missingCount"),
  selectedCount: document.getElementById("selectedCount"),
  tablePanel: document.getElementById("tablePanel"),
  sourceLegend: document.getElementById("sourceLegend"),
  topTable: document.getElementById("topTable"),
};

els.renderBtn.addEventListener("click", loadAndRender);
els.addLcaBtn.addEventListener("click", addLcaInput);
els.clearLcaListBtn.addEventListener("click", clearLcaListFile);
els.connectBtn.addEventListener("click", connectRemote);
els.uploadBtn.addEventListener("click", uploadLoadedFiles);
els.copyTunnelBtn.addEventListener("click", copyTunnelCommand);
els.remoteRefreshBtn.addEventListener("click", async () => {
  try {
    await refreshRemoteDatasets();
    const count = state.remote.datasets.length;
    setStatus(`Remote dataset list refreshed. ${count.toLocaleString()} file${count === 1 ? "" : "s"} available on the backend.`);
  } catch (error) {
    setStatus(`Could not refresh remote datasets: ${error.message || error}`);
  }
});
els.remoteSelectAllBtn.addEventListener("click", selectAllRemoteDatasets);
els.remoteClearAllBtn.addEventListener("click", clearRemoteDatasets);
els.remoteUser.addEventListener("input", updateTunnelHint);
els.remoteHost.addEventListener("input", updateTunnelHint);
els.centerBtn.addEventListener("click", () => {
  state.focusTaxid = null;
  centerRoot();
});
els.toggleTableBtn.addEventListener("click", toggleTablePanel);
els.countMode.addEventListener("change", redraw);
els.scaleMode.addEventListener("change", redraw);
els.minReads.addEventListener("input", redraw);
els.searchBox.addEventListener("input", redraw);
els.sidebarToggle.addEventListener("click", toggleSidebar);
els.uncollapseBtn.addEventListener("click", uncollapseSelected);
els.uncollapseTipsBtn.addEventListener("click", uncollapseSelectedToTips);
els.selectDescendantsBtn.addEventListener("click", selectDescendants);
els.clearSelectionBtn.addEventListener("click", clearSelection);
initControlsResize();
initSummaryResize();
initTablePanel();
initSidebarPanel();
initChartPan();
initRemotePanel();

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

function initRemotePanel() {
  const savedUser = localStorage.getItem("unicorn.remoteUser") || "";
  const savedHost = localStorage.getItem("unicorn.remoteHost") || "";
  els.remoteUser.value = savedUser;
  els.remoteHost.value = savedHost;
  state.remote.user = savedUser;
  state.remote.host = savedHost;
  updateTunnelHint();
  updateConnectionState(false, "Not connected");
  renderRemoteDatasets();
}

function initSidebarPanel() {
  const savedCollapsed = localStorage.getItem("unicorn.sidebarCollapsed") === "true";
  setSidebarCollapsed(savedCollapsed);
  initSectionToggle("remoteSection", els.toggleRemoteSection, els.remoteSectionBody);
  initSectionToggle("filesSection", els.toggleFilesSection, els.filesSectionBody);
  initSectionToggle("optionsSection", els.toggleOptionsSection, els.optionsSectionBody);
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
      centerNode(state.focusTaxid
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

async function connectRemote() {
  const user = els.remoteUser.value.trim();
  const host = els.remoteHost.value.trim();
  state.remote.user = user;
  state.remote.host = host;
  localStorage.setItem("unicorn.remoteUser", user);
  localStorage.setItem("unicorn.remoteHost", host);
  updateTunnelHint();

  if (!user || !host) {
    updateConnectionState(false, "Enter username and host");
    setStatus("Enter a remote username and host, open the SSH tunnel, then test the connection.");
    return;
  }

  updateConnectionState(false, "Connecting...");
  setStatus(`Testing local tunnel endpoint for ${user}@${host}...`);
  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 2500);
    const response = await fetch("http://localhost:8000/ping", {
      method: "GET",
      signal: controller.signal,
    });
    clearTimeout(timeoutId);
    if (!response.ok) {
      throw new Error(`Ping returned HTTP ${response.status}`);
    }
    updateConnectionState(true, "Connected");
    try {
      await refreshRemoteDatasets({ selectAll: true });
      setStatus(`Tunnel check succeeded for ${user}@${host}. Remote HTTP endpoint is reachable and remote datasets are ready to render.`);
    } catch (error) {
      setStatus(`Tunnel check succeeded for ${user}@${host}, but the remote dataset list could not be loaded yet. ${error.message || error}`);
    }
  } catch (error) {
    updateConnectionState(false, "Tunnel check failed");
    state.remote.datasets = [];
    state.remote.selectedDatasets.clear();
    renderRemoteDatasets();
    setStatus(`Tunnel check failed for ${user}@${host}. Make sure the SSH tunnel is open and the remote HTTP server is running on port 8000.`);
  }
}

function updateTunnelHint() {
  const user = els.remoteUser.value.trim() || "youruser";
  const host = els.remoteHost.value.trim() || "remote-server";
  const command = `ssh -L 8000:localhost:8000 ${user}@${host}`;
  els.tunnelCommand.textContent = command;
  els.tunnelHint.innerHTML = `Open the SSH tunnel first, then use <strong>Test Tunnel</strong> to check whether the remote HTTP endpoint is reachable.`;
}

function updateConnectionState(connected, message) {
  state.remote.connected = connected;
  els.connectionState.textContent = message;
  els.connectionState.classList.toggle("online", connected);
  els.connectionState.classList.toggle("offline", !connected);
  els.remoteDatasetsPanel.hidden = !connected;
}

async function uploadLoadedFiles() {
  const user = els.remoteUser.value.trim();
  const host = els.remoteHost.value.trim();
  const localFiles = Array.from(document.querySelectorAll(".lca-file-input"))
    .map((input) => input.files[0])
    .filter(Boolean);

  if (!user || !host) {
    setStatus("Enter a remote username and host before uploading files.");
    return;
  }
  if (!localFiles.length) {
    setStatus("No local .bdamage/LCA files are currently selected for upload.");
    return;
  }
  if (!state.remote.connected) {
    setStatus(`Tunnel has not been confirmed for ${user}@${host}. Test the tunnel first, then upload.`);
    return;
  }

  els.uploadBtn.disabled = true;
  setStatus(`Uploading ${localFiles.length.toLocaleString()} local file(s) to ${user}@${host} through the tunnel...`);
  let uploaded = 0;
  try {
    for (const file of localFiles) {
      const form = new FormData();
      form.append("file", file, file.name);
      const response = await fetch("http://localhost:8000/upload", {
        method: "POST",
        body: form,
      });
      if (!response.ok) {
        throw new Error(`Upload failed for ${file.name} with HTTP ${response.status}`);
      }
      uploaded++;
    }
    setStatus(`Uploaded ${uploaded.toLocaleString()} file(s) to the remote server for ${user}@${host}.`);
    await refreshRemoteDatasets();
  } catch (error) {
    setStatus(`Upload stopped after ${uploaded.toLocaleString()} file(s). ${error.message || error}`);
  } finally {
    els.uploadBtn.disabled = false;
  }
}

async function copyTunnelCommand() {
  const command = els.tunnelCommand.textContent;
  try {
    await navigator.clipboard.writeText(command);
    setStatus("Copied SSH tunnel command to clipboard.");
  } catch (error) {
    setStatus("Could not copy tunnel command automatically. You can still copy it manually.");
  }
}

async function loadAndRender() {
  if (!els.nodesFile.files[0]) {
    setStatus("Choose nodes.dmp before rendering.");
    return;
  }
  try {
    setStatus(state.remote.connected ? "Fetching remote datasets..." : "Parsing local input files...");
    state.collapsed.clear();
    state.missingTaxids.clear();

    const [parsedSources, nodesText, namesText] = await Promise.all([
      state.remote.connected ? loadRemoteSources() : loadLocalSources(),
      readFile(els.nodesFile.files[0]),
      els.namesFile.files[0] ? readFile(els.namesFile.files[0]) : Promise.resolve(""),
    ]);
    if (!parsedSources.length) {
      if (state.remote.connected) {
        setStatus(state.remote.datasets.length
          ? "No remote datasets are currently selected. Select one or more files in the Remote Datasets panel."
          : "No remote .bdamage datasets are available to render.");
      } else {
        setStatus("Choose at least one LCA output file or provide a file list.");
      }
      return;
    }

    state.nodes = parseNodes(nodesText);
    state.series = buildSeries(parsedSources);
    state.counts = aggregateSeriesCounts(state.series);
    state.names = mergeNames(mergeSourceNames(parsedSources), parseNames(namesText));
    state.tree = buildTree(state.nodes, state.series, state.names);
    state.centerOnNextRender = true;
    renderSourceLegend();

    setStatus(`Loaded ${state.nodes.size.toLocaleString()} taxonomy nodes, ${state.series.length.toLocaleString()} ${state.remote.connected ? "remote dataset" : "input file"}${state.series.length === 1 ? "" : "s"}, and ${state.counts.size.toLocaleString()} LCA taxa.`);
    redraw();
  } catch (error) {
    console.error(error);
    setStatus(`Could not render tree: ${error.message || error}`);
  }
}

async function loadLocalSources() {
  const uploads = Array.from(document.querySelectorAll(".lca-file-input"))
    .map((input) => input.files[0])
    .filter(Boolean);
  const uploadedSources = await Promise.all(
    uploads.map(async (file) => ({
      name: file.name,
      text: await readFile(file),
    })),
  );

  let listedSources = [];
  if (els.lcaListFile.files[0]) {
    const listText = await readFile(els.lcaListFile.files[0]);
    listedSources = await loadSourcesFromList(listText);
  }
  return [...uploadedSources, ...listedSources].map((source) => ({
    label: source.name,
    ...parseLcaOutput(source.text),
  }));
}

async function loadSourcesFromList(text) {
  const paths = text
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line && !line.startsWith("#"));
  return Promise.all(paths.map(async (path) => ({
    name: path.split(/[\\/]/).pop() || path,
    text: await fetchTextPath(path),
  })));
}

async function fetchTextPath(path) {
  const target = new URL(path, window.location.href);
  const response = await fetch(target.href);
  if (!response.ok) {
    throw new Error(`Could not load input file from ${path}`);
  }
  return response.text();
}

async function loadRemoteSources() {
  const files = getSelectedRemoteDatasets();
  if (!files.length) return [];

  const url = new URL("http://localhost:8000/render-data");
  for (const file of files) {
    url.searchParams.append("files", file);
  }

  const response = await fetch(url.toString(), {
    method: "GET",
  });
  if (!response.ok) {
    throw new Error(`Remote render-data request failed with HTTP ${response.status}`);
  }
  const payload = await response.json();
  const datasets = Array.isArray(payload.datasets) ? payload.datasets : [];
  return datasets.map((dataset) => ({
    label: dataset.filename || dataset.id || "remote-dataset",
    counts: countsArrayToMap(dataset.counts || []),
    names: countsArrayToNames(dataset.counts || []),
  }));
}

async function refreshRemoteDatasets(options = {}) {
  if (!state.remote.connected) {
    renderRemoteDatasets();
    return;
  }

  const { selectAll = false } = options;
  const response = await fetch("http://localhost:8000/datasets", {
    method: "GET",
  });
  if (!response.ok) {
    throw new Error(`Remote datasets request failed with HTTP ${response.status}`);
  }

  const payload = await response.json();
  const datasets = Array.isArray(payload.datasets) ? payload.datasets : [];
  const previous = new Set(state.remote.selectedDatasets);
  const previouslyAllSelected = state.remote.datasets.length > 0
    && previous.size === state.remote.datasets.length;

  state.remote.datasets = datasets.map((dataset) => ({
    id: dataset.id || dataset.filename || "",
    filename: dataset.filename || dataset.id || "remote-dataset",
    bytes: Number(dataset.bytes || 0),
    modified_at: dataset.modified_at || "",
  }));

  if (selectAll || !previous.size || previouslyAllSelected) {
    state.remote.selectedDatasets = new Set(state.remote.datasets.map((dataset) => dataset.filename));
  } else {
    state.remote.selectedDatasets = new Set(
      state.remote.datasets
        .map((dataset) => dataset.filename)
        .filter((filename) => previous.has(filename)),
    );
  }

  renderRemoteDatasets();
}

function renderRemoteDatasets() {
  els.remoteDatasetsList.innerHTML = "";

  if (!state.remote.connected) {
    els.remoteDatasetsMeta.textContent = "Connect to browse remote files";
    return;
  }

  const datasets = state.remote.datasets;
  const selected = state.remote.selectedDatasets;

  if (!datasets.length) {
    els.remoteDatasetsMeta.textContent = "0 files available";
    const empty = document.createElement("div");
    empty.className = "remote-datasets-empty";
    empty.textContent = "No .bdamage datasets found in uploads on the backend.";
    els.remoteDatasetsList.appendChild(empty);
    return;
  }

  els.remoteDatasetsMeta.textContent =
    `${selected.size.toLocaleString()} of ${datasets.length.toLocaleString()} file${datasets.length === 1 ? "" : "s"} selected`;

  for (const dataset of datasets) {
    const item = document.createElement("div");
    item.className = "remote-dataset-item";

    const label = document.createElement("label");
    const checkbox = document.createElement("input");
    checkbox.type = "checkbox";
    checkbox.checked = selected.has(dataset.filename);
    checkbox.addEventListener("change", () => {
      if (checkbox.checked) state.remote.selectedDatasets.add(dataset.filename);
      else state.remote.selectedDatasets.delete(dataset.filename);
      renderRemoteDatasets();
    });

    const copy = document.createElement("div");
    copy.className = "remote-dataset-copy";

    const name = document.createElement("div");
    name.className = "remote-dataset-name";
    name.textContent = dataset.filename;

    const detail = document.createElement("div");
    detail.className = "remote-dataset-detail";
    detail.textContent = formatRemoteDatasetDetail(dataset);

    copy.appendChild(name);
    copy.appendChild(detail);
    label.appendChild(checkbox);
    label.appendChild(copy);
    item.appendChild(label);
    els.remoteDatasetsList.appendChild(item);
  }
}

function selectAllRemoteDatasets() {
  state.remote.selectedDatasets = new Set(
    state.remote.datasets.map((dataset) => dataset.filename),
  );
  renderRemoteDatasets();
}

function clearRemoteDatasets() {
  state.remote.selectedDatasets.clear();
  renderRemoteDatasets();
}

function getSelectedRemoteDatasets() {
  return state.remote.datasets
    .map((dataset) => dataset.filename)
    .filter((filename) => state.remote.selectedDatasets.has(filename));
}

function formatRemoteDatasetDetail(dataset) {
  const parts = [];
  if (dataset.bytes > 0) parts.push(formatBytes(dataset.bytes));
  if (dataset.modified_at) {
    const parsed = new Date(dataset.modified_at);
    parts.push(Number.isNaN(parsed.getTime()) ? dataset.modified_at : parsed.toLocaleString());
  }
  return parts.join(" \u2022 ") || "ready";
}

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

function readFile(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result || ""));
    reader.onerror = () => reject(reader.error);
    reader.readAsText(file);
  });
}

function parseNodes(text) {
  const map = new Map();
  for (const line of text.split(/\r?\n/)) {
    if (!line.trim()) continue;
    const parts = line.split("|").map((p) => p.trim());
    const taxid = Number(parts[0]);
    const parent = Number(parts[1]);
    if (!Number.isFinite(taxid) || !Number.isFinite(parent)) continue;
    map.set(taxid, {
      taxid,
      parent,
      rank: parts[2] || "no rank",
    });
  }
  return map;
}

function parseNames(text) {
  const map = new Map();
  if (!text) return map;
  for (const line of text.split(/\r?\n/)) {
    if (!line.trim()) continue;
    const parts = line.split("|").map((p) => p.trim());
    const taxid = Number(parts[0]);
    const name = parts[1] || "";
    const cls = parts[3] || "";
    if (!Number.isFinite(taxid) || !name) continue;
    if (cls === "scientific name" || !map.has(taxid)) map.set(taxid, name);
  }
  return map;
}

function parseLcaOutput(text) {
  const queryCounts = new Map();
  const summaryCounts = new Map();
  const names = new Map();
  let inSummary = false;
  let sawSummary = false;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line) continue;
    if (line.startsWith("#")) {
      inSummary = /taxid/i.test(line) && /count/i.test(line);
      continue;
    }
    const parts = line.split("\t");
    if (parts.length < 2) continue;

    const first = Number(parts[0]);
    const second = Number(parts[1]);
    let taxid = 0;
    let count = 1;
    let name = "";

    if (Number.isFinite(first) && Number.isFinite(second) && inSummary) {
      taxid = first;
      count = second;
      name = cleanName(parts.slice(2).join("\t"));
      sawSummary = true;
    } else if (Number.isFinite(first) && Number.isFinite(second) && parts.length >= 3) {
      taxid = first;
      count = second;
      name = cleanName(parts.slice(2).join("\t"));
    } else if (Number.isFinite(second)) {
      taxid = second;
      name = cleanName(parts.slice(2).join("\t"));
    }

    if (!Number.isFinite(taxid)) continue;
    const target = inSummary ? summaryCounts : queryCounts;
    target.set(taxid, (target.get(taxid) || 0) + count);
    if (name && name !== "NA") names.set(taxid, name);
  }
  const counts = sawSummary ? summaryCounts : queryCounts;
  return { counts, names };
}

function cleanName(name) {
  return name.replace(/^"+|"+$/g, "").trim();
}

function mergeNames(a, b) {
  const out = new Map(b);
  for (const [taxid, name] of a) out.set(taxid, name);
  return out;
}

function mergeSourceNames(series) {
  const names = new Map();
  for (const source of series) {
    for (const [taxid, name] of source.names) names.set(taxid, name);
  }
  return names;
}

function buildSeries(parsedSources) {
  return parsedSources.map((source, index) => ({
    label: source.label,
    color: colorForSource(index),
    counts: source.counts,
    visible: true,
  }));
}

function countsArrayToMap(rows) {
  const counts = new Map();
  for (const row of rows) {
    const taxid = Number(row.taxid);
    const count = Number(row.count);
    if (!Number.isFinite(taxid) || !Number.isFinite(count)) continue;
    counts.set(taxid, (counts.get(taxid) || 0) + count);
  }
  return counts;
}

function countsArrayToNames(rows) {
  const names = new Map();
  for (const row of rows) {
    const taxid = Number(row.taxid);
    const name = typeof row.name === "string" ? cleanName(row.name) : "";
    if (!Number.isFinite(taxid) || !name || name === "NA") continue;
    names.set(taxid, name);
  }
  return names;
}

function aggregateSeriesCounts(series) {
  const total = new Map();
  for (const source of series) {
    for (const [taxid, count] of source.counts) {
      total.set(taxid, (total.get(taxid) || 0) + count);
    }
  }
  return total;
}

function buildTree(nodes, series, names) {
  const counts = aggregateSeriesCounts(series);
  const included = new Set();
  for (const [taxid, count] of counts) {
    if (!count || taxid === 0) continue;
    if (!nodes.has(taxid)) {
      state.missingTaxids.add(taxid);
      continue;
    }
    let cur = taxid;
    const seen = new Set();
    while (nodes.has(cur) && !seen.has(cur)) {
      seen.add(cur);
      included.add(cur);
      const parent = nodes.get(cur).parent;
      if (!parent || parent === cur) break;
      cur = parent;
    }
  }

  const objects = new Map();
  for (const taxid of included) {
    const raw = nodes.get(taxid);
    objects.set(taxid, {
      taxid,
      parent: raw.parent,
      rank: raw.rank,
      name: names.get(taxid) || String(taxid),
      direct: counts.get(taxid) || 0,
      directBySource: series.map((source) => source.counts.get(taxid) || 0),
      total: 0,
      totalBySource: new Array(series.length).fill(0),
      children: [],
      depth: 0,
    });
  }

  const roots = [];
  for (const node of objects.values()) {
    const parent = objects.get(node.parent);
    if (parent && parent.taxid !== node.taxid) parent.children.push(node);
    else roots.push(node);
  }

  const unknown = counts.get(0) || 0;
  if (unknown) {
    const unclassified = {
      taxid: 0,
      parent: null,
      rank: "unclassified",
      name: names.get(0) || "unclassified",
      direct: unknown,
      directBySource: series.map((source) => source.counts.get(0) || 0),
      total: unknown,
      totalBySource: series.map((source) => source.counts.get(0) || 0),
      children: [],
      depth: 0,
    };
    if (roots.length === 1) roots[0].children.push(unclassified);
    else roots.push(unclassified);
  }

  const root = roots.length === 1 ? roots[0] : {
    taxid: -1,
    parent: null,
    rank: "synthetic root",
    name: "root",
    direct: 0,
    directBySource: new Array(series.length).fill(0),
    total: 0,
    totalBySource: new Array(series.length).fill(0),
    children: roots,
    depth: 0,
  };

  computeTotals(root, 0);
  sortTree(root);
  return root;
}

function computeTotals(node, depth) {
  node.depth = depth;
  node.total = node.direct;
  node.totalBySource = [...node.directBySource];
  for (const child of node.children) {
    computeTotals(child, depth + 1);
    node.total += child.total;
    for (let i = 0; i < node.totalBySource.length; i++) {
      node.totalBySource[i] += child.totalBySource[i];
    }
  }
}

function sortTree(node) {
  node.children.sort((a, b) => b.total - a.total || a.name.localeCompare(b.name));
  for (const child of node.children) sortTree(child);
}

function redraw() {
  if (!state.tree) return;
  applySeriesVisibility(state.tree);
  state.counts = aggregateSeriesCounts(getVisibleSeries());
  const minReads = Number(els.minReads.value || 0);
  const search = els.searchBox.value.trim().toLowerCase();
  const visible = [];
  const links = [];
  const leaves = { count: 0 };
  collectVisible(state.tree, null, visible, links, leaves, minReads);
  layoutVisible(state.tree, new Set(visible));
  state.flat = visible;
  renderSvg(visible, links, search);
  renderSummary(visible);
  renderTopTable();
  const activeSeries = getVisibleSeries().length;
  setStatus(`Rendered ${visible.length.toLocaleString()} visible nodes from ${state.counts.size.toLocaleString()} LCA taxa across ${activeSeries.toLocaleString()} active sample${activeSeries === 1 ? "" : "s"}.`);
}

function collectVisible(node, parent, nodes, links, leaves, minReads) {
  if (node !== state.tree && node.total <= 0) return false;
  // The minimum-read filter is defined on subtree totals, so any node below
  // the threshold is hidden and its nearest visible ancestor becomes the
  // rendered collapse point for that branch.
  if (node !== state.tree && node.total < minReads) return false;
  nodes.push(node);
  if (parent) links.push([parent, node]);
  const collapsed = state.collapsed.has(node.taxid);
  let visibleChildren = 0;
  if (!collapsed) {
    for (const child of node.children) {
      if (collectVisible(child, node, nodes, links, leaves, minReads)) visibleChildren++;
    }
  }
  if (visibleChildren === 0) {
    node._leaf = leaves.count++;
  }
  return true;
}

function layoutVisible(root, visibleSet) {
  const rowGap = 34;
  const levelGap = 230;
  const top = 42;
  const left = 42;
  const setY = (node) => {
    const children = node.children.filter((child) => visibleSet.has(child));
    if (state.collapsed.has(node.taxid) || children.length === 0) {
      node.x = left + node.depth * levelGap;
      node.y = top + (node._leaf || 0) * rowGap;
      return node.y;
    }
    const ys = children.map(setY);
    node.x = left + node.depth * levelGap;
    node.y = ys.reduce((sum, y) => sum + y, 0) / ys.length;
    return node.y;
  };
  setY(root);
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
  els.svg.innerHTML = "";

  const edgeLayer = svgEl("g", { class: "edges" });
  for (const [a, b] of links) {
    edgeLayer.appendChild(svgEl("path", {
      class: "edge",
      d: `M ${a.x} ${a.y} C ${a.x + 95} ${a.y}, ${b.x - 95} ${b.y}, ${b.x} ${b.y}`,
    }));
  }
  els.svg.appendChild(edgeLayer);

  const nodeLayer = svgEl("g", { class: "nodes" });
  for (const node of nodes) {
    const value = mode === "direct" ? node.direct : node.total;
    const match = search && (`${node.taxid} ${node.name}`.toLowerCase().includes(search));
    const dim = search && !match;
    const selected = state.selected.has(node.taxid);
    const group = svgEl("g", {
      class: `node${match ? " match" : ""}${dim ? " dim" : ""}${selected ? " selected" : ""}`,
      transform: `translate(${node.x}, ${node.y})`,
    });
    const radius = radiusFor(value, maxValue);
    group.appendChild(svgEl("circle", {
      class: "node-hit",
      r: Math.max(radius + 7, 12),
    }));
    renderNodePie(group, node, radius);
    group.appendChild(svgEl("text", {
      x: radius + 8,
      y: -5,
    }, labelFor(node)));
    group.appendChild(svgEl("text", {
      class: "count",
      x: radius + 8,
      y: 10,
    }, `${node.direct.toLocaleString()} direct / ${node.total.toLocaleString()} subtree`));
    group.addEventListener("click", (event) => {
      if (performance.now() < state.suppressClicksUntil) return;
      if (event.ctrlKey || event.metaKey) {
        if (state.clickTimer) {
          clearTimeout(state.clickTimer);
          state.clickTimer = null;
        }
        toggleSelection(node);
        return;
      }
      if (state.clickTimer) clearTimeout(state.clickTimer);
      state.clickTimer = setTimeout(() => {
        state.clickTimer = null;
        toggleCollapse(node);
      }, 220);
    });
    group.addEventListener("dblclick", (event) => {
      if (performance.now() < state.suppressClicksUntil) return;
      event.preventDefault();
      if (state.clickTimer) {
        clearTimeout(state.clickTimer);
        state.clickTimer = null;
      }
      toggleFocus(node);
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
    group.appendChild(svgEl("circle", {
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
    group.appendChild(svgEl("path", {
      d: pieSlicePath(radius, start, end),
      fill: state.series[i]?.color || fillFor(node),
    }));
    start = end;
    drawn += fraction;
  }
  group.appendChild(svgEl("circle", {
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

function toggleFocus(node) {
  if (state.focusTaxid === node.taxid) {
    state.focusTaxid = null;
    centerRoot();
    return;
  }
  state.focusTaxid = node.taxid;
  centerNode(node);
}

function toggleCollapse(node) {
  if (!node.children.length) return;
  if (state.collapsed.has(node.taxid)) {
    state.collapsed.delete(node.taxid);
    collapseChildren(node);
  } else {
    state.collapsed.add(node.taxid);
  }
  redraw();
}

function toggleSelection(node) {
  if (state.selected.has(node.taxid)) state.selected.delete(node.taxid);
  else state.selected.add(node.taxid);
  redraw();
}

function clearSelection() {
  if (!state.selected.size) {
    setStatus("No nodes are currently selected.");
    return;
  }
  state.selected.clear();
  redraw();
}

function selectDescendants() {
  if (!state.selected.size) {
    setStatus("Select one or more nodes first, then use Select Descendants.");
    return;
  }

  const selectedNodes = getSelectedNodes();
  if (!selectedNodes.length) {
    setStatus("The current selection could not be resolved in the active tree.");
    return;
  }

  for (const node of selectedNodes) {
    addDescendantsToSelection(node);
  }
  redraw();
}

function addDescendantsToSelection(node) {
  for (const child of node.children) {
    state.selected.add(child.taxid);
    addDescendantsToSelection(child);
  }
}

function uncollapseSelected() {
  if (!state.selected.size) {
    setStatus("Select one or more nodes first, then use Uncollapse.");
    return;
  }

  const selectedNodes = getSelectedNodes();
  if (!selectedNodes.length) {
    setStatus("The current selection could not be resolved in the active tree.");
    return;
  }

  for (const node of selectedNodes) {
    state.collapsed.delete(node.taxid);
    addDescendantsToSelection(node);
    collapseSubtreeBelow(node);
  }
  redraw();
}

function uncollapseSelectedToTips() {
  if (!state.selected.size) {
    setStatus("Select one or more nodes first, then use Uncollapse Tips.");
    return;
  }

  const selectedNodes = getSelectedNodes();
  if (!selectedNodes.length) {
    setStatus("The current selection could not be resolved in the active tree.");
    return;
  }

  for (const node of selectedNodes) {
    addDescendantsToSelection(node);
    uncollapseSubtree(node);
  }
  redraw();
}

function collapseSubtreeBelow(node) {
  for (const child of node.children) {
    if (child.children.length) state.collapsed.add(child.taxid);
    collapseSubtreeBelow(child);
  }
}

function uncollapseSubtree(node) {
  state.collapsed.delete(node.taxid);
  for (const child of node.children) {
    uncollapseSubtree(child);
  }
}

function getSelectedNodes() {
  if (!state.tree) return [];
  const nodes = [];
  const seen = new Set();
  walkTree(state.tree, (node) => {
    if (state.selected.has(node.taxid) && !seen.has(node.taxid)) {
      seen.add(node.taxid);
      nodes.push(node);
    }
  });
  return nodes;
}

function walkTree(node, visit) {
  visit(node);
  for (const child of node.children) {
    walkTree(child, visit);
  }
}

function collapseChildren(node) {
  for (const child of node.children) {
    if (child.children.length) state.collapsed.add(child.taxid);
  }
}

function radiusFor(value, maxValue) {
  if (!value) return 4;
  const t = els.scaleMode.value === "linear" ? value / maxValue : Math.sqrt(value / maxValue);
  return 4 + t * 24;
}

function fillFor(node) {
  if (node.taxid === 0) return "#6f7a80";
  if (node.direct > 0 && node.children.length > 0) return "#e2a44e";
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
  const mode = els.countMode.value === "direct" ? "direct reads" : "subtree reads";
  const breakdown = getNodeSeriesValues(node)
    .map((value, index) => ({ value, source: state.series[index] }))
    .filter((entry) => entry.value > 0)
    .sort((a, b) => b.value - a.value)
    .map((entry) => `
      <div class="tooltip-source">
        <span class="tooltip-swatch" style="background:${entry.source.color}"></span>
        <span>${escapeHtml(entry.source.label)}: ${entry.value.toLocaleString()}</span>
      </div>
    `)
    .join("");
  els.tooltip.hidden = false;
  positionTooltip(event);
  els.tooltip.innerHTML = `
    <strong>${escapeHtml(node.name)}</strong>
    taxid: ${node.taxid}<br>
    rank: ${escapeHtml(node.rank || "NA")}<br>
    direct reads: ${node.direct.toLocaleString()}<br>
    subtree reads: ${node.total.toLocaleString()}<br>
    children: ${node.children.length.toLocaleString()}<br>
    ${breakdown ? `<div class="tooltip-breakdown"><em>${mode}</em>${breakdown}</div>` : ""}
  `;
}

function hideTooltip() {
  if (state.tooltipTimer) {
    clearTimeout(state.tooltipTimer);
    state.tooltipTimer = null;
  }
  state.tooltipNode = null;
  state.tooltipPoint = null;
  els.tooltip.hidden = true;
}

function renderSummary(visible) {
  const totalReads = state.tree ? state.tree.total : 0;
  const directTaxa = Array.from(state.counts.values()).filter((v) => v > 0).length;
  els.readCount.textContent = totalReads.toLocaleString();
  els.taxonCount.textContent = directTaxa.toLocaleString();
  els.visibleCount.textContent = visible.length.toLocaleString();
  els.missingCount.textContent = state.missingTaxids.size.toLocaleString();
  els.selectedCount.textContent = state.selected.size.toLocaleString();
}

function renderTopTable() {
  const rows = state.flat
    .filter((node) => node.direct > 0)
    .sort((a, b) => b.direct - a.direct)
    .slice(0, 40);
  els.topTable.innerHTML = rows.map((node) => `
    <tr>
      <td>${node.taxid}</td>
      <td>${escapeHtml(node.name)}</td>
      <td>${escapeHtml(node.rank || "NA")}</td>
      <td>${node.direct.toLocaleString()}</td>
      <td>${node.total.toLocaleString()}</td>
    </tr>
  `).join("");
}

function svgEl(name, attrs = {}, text = "") {
  const el = document.createElementNS("http://www.w3.org/2000/svg", name);
  for (const [key, value] of Object.entries(attrs)) el.setAttribute(key, value);
  if (text) el.textContent = text;
  return el;
}

function setStatus(message) {
  els.status.textContent = message;
}

function initChartPan() {
  let pointerId = null;
  let startX = 0;
  let startY = 0;
  let startLeft = 0;
  let startTop = 0;
  let moved = false;

  els.chartWrap.addEventListener("pointerdown", (event) => {
    if (event.button !== 0) return;
    if (event.target.closest(".tooltip")) return;
    pointerId = event.pointerId;
    startX = event.clientX;
    startY = event.clientY;
    startLeft = els.chartWrap.scrollLeft;
    startTop = els.chartWrap.scrollTop;
    moved = false;
  });

  window.addEventListener("pointermove", (event) => {
    if (pointerId !== event.pointerId) return;
    const dx = event.clientX - startX;
    const dy = event.clientY - startY;
    if (!moved && (Math.abs(dx) > 3 || Math.abs(dy) > 3)) {
      moved = true;
      hideTooltip();
      els.chartWrap.classList.add("dragging");
      document.body.style.userSelect = "none";
    }
    if (!moved) return;
    els.chartWrap.scrollLeft = startLeft - dx;
    els.chartWrap.scrollTop = startTop - dy;
  });

  const stopPan = (event) => {
    if (pointerId !== event.pointerId) return;
    if (moved) state.suppressClicksUntil = performance.now() + 120;
    pointerId = null;
    moved = false;
    els.chartWrap.classList.remove("dragging");
    document.body.style.userSelect = "";
  };

  window.addEventListener("pointerup", stopPan);
  window.addEventListener("pointercancel", stopPan);
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
      if (state.tree) centerNode(state.focusTaxid ? state.flat.find((n) => n.taxid === state.focusTaxid) || state.tree : state.tree);
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
      if (state.tree) centerRoot();
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

function toggleTablePanel() {
  setTablePanelVisible(els.tablePanel.hidden);
  localStorage.setItem("unicorn.tableVisible", els.tablePanel.hidden ? "0" : "1");
  requestAnimationFrame(() => {
    if (state.tree) centerNode(state.focusTaxid ? state.flat.find((n) => n.taxid === state.focusTaxid) || state.tree : state.tree);
  });
}

function setTablePanelVisible(visible) {
  els.tablePanel.hidden = !visible;
  els.tablePanel.classList.toggle("hidden", !visible);
  els.toggleTableBtn.textContent = visible ? "Hide Counts" : "Show Counts";
  els.toggleTableBtn.setAttribute("aria-expanded", visible ? "true" : "false");
}

function addLcaInput() {
  const row = document.createElement("div");
  row.className = "file-row";
  row.innerHTML = `
    <input class="lca-file-input" type="file" accept=".txt,.tsv,.bdamage,.lca">
    <button class="secondary remove-file-btn" type="button" aria-label="Remove input file">Remove</button>
  `;
  row.querySelector(".remove-file-btn").addEventListener("click", () => {
    row.remove();
  });
  els.lcaInputs.appendChild(row);
}

function clearLcaListFile() {
  els.lcaListFile.value = "";
  setStatus("Cleared input file list selection.");
}

function colorForSource(index) {
  return SOURCE_COLORS[index % SOURCE_COLORS.length];
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
      <input class="legend-toggle" type="checkbox" ${source.visible ? "checked" : ""} aria-label="Toggle ${escapeHtml(source.label)}">
      <span class="legend-swatch" style="background:${source.color}"></span>
      <span class="legend-label" title="${escapeHtml(source.label)}">${escapeHtml(source.label)}</span>
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
      redraw();
    });
  });
}

function getVisibleSeries() {
  return state.series.filter((source) => source.visible);
}

function applySeriesVisibility(node) {
  node.direct = sumVisibleValues(node.directBySource);
  node.total = node.direct;
  for (const child of node.children) {
    applySeriesVisibility(child);
    node.total += child.total;
  }
}

function sumVisibleValues(values) {
  let total = 0;
  for (let i = 0; i < values.length; i++) {
    if (state.series[i]?.visible) total += values[i];
  }
  return total;
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}
