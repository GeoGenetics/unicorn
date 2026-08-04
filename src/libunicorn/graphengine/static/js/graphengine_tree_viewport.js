"use strict";

(function initUnicornGraphEngineTreeViewport(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};
  const state = namespace.state;
  const els = namespace.els;

  if (!state || !state.treeViewport || !els) {
    throw new Error("Unicorn graphengine tree viewport expected state and DOM modules to load first.");
  }

  const viewport = state.treeViewport;
  const WHEEL_ZOOM_SENSITIVITY = 0.001;
  const WHEEL_LINE_PIXELS = 16;
  const BUTTON_ZOOM_FACTOR = 1.25;
  let hideTooltip = () => {};
  let initialized = false;
  let wheelFrame = null;
  let pendingWheelZoom = null;

  function setTooltipHider(callback) {
    hideTooltip = typeof callback === "function" ? callback : () => {};
  }

  function setBaseDimensions(width, height) {
    viewport.baseWidth = Math.max(0, Number(width) || 0);
    viewport.baseHeight = Math.max(0, Number(height) || 0);
    applyDisplayDimensions();
  }

  function applyDisplayDimensions() {
    if (!els.svg || !viewport.baseWidth || !viewport.baseHeight) {
      updateControls();
      return;
    }
    const zoom = clampZoom(viewport.zoom);
    viewport.zoom = zoom;
    els.svg.setAttribute("width", String(viewport.baseWidth * zoom));
    els.svg.setAttribute("height", String(viewport.baseHeight * zoom));
    updateControls();
  }

  function clampZoom(value) {
    return Math.min(
      viewport.maxZoom,
      Math.max(viewport.minZoom, Number(value) || 1),
    );
  }

  function normalizeWheelDelta(event) {
    if (event.deltaMode === WheelEvent.DOM_DELTA_LINE) {
      return event.deltaY * WHEEL_LINE_PIXELS;
    }
    if (event.deltaMode === WheelEvent.DOM_DELTA_PAGE) {
      return event.deltaY * Math.max(els.chartWrap.clientHeight, 1);
    }
    return event.deltaY;
  }

  function queueWheelZoom(event) {
    if (!viewport.baseWidth || !viewport.baseHeight || !els.svg) return;
    const delta = normalizeWheelDelta(event);
    if (!Number.isFinite(delta) || delta === 0) return;

    event.preventDefault();
    hideTooltip();

    const currentZoom = pendingWheelZoom?.zoom ?? viewport.zoom;
    const zoom = clampZoom(
      currentZoom * Math.exp(-delta * WHEEL_ZOOM_SENSITIVITY),
    );
    pendingWheelZoom = {
      zoom,
      clientX: event.clientX,
      clientY: event.clientY,
    };
    if (wheelFrame !== null) return;
    wheelFrame = requestAnimationFrame(applyQueuedWheelZoom);
  }

  function applyQueuedWheelZoom() {
    wheelFrame = null;
    const request = pendingWheelZoom;
    pendingWheelZoom = null;
    if (!request || !els.svg || !els.chartWrap) return;

    setZoom(request.zoom, request);
  }

  function getViewportCenter() {
    if (!els.chartWrap) return null;
    const bounds = els.chartWrap.getBoundingClientRect();
    return {
      clientX: bounds.left + (bounds.width / 2),
      clientY: bounds.top + (bounds.height / 2),
    };
  }

  function setZoom(value, anchor = null) {
    const zoom = clampZoom(value);
    if (!viewport.baseWidth || !viewport.baseHeight || !els.svg || !els.chartWrap) {
      viewport.zoom = zoom;
      updateControls();
      return;
    }

    const chartBounds = els.chartWrap.getBoundingClientRect();
    const svgBounds = els.svg.getBoundingClientRect();
    if (!anchor || !svgBounds.width || !svgBounds.height) {
      viewport.zoom = zoom;
      applyDisplayDimensions();
      return;
    }

    const pointerX = anchor.clientX - chartBounds.left;
    const pointerY = anchor.clientY - chartBounds.top;
    const logicalX = (
      els.chartWrap.scrollLeft + pointerX
    ) / (svgBounds.width / viewport.baseWidth);
    const logicalY = (
      els.chartWrap.scrollTop + pointerY
    ) / (svgBounds.height / viewport.baseHeight);

    viewport.zoom = zoom;
    applyDisplayDimensions();

    const scaledBounds = els.svg.getBoundingClientRect();
    els.chartWrap.scrollLeft = (
      logicalX * (scaledBounds.width / viewport.baseWidth)
    ) - pointerX;
    els.chartWrap.scrollTop = (
      logicalY * (scaledBounds.height / viewport.baseHeight)
    ) - pointerY;
  }

  function zoomIn() {
    setZoom(viewport.zoom * BUTTON_ZOOM_FACTOR, getViewportCenter());
  }

  function zoomOut() {
    setZoom(viewport.zoom / BUTTON_ZOOM_FACTOR, getViewportCenter());
  }

  function resetZoom() {
    hideTooltip();
    if (wheelFrame !== null) {
      cancelAnimationFrame(wheelFrame);
      wheelFrame = null;
      pendingWheelZoom = null;
    }
    setZoom(1, getViewportCenter());
  }

  function toDisplayCoordinates(x, y) {
    if (!els.svg || !viewport.baseWidth || !viewport.baseHeight) {
      return { x: Number(x) || 0, y: Number(y) || 0 };
    }
    const bounds = els.svg.getBoundingClientRect();
    return {
      x: (Number(x) || 0) * (bounds.width / viewport.baseWidth),
      y: (Number(y) || 0) * (bounds.height / viewport.baseHeight),
    };
  }

  function updateControls() {
    const hasTree = viewport.baseWidth > 0 && viewport.baseHeight > 0;
    const zoom = clampZoom(viewport.zoom);
    if (els.treeZoomValue) {
      els.treeZoomValue.textContent = `${Math.round(zoom * 100)}%`;
    }
    if (els.treeZoomOut) {
      els.treeZoomOut.disabled = !hasTree || zoom <= viewport.minZoom;
    }
    if (els.treeZoomIn) {
      els.treeZoomIn.disabled = !hasTree || zoom >= viewport.maxZoom;
    }
    if (els.treeZoomReset) {
      els.treeZoomReset.disabled = !hasTree || Math.abs(zoom - 1) < 0.0001;
    }
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

    els.chartWrap.addEventListener("wheel", queueWheelZoom, { passive: false });
    els.treeZoomOut?.addEventListener("click", zoomOut);
    els.treeZoomReset?.addEventListener("click", resetZoom);
    els.treeZoomIn?.addEventListener("click", zoomIn);
    updateControls();

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
    applyDisplayDimensions,
    clampZoom,
    setZoom,
    zoomIn,
    zoomOut,
    resetZoom,
    toDisplayCoordinates,
    getState,
  };
})(window);
