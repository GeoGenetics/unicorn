"use strict";

(function initUnicornGraphEngineBoot(globalObject) {
  const namespace = globalObject.UnicornGraphEngine = globalObject.UnicornGraphEngine || {};

  function bootstrapGraphengine() {
    if (namespace.booted) return;
    const state = namespace.state;
    const els = namespace.els;
    const backend = namespace.backend;
    const metadata = namespace.metadata;
    const treeRender = namespace.treeRender;
    const ui = namespace.ui;
    const selection = namespace.selection;
    const damage = namespace.damage;
    const reports = namespace.reports;
    const agentUi = namespace.agentUi;

    if (!state || !els || !backend || !metadata || !treeRender || !ui || !selection || !damage || !reports || !agentUi) {
      throw new Error("Unicorn graphengine boot expected state, DOM, backend, metadata, tree render, UI, selection, damage, reports, and Agent UI modules to load before boot.");
    }

    els.renderBtn.onclick = async (event) => {
      event.preventDefault();
      await backend.loadAndRender();
    };
    els.addLcaBtn.addEventListener("click", ui.addLcaInput);
    els.clearLcaListBtn.addEventListener("click", ui.clearLcaListFile);
    els.connectBtn.addEventListener("click", backend.connectRemote);
    els.uploadBtn.addEventListener("click", backend.uploadLoadedFiles);
    els.copyTunnelBtn.addEventListener("click", backend.copyTunnelCommand);
    els.remoteRefreshBtn.addEventListener("click", async () => {
      try {
        await backend.refreshRemoteDatasets();
        const count = state.remote.datasets.length;
        globalObject.setStatus(`Backend dataset list refreshed. ${count.toLocaleString()} file${count === 1 ? "" : "s"} available.`);
      } catch (error) {
        globalObject.setStatus(`Could not refresh backend datasets: ${error.message || error}`);
      }
    });
    els.remoteSelectAllBtn.addEventListener("click", backend.selectAllRemoteDatasets);
    els.remoteClearAllBtn.addEventListener("click", backend.clearRemoteDatasets);
    els.remoteUser.addEventListener("input", backend.updateTunnelHint);
    els.remoteHost.addEventListener("input", backend.updateTunnelHint);
    els.centerBtn.addEventListener("click", () => {
      selection.clearFocus({ centerRoot: true });
    });
    if (els.countViewMode) {
      els.countViewMode.addEventListener("change", () => {
        treeRender.redraw();
      });
    }
    els.toggleTableBtn.addEventListener("click", ui.toggleTablePanel);
    els.countMode.addEventListener("change", treeRender.redraw);
    els.scaleMode.addEventListener("change", treeRender.redraw);
    els.minReads.addEventListener("input", backend.handleMinReadsChange);
    if (els.minReadsScale) {
      els.minReadsScale.addEventListener("change", ui.handleMinReadsScaleChange);
    }
    if (els.minReadsMax) {
      els.minReadsMax.addEventListener("input", ui.handleMinReadsMaxChange);
      els.minReadsMax.addEventListener("change", ui.handleMinReadsMaxChange);
    }
    els.searchBox.addEventListener("input", treeRender.redraw);
    els.sidebarToggle.addEventListener("click", ui.toggleSidebar);
    els.uncollapseBtn.addEventListener("click", selection.uncollapseSelected);
    els.uncollapseTipsBtn.addEventListener("click", selection.uncollapseSelectedToTips);
    if (els.subtreeReportBtn) {
      els.subtreeReportBtn.addEventListener("click", reports.openSelectedSubtreeReport);
    }
    if (els.rankReportBtn) {
      els.rankReportBtn.addEventListener("click", reports.openSelectedRankReport);
    }
    if (els.damageBtn) {
      els.damageBtn.addEventListener("click", damage.openSelectedDamage);
    }
    els.selectDescendantsBtn.addEventListener("click", selection.selectDescendants);
    if (els.selectToRankBtn) {
      els.selectToRankBtn.addEventListener("click", selection.selectToRank);
    }
    if (els.selectToRankValue) {
      els.selectToRankValue.addEventListener("change", backend.syncBackendRuntimeUiState);
    }
    els.clearSelectionBtn.addEventListener("click", selection.clearSelection);
    if (els.exportMatrixBtn) {
      els.exportMatrixBtn.addEventListener("click", reports.exportCurrentSubtreeMatrix);
    }
    if (els.pcoaBtn) {
      els.pcoaBtn.addEventListener("click", reports.openCurrentCountMatrixPcoa);
    }
    if (els.barplotBtn) {
      els.barplotBtn.addEventListener("click", reports.openCurrentCountMatrixBarplot);
    }
    if (els.clearClientLogBtn && typeof globalObject.clearClientLog === "function") {
      els.clearClientLogBtn.addEventListener("click", globalObject.clearClientLog);
    }

    ui.initControlsResize();
    ui.initSummaryResize();
    ui.initTablePanel();
    ui.initTableResize();
    ui.initSidebarPanel();
    treeRender.initChartPan();
    ui.initRemotePanel();
    ui.initFileInputs();
    ui.initMinReadsControls();
    agentUi.init();
    if (typeof globalObject.renderClientLog === "function") {
      globalObject.renderClientLog();
    }
    backend.updateTunnelHint();
    backend.syncBackendRuntimeUiState();
    backend.updateRenderAvailability();

    namespace.booted = true;
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", bootstrapGraphengine, { once: true });
  } else {
    bootstrapGraphengine();
  }
})(window);
