"use strict";

const state = {
  nodes: new Map(),
  counts: new Map(),
  names: new Map(),
  tree: null,
  flat: [],
  collapsed: new Set(),
  missingTaxids: new Set(),
  centerOnNextRender: false,
  focusTaxid: null,
  clickTimer: null,
};

const els = {
  lcaFile: document.getElementById("lcaFile"),
  nodesFile: document.getElementById("nodesFile"),
  namesFile: document.getElementById("namesFile"),
  renderBtn: document.getElementById("renderBtn"),
  centerBtn: document.getElementById("centerBtn"),
  countMode: document.getElementById("countMode"),
  scaleMode: document.getElementById("scaleMode"),
  minReads: document.getElementById("minReads"),
  searchBox: document.getElementById("searchBox"),
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
  topTable: document.getElementById("topTable"),
};

els.renderBtn.addEventListener("click", loadAndRender);
els.centerBtn.addEventListener("click", () => {
  state.focusTaxid = null;
  centerRoot();
});
els.countMode.addEventListener("change", redraw);
els.scaleMode.addEventListener("change", redraw);
els.minReads.addEventListener("input", redraw);
els.searchBox.addEventListener("input", redraw);
initSummaryResize();

async function loadAndRender() {
  if (!els.lcaFile.files[0] || !els.nodesFile.files[0]) {
    setStatus("Choose an LCA output file and nodes.dmp.");
    return;
  }
  try {
    setStatus("Parsing input files...");
    state.collapsed.clear();
    state.missingTaxids.clear();

    const [lcaText, nodesText, namesText] = await Promise.all([
      readFile(els.lcaFile.files[0]),
      readFile(els.nodesFile.files[0]),
      els.namesFile.files[0] ? readFile(els.namesFile.files[0]) : Promise.resolve(""),
    ]);

    state.nodes = parseNodes(nodesText);
    const parsed = parseLcaOutput(lcaText);
    state.counts = parsed.counts;
    state.names = mergeNames(parsed.names, parseNames(namesText));
    state.tree = buildTree(state.nodes, state.counts, state.names);
    state.centerOnNextRender = true;

    setStatus(`Loaded ${state.nodes.size.toLocaleString()} taxonomy nodes and ${state.counts.size.toLocaleString()} LCA taxa.`);
    redraw();
  } catch (error) {
    console.error(error);
    setStatus(`Could not render tree: ${error.message || error}`);
  }
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

function buildTree(nodes, counts, names) {
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
      total: 0,
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
      total: unknown,
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
    total: 0,
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
  for (const child of node.children) {
    computeTotals(child, depth + 1);
    node.total += child.total;
  }
}

function sortTree(node) {
  node.children.sort((a, b) => b.total - a.total || a.name.localeCompare(b.name));
  for (const child of node.children) sortTree(child);
}

function redraw() {
  if (!state.tree) return;
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
  setStatus(`Rendered ${visible.length.toLocaleString()} visible nodes from ${state.counts.size.toLocaleString()} LCA taxa.`);
}

function collectVisible(node, parent, nodes, links, leaves, minReads) {
  if (node !== state.tree && node.total < minReads && node.direct === 0) return false;
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
    const group = svgEl("g", {
      class: `node${match ? " match" : ""}${dim ? " dim" : ""}`,
      transform: `translate(${node.x}, ${node.y})`,
    });
    const radius = radiusFor(value, maxValue);
    group.appendChild(svgEl("circle", {
      r: radius,
      fill: fillFor(node),
    }));
    group.appendChild(svgEl("text", {
      x: radius + 8,
      y: -5,
    }, labelFor(node)));
    group.appendChild(svgEl("text", {
      class: "count",
      x: radius + 8,
      y: 10,
    }, `${node.direct.toLocaleString()} direct / ${node.total.toLocaleString()} subtree`));
    group.addEventListener("click", () => {
      if (state.clickTimer) clearTimeout(state.clickTimer);
      state.clickTimer = setTimeout(() => {
        state.clickTimer = null;
        toggleCollapse(node);
      }, 220);
    });
    group.addEventListener("dblclick", (event) => {
      event.preventDefault();
      if (state.clickTimer) {
        clearTimeout(state.clickTimer);
        state.clickTimer = null;
      }
      toggleFocus(node);
    });
    group.addEventListener("mousemove", (event) => showTooltip(event, node));
    group.addEventListener("mouseleave", hideTooltip);
    nodeLayer.appendChild(group);
  }
  els.svg.appendChild(nodeLayer);
  if (state.centerOnNextRender) {
    requestCenterRoot();
    state.centerOnNextRender = false;
  }
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

function showTooltip(event, node) {
  els.tooltip.hidden = false;
  els.tooltip.style.left = `${event.clientX + 14}px`;
  els.tooltip.style.top = `${event.clientY + 14}px`;
  els.tooltip.innerHTML = `
    <strong>${escapeHtml(node.name)}</strong>
    taxid: ${node.taxid}<br>
    rank: ${escapeHtml(node.rank || "NA")}<br>
    direct reads: ${node.direct.toLocaleString()}<br>
    subtree reads: ${node.total.toLocaleString()}<br>
    children: ${node.children.length.toLocaleString()}
  `;
}

function hideTooltip() {
  els.tooltip.hidden = true;
}

function renderSummary(visible) {
  const totalReads = state.tree ? state.tree.total : 0;
  const directTaxa = Array.from(state.counts.values()).filter((v) => v > 0).length;
  els.readCount.textContent = totalReads.toLocaleString();
  els.taxonCount.textContent = directTaxa.toLocaleString();
  els.visibleCount.textContent = visible.length.toLocaleString();
  els.missingCount.textContent = state.missingTaxids.size.toLocaleString();
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

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}
