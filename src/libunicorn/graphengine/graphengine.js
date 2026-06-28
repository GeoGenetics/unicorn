"use strict";

(function initUnicornGraphEngineCompatibilityShell(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};

  if (!namespace.state || !namespace.els || !namespace.core || !namespace.treeModel || !namespace.backend || !namespace.treeRender || !namespace.ui || !namespace.selection || !namespace.reports || !namespace.agent) {
    throw new Error("Unicorn graphengine compatibility shell expected every browser-side module to load before graphengine.js.");
  }

  // Pass 8B compatibility shim:
  // semantic ownership now lives in dedicated modules; this file remains only
  // as a transitional loader target until Pass 8C removes or freezes it.
})(window);
