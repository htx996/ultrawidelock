(() => {
  "use strict";

  const model = JSON.parse(document.getElementById("dashboard-data").textContent);
  const number = new Intl.NumberFormat("en-US", { maximumFractionDigits: 1 });
  let latencyMode = "duration";

  function node(tag, className, text) {
    const element = document.createElement(tag);
    if (className) element.className = className;
    if (text !== undefined && text !== null) element.textContent = String(text);
    return element;
  }

  function clear(element) {
    element.replaceChildren();
    return element;
  }

  function formatBytes(value) {
    if (value === null || value === undefined || !Number.isFinite(Number(value))) return "n/a";
    const bytes = Number(value);
    if (Math.abs(bytes) >= 1024 * 1024) return `${number.format(bytes / 1024 / 1024)} MiB`;
    if (Math.abs(bytes) >= 1024) return `${number.format(bytes / 1024)} KiB`;
    return `${number.format(bytes)} B`;
  }

  function formatMs(value) {
    return value === null || value === undefined ? "n/a" : `${number.format(value)} ms`;
  }

  function signedBytes(value) {
    if (value === null || value === undefined) return "n/a";
    return `${value > 0 ? "+" : ""}${formatBytes(value)}`;
  }

  function emptyState(title, detail) {
    const wrapper = node("div", "empty-state");
    const inner = node("div");
    inner.append(node("strong", "", title), node("p", "", detail));
    wrapper.append(inner);
    return wrapper;
  }

  function panelTitle(title, detail) {
    const wrapper = node("div", "panel-title");
    wrapper.append(node("h3", "", title), node("span", "", detail));
    return wrapper;
  }

  function metric(label, value, detail) {
    const card = node("article", "metric-card");
    card.append(
      node("span", "metric-label", label),
      node("span", "metric-value", value),
      node("span", "metric-detail", detail)
    );
    return card;
  }

  function regionByName(regions, name) {
    return (regions || []).find((region) => String(region.name).toUpperCase() === name);
  }

  function renderHeader() {
    const run = model.run;
    const verdict = model.verdict;
    document.getElementById("verdict-detail").textContent = verdict.detail;
    document.getElementById("next-action").textContent = model.next_action;
    const pill = document.getElementById("verdict-pill");
    pill.textContent = verdict.label;
    pill.dataset.tone = verdict.code === "blocked" || verdict.code === "failed"
      ? "danger"
      : verdict.code === "measured" ? "good" : "warn";

    const meta = document.getElementById("run-meta");
    const commit = run.commit ? run.commit.slice(0, 12) : "unknown commit";
    [run.board, run.profile, commit + (run.dirty ? " · dirty" : ""), run.generated_at].forEach((item) => {
      meta.append(node("span", "", item));
    });
    document.getElementById("footer-stamp").textContent = `${run.id} · SCHEMA ${model.schema_version}`;
  }

  function renderMetrics() {
    const strip = document.getElementById("metric-strip");
    const memory = model.memory;
    const current = memory.status === "measured" ? memory.regions : [];
    const reference = memory.baseline_reference ? memory.baseline_reference.regions : [];
    const flash = regionByName(current, "FLASH") || regionByName(reference, "FLASH");
    const ram = regionByName(current, "RAM") || regionByName(reference, "RAM");
    const referenceOnly = memory.status !== "measured";
    const p95 = model.latency.status === "measured" ? model.latency.end_to_end_ms.p95 : null;
    const measuredCount = model.evidence.filter((item) => item.status === "measured").length;
    strip.append(
      metric("Flash free", flash ? formatBytes(flash.free) : "Unavailable", referenceOnly ? "baseline reference only" : `${number.format(flash.pct)}% used`),
      metric("RAM free", ram ? formatBytes(ram.free) : "Unavailable", referenceOnly ? "baseline reference only" : `${number.format(ram.pct)}% used`),
      metric("Walk-up p95", p95 === null ? "Not measured" : formatMs(p95), p95 === null ? "hardware evidence absent" : `${model.latency.end_to_end_ms.count} successful samples`),
      metric("Evidence", `${measuredCount}/${model.evidence.length}`, "sources measured")
    );
  }

  function regionCard(region, referenceOnly) {
    const card = node("article", "region-card");
    const top = node("div", "region-top");
    top.append(node("span", "region-name", region.name));
    const gate = node("span", "gate-chip", referenceOnly ? "reference" : region.gate);
    gate.dataset.status = referenceOnly ? "unknown" : region.gate;
    top.append(gate);
    const values = node("div", "region-numbers");
    const used = node("div");
    used.append(node("strong", "", formatBytes(region.used)), node("span", "", "used"));
    const free = node("div");
    free.append(node("strong", "", formatBytes(region.free)), node("span", "", "free"));
    values.append(used, free);
    const meter = node("div", "meter");
    meter.setAttribute("role", "meter");
    meter.setAttribute("aria-label", `${region.name} utilization`);
    meter.setAttribute("aria-valuemin", "0");
    meter.setAttribute("aria-valuemax", "100");
    meter.setAttribute("aria-valuenow", String(region.pct || 0));
    const fill = node("div", "meter-fill");
    fill.style.width = `${Math.min(100, Math.max(0, Number(region.pct || 0)))}%`;
    meter.append(fill);
    const foot = node("div", "region-foot");
    foot.append(
      node("span", "", `${number.format(region.pct || 0)}% occupied`),
      node("span", "", referenceOnly ? "not a current measurement" : `delta ${signedBytes(region.delta)}`)
    );
    card.append(top, values, meter, foot);
    return card;
  }

  function barList(rows, valueField, valueFormatter, signed) {
    const list = node("div", "bar-list");
    const magnitude = Math.max(1, ...rows.map((row) => Math.abs(Number(row[valueField] || 0))));
    rows.forEach((row) => {
      const line = node("div", "bar-row");
      const label = node("span", "bar-label", row.name || row.phase);
      label.title = row.name || row.phase;
      const track = node("div", "bar-track");
      const fill = node("div", "bar-fill");
      fill.style.width = `${Math.max(1.5, Math.abs(Number(row[valueField] || 0)) / magnitude * 100)}%`;
      track.append(fill);
      const value = node("span", "bar-value", valueFormatter(row[valueField]));
      if (signed) value.classList.add(Number(row[valueField]) > 0 ? "positive" : "negative");
      line.append(label, track, value);
      list.append(line);
    });
    return list;
  }

  function renderMemory() {
    const memory = model.memory;
    const content = clear(document.getElementById("memory-content"));
    const note = document.getElementById("memory-note");
    let regions = memory.regions;
    const referenceOnly = memory.status !== "measured";
    if (referenceOnly && memory.baseline_reference) regions = memory.baseline_reference.regions;
    note.textContent = referenceOnly
      ? "No current ELF was measured. Checked-in values below are context, never a result."
      : memory.comparable ? `Compared with ${memory.baseline_key}.` : "Current data exists, but its configuration does not match a baseline.";
    if (!regions || !regions.length) {
      content.append(emptyState("No footprint evidence", "Build the nested application image and produce size-report.json, then recollect."));
      return;
    }
    const grid = node("div", "memory-grid");
    regions.forEach((region) => grid.append(regionCard(region, referenceOnly)));
    content.append(grid);
    if (referenceOnly) {
      content.append(node("div", "warning-callout", "Reference-only baseline: these bytes do not describe this worktree's current image."));
      return;
    }
    const subgrid = node("div", "subgrid");
    const primary = node("article", "chart-panel");
    const hasMovers = memory.top_movers && memory.top_movers.length;
    primary.append(panelTitle(hasMovers ? "Top symbol movers" : "Largest sections", hasMovers ? "indicative under LTO" : "current image"));
    if (hasMovers) primary.append(barList(memory.top_movers, "delta", signedBytes, true));
    else if (memory.top_sections && memory.top_sections.length) primary.append(barList(memory.top_sections, "bytes", formatBytes, false));
    else primary.append(emptyState("No attribution data", "The region totals remain authoritative."));

    const cross = node("article", "table-panel");
    cross.append(panelTitle("Independent cross-check", "linker vs Zephyr"));
    if (memory.crosscheck && memory.crosscheck.length) {
      const table = node("table", "data-table");
      const head = node("thead");
      const headRow = node("tr");
      ["Source", "Regions", "Zephyr", "Delta"].forEach((label) => headRow.append(node("th", "", label)));
      head.append(headRow);
      const body = node("tbody");
      memory.crosscheck.forEach((row) => {
        const tr = node("tr");
        tr.append(node("td", "", row.kind.toUpperCase()), node("td", "", formatBytes(row.regions)), node("td", "", formatBytes(row.zephyr_report)), node("td", "", signedBytes(row.delta)));
        body.append(tr);
      });
      table.append(head, body);
      cross.append(table);
    } else {
      cross.append(node("p", "section-note", "Zephyr RAM/ROM report cross-checks were unavailable."));
    }
    subgrid.append(primary, cross);
    if (memory.compiler_stack && memory.compiler_stack.status === "measured") {
      const stacks = node("article", "chart-panel");
      stacks.append(panelTitle("Compiler stack estimates", `${memory.compiler_stack.file_count} .su files`));
      const rows = memory.compiler_stack.rows.slice(0, 12).map((row) => ({
        name: row.function,
        bytes: row.bytes
      }));
      stacks.append(barList(rows, "bytes", formatBytes, false));
      subgrid.append(stacks);
    }
    content.append(subgrid);
    if (!memory.comparable) {
      content.append(node("div", "warning-callout", "Configuration mismatch: deltas and gates are intentionally withheld."));
    }
  }

  function renderLatencyChart() {
    const holder = clear(document.getElementById("latency-chart"));
    const field = latencyMode === "duration" ? "duration_ms" : "offset_ms";
    const rows = model.latency.phases.map((row) => ({
      phase: row.phase,
      value: row[field].p95
    })).filter((row) => row.value !== null);
    if (!rows.length) {
      holder.append(emptyState("No phase samples", "The supplied trace did not contain enough successful phase measurements."));
      return;
    }
    const normalized = rows.map((row) => ({ name: row.phase, value: row.value }));
    holder.append(barList(normalized, "value", formatMs, false));
  }

  function renderLatency() {
    const content = clear(document.getElementById("latency-content"));
    const latency = model.latency;
    const controls = document.getElementById("latency-mode");
    controls.hidden = latency.status !== "measured";
    if (latency.status !== "measured") {
      content.append(emptyState("Latency not measured", "Supply a sanitized ultrawidelock-lat log. The dashboard will calculate phase durations and p50/p95/p99 distributions."));
      return;
    }
    const layout = node("div", "latency-layout");
    const chart = node("article", "chart-panel latency-bars");
    chart.append(panelTitle("Phase p95", "milliseconds"));
    const chartHolder = node("div");
    chartHolder.id = "latency-chart";
    chart.append(chartHolder);

    const summary = node("aside", "runtime-card");
    summary.append(panelTitle("Walk-up distribution", `${latency.end_to_end_ms.count} successes`));
    const stats = node("div", "stat-grid");
    [["p50", latency.end_to_end_ms.p50], ["p95", latency.end_to_end_ms.p95], ["p99", latency.end_to_end_ms.p99], ["max", latency.end_to_end_ms.max]].forEach(([label, value]) => {
      const cell = node("div", "stat-cell");
      cell.append(node("strong", "", formatMs(value)), node("span", "", label));
      stats.append(cell);
    });
    summary.append(stats);
    const outcomes = node("div", "outcome-list");
    const rows = [
      ["Attempts", latency.outcomes.attempts === null ? "partial denominator" : latency.outcomes.attempts],
      ["Successful", latency.outcomes.successful],
      ["No grant", latency.outcomes.no_grant],
      ["Rejected", latency.outcomes.rejected],
      ["Timed out", latency.outcomes.timed_out],
      ["Invalid", latency.outcomes.invalid]
    ];
    rows.forEach(([label, value]) => {
      const row = node("div", "outcome-row");
      row.append(node("span", "", label), node("strong", "", value));
      outcomes.append(row);
    });
    summary.append(outcomes);
    layout.append(chart, summary);
    content.append(layout);
    renderLatencyChart();
  }

  function renderStacks(runtime) {
    const card = node("article", "runtime-card");
    card.append(panelTitle("Stack high-water", `${runtime.stacks.length} threads`));
    runtime.stacks.forEach((stack) => {
      const row = node("div", "stack-row");
      const meter = node("div", "meter");
      const fill = node("div", "meter-fill");
      fill.style.width = `${stack.used_pct}%`;
      fill.style.background = stack.used_pct >= 90 ? "var(--red)" : stack.used_pct >= 75 ? "var(--orange)" : "var(--cyan)";
      meter.append(fill);
      row.append(node("span", "stack-name", stack.thread), meter, node("span", "stack-value", `${formatBytes(stack.headroom_bytes)} free`));
      card.append(row);
    });
    return card;
  }

  function renderCpu(cpu) {
    const card = node("article", "runtime-card");
    card.append(panelTitle("CPU attribution", cpu.method));
    const stats = node("div", "stat-grid");
    const total = node("div", "stat-cell");
    total.append(node("strong", "", `${number.format(cpu.busy_pct)}%`), node("span", "", "busy"));
    const idle = node("div", "stat-cell");
    idle.append(node("strong", "", `${number.format(100 - cpu.busy_pct)}%`), node("span", "", "idle"));
    stats.append(total, idle);
    card.append(stats);
    if (cpu.threads.length) {
      card.append(barList(cpu.threads.map((row) => ({name: row.thread, value: row.utilization_pct})), "value", (value) => `${number.format(value)}%`, false));
    }
    return card;
  }

  function renderUwb(uwb) {
    const card = node("article", "runtime-card wide");
    card.append(panelTitle("UWB scheduling windows", `${number.format(uwb.deadline_us)} µs deadline`));
    const histogram = node("div", "histogram");
    Object.entries(uwb.phases).forEach(([phase, data]) => {
      const row = node("div", "hist-row");
      const bars = node("div", "hist-bars");
      const maxCount = Math.max(1, ...data.histogram.map((bucket) => bucket.count));
      data.histogram.forEach((bucket) => {
        const bar = node("span", "hist-bar");
        bar.style.height = `${Math.max(2, bucket.count / maxCount * 100)}%`;
        bar.title = `${number.format(bucket.start_us)}-${number.format(bucket.end_us)} µs: ${bucket.count}`;
        bars.append(bar);
      });
      const meta = node("div", "hist-meta");
      meta.append(node("strong", "", `${formatBytes(data.samples).replace(" B", "")} samples`), document.createElement("br"), document.createTextNode(`${number.format(data.min_us)}-${number.format(data.max_us)} µs · ${data.deadline_misses} misses · ${data.arm_failures} arm failures`));
      row.append(node("span", "hist-phase", phase), bars, meta);
      histogram.append(row);
    });
    card.append(histogram);
    return card;
  }

  function renderSpi(spi) {
    const card = node("article", "runtime-card wide");
    card.append(panelTitle("SPI traffic by ranging phase", `${number.format(spi.frequency_hz / 1e6)} MHz`));
    const table = node("table", "data-table");
    const header = node("tr");
    ["Phase", "Samples", "Avg txns", "Avg wire", "Max txns", "Errors", "Timeouts"].forEach((label) => header.append(node("th", "", label)));
    const thead = node("thead");
    thead.append(header);
    const tbody = node("tbody");
    Object.entries(spi.phases).forEach(([phase, values]) => {
      const row = node("tr");
      [phase, values.samples, values.avg_transactions ?? "n/a", values.avg_wire_bytes === null ? "n/a" : formatBytes(values.avg_wire_bytes), values.max_transactions, values.errors, values.timeouts].forEach((value) => row.append(node("td", "", value)));
      tbody.append(row);
    });
    table.append(thead, tbody);
    card.append(table);
    return card;
  }

  function renderRuntime() {
    const content = clear(document.getElementById("runtime-content"));
    const runtime = model.runtime;
    const note = document.getElementById("runtime-note");
    if (runtime.status !== "measured") {
      note.textContent = "No authorized runtime snapshot was supplied.";
      content.append(emptyState("On-target pressure is unknown", "Static size cannot establish stack headroom, CPU load, UWB deadlines, or SPI transaction cost."));
      return;
    }
    note.textContent = runtime.hardware.used
      ? `${runtime.hardware.fixture} · ${runtime.hardware.workload} · ${runtime.hardware.sample_count} samples`
      : "Snapshot supplied without a hardware workload.";
    const layout = node("div", "runtime-layout");
    if (runtime.stacks.length) layout.append(renderStacks(runtime));
    if (runtime.cpu) layout.append(renderCpu(runtime.cpu));
    if (runtime.uwb) layout.append(renderUwb(runtime.uwb));
    if (runtime.spi) layout.append(renderSpi(runtime.spi));
    if (!layout.children.length) layout.append(emptyState("Snapshot has no measurements", "The envelope is valid, but stack, CPU, UWB, and SPI sections are empty."));
    if (runtime.gates.length) {
      const card = node("article", "runtime-card wide");
      card.append(panelTitle("Hardware gates", `${runtime.gates.length} declared`));
      const gates = node("div", "gate-list");
      runtime.gates.forEach((gate) => {
        const item = node("div", "gate-item");
        const status = node("span", "gate-status", gate.status);
        status.dataset.status = gate.status;
        item.append(status, node("span", "", `${gate.name}: ${gate.detail}`));
        gates.append(item);
      });
      card.append(gates);
      layout.append(card);
    }
    content.append(layout);
  }

  function renderEvidence() {
    const grid = document.getElementById("evidence-grid");
    model.evidence.forEach((item) => {
      const card = node("article", "evidence-card");
      const top = node("div", "evidence-card-top");
      top.append(node("h3", "", item.label));
      const status = node("span", "evidence-status", item.status);
      status.dataset.status = item.status;
      top.append(status);
      card.append(top, node("p", "", item.detail));
      grid.append(card);
    });
    const artifactRow = document.getElementById("artifact-row");
    if (!model.artifacts.length) {
      artifactRow.append(node("span", "metric-detail", "No supporting artifacts were collected."));
      return;
    }
    model.artifacts.forEach((artifact) => {
      const link = node("a", "artifact-link", `${artifact.kind.toUpperCase()} · ${artifact.label}`);
      link.href = artifact.href;
      link.rel = "noopener";
      artifactRow.append(link);
    });
  }

  document.querySelectorAll("#latency-mode button").forEach((button) => {
    button.addEventListener("click", () => {
      latencyMode = button.dataset.mode;
      document.querySelectorAll("#latency-mode button").forEach((candidate) => {
        candidate.setAttribute("aria-pressed", String(candidate === button));
      });
      renderLatencyChart();
    });
  });

  renderHeader();
  renderMetrics();
  renderMemory();
  renderLatency();
  renderRuntime();
  renderEvidence();
  document.documentElement.dataset.dashboardReady = "true";
  window.__uwlDashboardReady = true;
})();
