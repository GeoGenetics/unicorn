"use strict";

(function initUnicornGraphEngineSelection(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const state = namespace.state;
  const els = namespace.els;
  const core = namespace.core;
  const treeModel = namespace.treeModel;

  if (!state || !els || !core || !treeModel) {
    throw new Error("Unicorn graphengine selection expected state, DOM, core, and tree model modules to load first.");
  }

  const {
    RANK_DISPLAY_ORDER,
    escapeHtml,
  } = core;

  const {
    walkTree,
    findNodeByTaxid,
    collectExpandableTaxids,
    nodeHasChildren,
  } = treeModel;

  // Public ownership during modularization transition:
  // selected nodes, focused node, descendant expansion, and select-to-rank
  // flows should be implemented here and consumed via exported helpers.

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

  function getSingleSelectedNode() {
    const selectedNodes = getSelectedNodes();
    return selectedNodes.length === 1 ? selectedNodes[0] : null;
  }

  function hasSelection() {
    return state.selected.size > 0;
  }

  function getFocusedNode() {
    if (!state.tree || state.focusTaxid == null) return null;
    return findNodeByTaxid(state.tree, state.focusTaxid) || null;
  }

  function rankSortKey(rank) {
    const normalized = String(rank || "no rank").trim().toLowerCase();
    const index = RANK_DISPLAY_ORDER.indexOf(normalized);
    return index >= 0 ? index : RANK_DISPLAY_ORDER.length + normalized.charCodeAt(0);
  }

  function updateSelectToRankOptions() {
    if (!els.selectToRankValue) return;
    const previousValue = String(els.selectToRankValue.value || "");
    const rankSet = new Set();
    for (const node of state.flat) {
      const rank = String(node.rank || "no rank").trim();
      if (!rank) continue;
      rankSet.add(rank);
    }
    const ranks = Array.from(rankSet).sort((a, b) => {
      const aKey = rankSortKey(a);
      const bKey = rankSortKey(b);
      if (aKey !== bKey) return aKey - bKey;
      return a.localeCompare(b);
    });
    els.selectToRankValue.innerHTML = [
      `<option value="">Choose rank</option>`,
      ...ranks.map((rank) => `<option value="${escapeHtml(rank)}">${escapeHtml(rank)}</option>`),
    ].join("");
    if (ranks.includes(previousValue)) {
      els.selectToRankValue.value = previousValue;
    }
  }

  function toggleFocus(node) {
    if (state.focusTaxid === node.taxid) {
      state.focusTaxid = null;
      globalObject.centerRoot();
      return;
    }
    state.focusTaxid = node.taxid;
    globalObject.centerNode(node);
  }

  function toggleSelection(node) {
    if (state.selected.has(node.taxid)) state.selected.delete(node.taxid);
    else state.selected.add(node.taxid);
    globalObject.redraw();
  }

  function clearSelection() {
    if (!state.selected.size) {
      setStatus("No nodes are currently selected.");
      return;
    }
    state.selected.clear();
    globalObject.clearSubtreeReportView();
    globalObject.redraw();
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
    globalObject.redraw();
  }

  function selectToRank() {
    if (!state.selected.size) {
      setStatus("Select one or more nodes first, then use Select To Rank.");
      return;
    }
    const targetRank = String(els.selectToRankValue?.value || "").trim();
    if (!targetRank) {
      setStatus("Choose a target rank first, then use Select To Rank.");
      return;
    }
    const selectedNodes = getSelectedNodes();
    if (!selectedNodes.length) {
      setStatus("The current selection could not be resolved in the active tree.");
      return;
    }
    const nextSelection = new Set();
    for (const node of selectedNodes) {
      addDescendantsAtRankToSelection(node, targetRank, nextSelection);
    }
    if (!nextSelection.size) {
      setStatus(`No visible descendant nodes at rank ${targetRank} were found below the current selection.`);
      return;
    }
    state.selected = nextSelection;
    globalObject.redraw();
    setStatus(`Selected ${nextSelection.size.toLocaleString()} visible node${nextSelection.size === 1 ? "" : "s"} at rank ${targetRank}.`);
  }

  function addDescendantsAtRankToSelection(node, targetRank, selection) {
    for (const child of node.children || []) {
      if (String(child.rank || "").trim() === targetRank) {
        selection.add(child.taxid);
      }
      addDescendantsAtRankToSelection(child, targetRank, selection);
    }
  }

  function addDescendantsToSelection(node) {
    for (const child of node.children || []) {
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

    expandSelectedNodes(selectedNodes).catch((error) => {
      setStatus(`Could not expand the selected backend nodes: ${error.message || error}`);
    });
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

    uncollapseSelectedToTipsRemote(selectedNodes);
  }

  async function expandSelectedNodes(selectedNodes) {
    const expandedTaxids = new Set(state.remote.expandedTaxids);
    for (const node of selectedNodes) {
      if (nodeHasChildren(node)) {
        expandedTaxids.add(node.taxid);
      }
    }
    const visiblePayload = await globalObject.fetchRemoteVisibleTree({
      expandedTaxids: Array.from(expandedTaxids),
    });
    globalObject.applyRemoteVisiblePayload(visiblePayload);
    globalObject.redraw();
  }

  async function uncollapseSelectedToTipsRemote(selectedNodes) {
    try {
      const modelPayload = await globalObject.fetchRemoteFullTreeModel();
      const fullTree = modelPayload?.tree ? globalObject.buildRemoteTree(modelPayload.tree) : null;
      if (!fullTree) {
        setStatus("Could not load the full backend tree for Uncollapse Tips.");
        return;
      }
      const expandedTaxids = new Set(state.remote.expandedTaxids);
      for (const node of selectedNodes) {
        const target = findNodeByTaxid(fullTree, node.taxid);
        if (target) {
          collectExpandableTaxids(target, expandedTaxids);
        }
      }
      const visiblePayload = await globalObject.fetchRemoteVisibleTree({
        expandedTaxids: Array.from(expandedTaxids),
      });
      globalObject.applyRemoteVisiblePayload(visiblePayload);
      globalObject.redraw();
    } catch (error) {
      setStatus(`Could not uncollapse selected nodes to tips on the backend: ${error.message || error}`);
    }
  }

  function setStatus(message) {
    if (typeof globalObject.setStatus === "function") {
      globalObject.setStatus(message);
    }
  }

  namespace.selection = {
    getSelectedNodes,
    getSingleSelectedNode,
    hasSelection,
    getFocusedNode,
    toggleSelection,
    clearSelection,
    toggleFocus,
    selectDescendants,
    uncollapseSelected,
    uncollapseSelectedToTips,
    selectToRank,
    updateSelectToRankOptions,
  };

  Object.assign(globalObject, namespace.selection);
})(window);
