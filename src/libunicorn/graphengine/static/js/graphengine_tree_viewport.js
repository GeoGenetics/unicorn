"use strict";

(function initUnicornGraphEngineTreeViewport(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const state = namespace.state;
  const els = namespace.els;

  if (!state || !els) {
    throw new Error("Unicorn graphengine tree viewport expected state and DOM modules to load first.");
  }

  const viewport = {
    zoom: 1,
    minZoom: 0.25,
    maxZoom: 4,
    baseWidth: 0,
    baseHeight: 0,
  };
  let hideTooltip = () => {};
  let initialized = false;

  function setTooltipHider(callback) {
    hideTooltip = typeof callback === "function" ? callback : () => {};
  }

  function setBaseDimensions(width, height) {
    viewport.baseWidth = Math.max(0, Number(width) || 0);
    viewport.baseHeight = Math.max(0, Number(height) || 0);
  }

  function getState() {
    return { ...viewport };
  }

  function init() {
    if (initialized || !els.chartWrap) return;
    initialized = true;

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

  namespace.treeViewport = {
    init,
    setTooltipHider,
    setBaseDimensions,
    getState,
  };
})(window);
