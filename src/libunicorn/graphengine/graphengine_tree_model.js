"use strict";

(function initUnicornGraphEngineTreeModel(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const state = namespace.state;

  if (!state) {
    throw new Error("Unicorn graphengine tree model expected state module to load first.");
  }

  function buildRemoteTree(node) {
    const builtChildren = Array.isArray(node.children) ? node.children.map(buildRemoteTree) : [];
    return {
      taxid: Number(node.taxid),
      parent: node.parent == null ? null : Number(node.parent),
      rank: node.rank || "no rank",
      name: node.name || String(node.taxid),
      direct: Number(node.direct || 0),
      directBySource: Array.isArray(node.direct_by_source)
        ? node.direct_by_source.map((value) => Number(value || 0))
        : [],
      total: Number(node.total || 0),
      totalBySource: Array.isArray(node.total_by_source)
        ? node.total_by_source.map((value) => Number(value || 0))
        : [],
      childCount: typeof node.child_count === "number"
        ? Number(node.child_count || 0)
        : builtChildren.length,
      hasChildren: typeof node.has_children === "boolean"
        ? Boolean(node.has_children)
        : builtChildren.length > 0,
      expanded: Boolean(node.expanded),
      children: builtChildren,
      depth: Number(node.depth || 0),
    };
  }

  function collectVisible(node, parent, nodes, links, leaves, minReads) {
    if (node !== state.tree && node.total <= 0) return false;
    if (node !== state.tree && node.total < minReads) return false;
    nodes.push(node);
    if (parent) links.push([parent, node]);
    let visibleChildren = 0;
    for (const child of node.children) {
      if (collectVisible(child, node, nodes, links, leaves, minReads)) visibleChildren++;
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
      if (children.length === 0) {
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

  function walkTree(node, visit) {
    visit(node);
    for (const child of node.children) {
      walkTree(child, visit);
    }
  }

  function findNodeByTaxid(node, taxid) {
    if (!node) return null;
    if (node.taxid === taxid) return node;
    for (const child of node.children || []) {
      const found = findNodeByTaxid(child, taxid);
      if (found) return found;
    }
    return null;
  }

  function collectExpandableTaxids(node, expandedTaxids) {
    if (nodeHasChildren(node)) {
      expandedTaxids.add(node.taxid);
    }
    for (const child of node.children || []) {
      collectExpandableTaxids(child, expandedTaxids);
    }
  }

  function nodeHasChildren(node) {
    if (typeof node.hasChildren === "boolean") return node.hasChildren;
    if (typeof node.childCount === "number") return node.childCount > 0;
    return Array.isArray(node.children) && node.children.length > 0;
  }

  function getVisibleChildCount(node) {
    const children = Array.isArray(node.children) ? node.children : [];
    return children.filter((child) => Number(child.total || 0) > 0).length;
  }

  namespace.treeModel = {
    buildRemoteTree,
    collectVisible,
    layoutVisible,
    walkTree,
    findNodeByTaxid,
    collectExpandableTaxids,
    nodeHasChildren,
    getVisibleChildCount,
  };

  Object.assign(globalObject, namespace.treeModel);
})(window);
