"use strict";

(function initUnicornGraphEngineState(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};

  if (namespace.state) {
    globalObject.state = namespace.state;
    globalObject.unicornGraphState = namespace.state;
    return;
  }

  namespace.state = {
    series: [],
    clientLog: [],
    agent: {
      history: [],
      runtimeProvider: "mock",
      transportMode: "backend",
      configuredProvider: "openai",
      configuredModel: "gpt-5",
      apiKey: "",
      baseUrl: "",
    },
    remote: {
      connected: false,
      datasets: [],
      selectedDatasets: new Set(),
      requestContext: null,
      backendNodesFile: "",
      backendNamesFile: "",
      expandedTaxids: new Set(),
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
  };

  globalObject.state = namespace.state;
  globalObject.unicornGraphState = namespace.state;
})(window);
