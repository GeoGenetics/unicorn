"use strict";

(function initUnicornGraphEngineBoot(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};

  function bootstrapGraphengine() {
    if (namespace.booted) return;
    if (typeof namespace.initialize !== "function") {
      throw new Error("Unicorn graphengine boot expected initialize() to be registered before boot.");
    }
    namespace.initialize();
    namespace.booted = true;
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", bootstrapGraphengine, { once: true });
  } else {
    bootstrapGraphengine();
  }
})(window);
