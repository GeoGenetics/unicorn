"use strict";

(function initUnicornGraphEngineState(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};

  function createTreeViewportState() {
    return {
      zoom: 1,
      minZoom: 0.25,
      maxZoom: 4,
      baseWidth: 0,
      baseHeight: 0,
    };
  }

  if (namespace.state) {
    namespace.state.treeViewport = namespace.state.treeViewport || createTreeViewportState();
    namespace.state.remote.activeExpandedTaxids = namespace.state.remote.activeExpandedTaxids || new Set();
    namespace.state.remote.treeRefreshGeneration = Number(namespace.state.remote.treeRefreshGeneration || 0);
    namespace.state.remote.taxonomyOnly = Boolean(namespace.state.remote.taxonomyOnly);
    globalObject.state = namespace.state;
    globalObject.unicornGraphState = namespace.state;
    return;
  }

  namespace.state = {
    series: [],
    clientLog: [],
    remote: {
      connected: false,
      datasets: [],
      selectedDatasets: new Set(),
      requestContext: null,
      backendNodesFile: "",
      backendNamesFile: "",
      metadata: null,
      metadataFieldSelection: [],
      metadataVisualizationMode: "none",
      metadataValueColors: {},
      // Requested expansion survives temporary filtering; active expansion is response state.
      expandedTaxids: new Set(),
      activeExpandedTaxids: new Set(),
      treeRefreshGeneration: 0,
      taxonomyOnly: false,
      serverTreeActive: false,
      totalReads: 0,
      directTaxa: 0,
      tooltipRequestId: 0,
      tableRequestId: 0,
      currentReport: null,
      currentTable: null,
    },
    tree: null,
    flat: [],
    selected: new Set(),
    missingTaxids: new Set(),
    centerOnNextRender: false,
    focusTaxid: null,
    clickTimer: null,
    tooltipTimer: null,
    tooltipNode: null,
    tooltipPoint: null,
    suppressClicksUntil: 0,
    minReadsActual: 0,
    treeViewport: createTreeViewportState(),
  };

  globalObject.state = namespace.state;
  globalObject.unicornGraphState = namespace.state;
})(window);
