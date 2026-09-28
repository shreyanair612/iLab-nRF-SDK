/* Dashboard client. Polls /api/state twice a second and /api/runs every 5 s. */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const PATHS = ["raw_left", "raw_right", "raw_lr_mix", "bf_lr", "raw_3ch", "bf_3ch"];
  // Categorical slots in fixed order (validated palette, light/dark via CSS tokens).
  const COLOR = { raw_left: "--s1", raw_right: "--s2", raw_lr_mix: "--s3", bf_lr: "--s4", raw_3ch: "--s5", bf_3ch: "--s6" };
  const css = (v) => getComputedStyle(document.documentElement).getPropertyValue(v).trim();
  const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const fmt = (v, d = 0) => (v === undefined || v === null || v === "" ? "—" : Number(v).toFixed(d));
  const ms = (v) => (v === undefined || v === null ? "—" : (v / 1000).toFixed(2) + " s");
  let state = null, runs = [], selected = new Set(), formInit = false;

  async function getJSON(url) { const r = await fetch(url); return r.json(); }
  async function postJSON(url, body) {
    const r = await fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });
    return r.json();
  }

  // ---------------------------------------------------------------- header
  function renderHeader(s) {
    const c = s.connection;
    const conn = $("conn");
    conn.textContent = c.connected ? (c.simulated ? "simulated device" : "connected " + c.port) : "disconnected";
    conn.className = "pill " + (c.connected ? "ok" : "bad");
    $("devpill").textContent = "device: " + (s.device.device || "—") + (s.device.fw ? " · " + s.device.fw : "");
    const st = s.run.phase || "idle";
    const sp = $("statepill");
    sp.textContent = st + (s.run.run_id ? " · run " + s.run.run_id : "");
    sp.className = "pill " + (st === "running" ? "ok" : st === "replay" ? "warn" : "");
    const kbs = (c.rx_bytes_s / 1000).toFixed(1);
    const lp = $("linkpill");
    lp.textContent = `link: ${kbs} kB/s · ${c.binary ? "binary" : "text"} · crc errors ${c.bad_crc}`;
    lp.className = "pill " + (c.bad_crc ? "warn" : "");
    $("clock").textContent = new Date().toLocaleTimeString();
  }

  // ---------------------------------------------------------------- live
  function renderLive(s) {
    const r = s.run, st = s.status || {}, cap = st.cap || {}, tx = st.tx || {}, cfg = r.config || s.config || {};
    const live = PATHS[cfg.live_path] || "—";
    const dur = cfg.duration_s || 0;
    const el = r.elapsed_ms || st.elapsed_ms || 0;
    const rows = [
      ["run id", r.run_id || "—"], ["phase", r.phase], ["run folder", r.run_dir || "—"],
      ["elapsed", ms(el)], ["duration", dur ? dur + " s" : "until stop"],
      ["angle / distance", `${cfg.angle_deg ?? "—"}° / ${cfg.distance_cm ?? "—"} cm`],
      ["label", cfg.label || "—"], ["preset", cfg.preset || "—"], ["live path", live],
      ["enabled paths", (cfg.paths || []).filter((p) => p.enabled).map((p) => p.name).join(", ") || "—"],
      ["sample rate / frame", `${cfg.sample_rate || 16000} Hz / ${cfg.frame || 256}`],
      ["capture mode", cfg.capture_mode || "—"], ["fw / model", `${cfg.fw || "—"} / ${cfg.model || "—"}`],
      ["capture drops", `${cap.dropped ?? "—"} (queue hw ${cap.qhw ?? "—"}/${cap.qdepth ?? "—"})`],
      ["tx drops", `${tx.dropped ?? "—"} (audio ${tx.audio_dropped ?? "—"})`],
      ["tx ring", `${tx.ring_used ?? "—"} / ${tx.ring ?? "—"} B (hw ${tx.ring_hw ?? "—"})`],
      ["audio gaps", r.gaps.length], ["quality", r.quality || "—"],
    ];
    if (r.replay_job) {
      const j = r.replay_job;
      rows.push(["replay", `${j.path || "—"} (${j.index + 1}/${j.paths.length}) ${j.consumed}/${j.total} frames${j.error ? " ERROR " + j.error : ""}`]);
    }
    $("live-kv").innerHTML = rows.map(([k, v]) => `<div><b>${esc(k)}</b><span>${esc(v)}</span></div>`).join("");
    let pct = 0;
    if (r.phase === "running" && dur) pct = Math.min(100, (el / 1000 / dur) * 100);
    if (r.phase === "replay" && r.replay_job && r.replay_job.total) pct = (r.replay_job.consumed / r.replay_job.total) * 100;
    $("progress").style.width = pct + "%";
    $("warnings").innerHTML = (r.warnings || []).map((w) => `<div>${esc(w)}</div>`).join("");
  }

  // ---------------------------------------------------------------- comparison
  function metricFor(r, name) {
    const live = r.paths[name], rep = r.replay_paths[name];
    const cfgLive = PATHS[(r.config || {}).live_path];
    if (name === cfgLive && live && live.scored) return { m: live, by: "live" };
    if (rep) return { m: rep, by: "replay" };
    return { m: live || {}, by: live && live.enabled ? "pending" : "none" };
  }
  function renderCompare(s) {
    const r = s.run, cfg = r.config || s.config || {};
    const files = r.audio_files || [];
    const tb = $("cmp").querySelector("tbody");
    tb.innerHTML = PATHS.map((name) => {
      const pc = (cfg.paths || []).find((p) => p.name === name) || {};
      const { m, by } = metricFor(r, name);
      const f = files.find((x) => x.stream === name && x.reconstructed);
      const crc = f ? (f.crc_match === true ? '<span class="badge-ok">match</span>' : f.crc_match === false ? '<span class="badge-bad">MISMATCH</span>' : "n/a") : (name === "raw_left" || name === "raw_right" ? "raw" : "—");
      const scored = by === "live" || by === "replay";
      return `<tr>
        <td><i class="sw" style="background:${css(COLOR[name])}"></i>${name}</td>
        <td>${pc.enabled ? "yes" : "no"}</td>
        <td>${by}</td>
        <td class="num">${scored ? fmt(m.det) : "—"}</td>
        <td class="num">${scored ? fmt(m.peak_x1000 / 1000, 3) : "—"}</td>
        <td class="num">${scored ? fmt(m.mean_x1000 / 1000, 3) : "—"}</td>
        <td class="num">${scored && m.last_det_sample !== 4294967295 && m.last_det_sample !== undefined ? (m.last_det_sample / 16000).toFixed(2) + " s" : "—"}</td>
        <td class="num">${fmt(m.rms)}</td><td class="num">${fmt(m.peak_abs)}</td><td class="num">${fmt(m.clips)}</td>
        <td class="num">${scored ? fmt(m.inf_mean_us / 1000, 2) + " ms" : "—"}</td>
        <td class="num">${scored ? fmt(m.inf_max_us / 1000, 2) + " ms" : "—"}</td>
        <td class="num">${scored ? fmt(m.miss) : "—"}</td>
        <td>${crc}</td></tr>`;
    }).join("");
    // beamformer table
    const bt = $("bf").querySelector("tbody");
    bt.innerHTML = ["bf_lr", "bf_3ch"].map((name) => {
      const pc = (cfg.paths || []).find((p) => p.name === name) || {};
      const { m } = metricFor(r, name);
      const bf = (m && m.bf) || {};
      return `<tr><td><i class="sw" style="background:${css(COLOR[name])}"></i>${name}</td>
        <td class="num">${pc.dl ?? 0}</td><td class="num">${pc.dr ?? 0}</td><td class="num">${pc.dc ?? 0}</td>
        <td class="num">${pc.gain ?? 32767}</td><td class="num">${fmt(bf.clips)}</td>
        <td class="muted">${name === "bf_3ch" && (cfg.channels || 2) < 3 ? "needs a center microphone (Phase 2)" : "baseline integer delay-and-sum"}</td></tr>`;
    }).join("");
    // streams
    const sb = $("streams").querySelector("tbody");
    sb.innerHTML = Object.values(r.streams || {}).map((x) => `<tr><td>${esc(x.name)}</td><td class="num">${fmt(x.rms)}</td><td class="num">${fmt(x.peak)}</td><td class="num">${fmt(x.clips)}</td></tr>`).join("") || '<tr><td colspan="4" class="muted">no audio stats yet</td></tr>';
  }

  // ---------------------------------------------------------------- timeline
  const canvas = $("timeline"), tip = $("tip");
  let marks = [];
  function renderTimeline(s) {
    const r = s.run, cfg = r.config || {};
    const dpr = window.devicePixelRatio || 1;
    const W = canvas.clientWidth, H = canvas.clientHeight;
    canvas.width = W * dpr; canvas.height = H * dpr;
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, W, H);
    const lanes = PATHS.filter((n) => (cfg.paths || []).some((p) => p.name === n && p.enabled));
    const list = lanes.length ? lanes : PATHS;
    const durS = Math.max(1, cfg.duration_s || 0, (r.elapsed_ms || 0) / 1000, ...r.events.map((e) => (e.ms || 0) / 1000 + 0.5));
    const left = 92, right = W - 12, top = 14, bottom = H - 26, laneH = (bottom - top) / list.length;
    const x = (t) => left + (t / durS) * (right - left);
    ctx.font = "11px system-ui, sans-serif";
    ctx.strokeStyle = css("--line"); ctx.lineWidth = 1;
    // grid + axis
    const ticks = 6;
    for (let i = 0; i <= ticks; i++) {
      const t = (durS * i) / ticks, px = Math.round(x(t)) + 0.5;
      ctx.beginPath(); ctx.moveTo(px, top); ctx.lineTo(px, bottom); ctx.stroke();
      ctx.fillStyle = css("--text-secondary"); ctx.textAlign = "center";
      ctx.fillText(t.toFixed(1) + " s", px, H - 8);
    }
    marks = [];
    list.forEach((name, i) => {
      const y = top + laneH * i + laneH / 2;
      ctx.strokeStyle = css("--line"); ctx.beginPath(); ctx.moveTo(left, y + 0.5); ctx.lineTo(right, y + 0.5); ctx.stroke();
      ctx.fillStyle = css("--text-primary"); ctx.textAlign = "left"; ctx.fillText(name, 8, y + 4);
      const col = css(COLOR[name]);
      r.events.filter((e) => e.name === name).forEach((e) => {
        const px = x((e.ms || 0) / 1000), rad = 5;
        ctx.beginPath(); ctx.arc(px, y, rad, 0, Math.PI * 2);
        ctx.fillStyle = col; ctx.fill();
        ctx.lineWidth = 2; ctx.strokeStyle = css("--surface-1"); ctx.stroke();
        if (e.replay) { ctx.lineWidth = 1.5; ctx.strokeStyle = col; ctx.beginPath(); ctx.arc(px, y, rad + 3, 0, Math.PI * 2); ctx.stroke(); }
        marks.push({ x: px, y, e, name });
      });
    });
    // live cursor
    if (r.phase === "running") {
      const px = x((r.elapsed_ms || 0) / 1000);
      ctx.strokeStyle = css("--accent"); ctx.setLineDash([3, 3]); ctx.beginPath(); ctx.moveTo(px, top); ctx.lineTo(px, bottom); ctx.stroke(); ctx.setLineDash([]);
    }
    $("legend").innerHTML = list.map((n) => `<span><i class="sw" style="background:${css(COLOR[n])}"></i>${n}</span>`).join("") + `<span>◎ ring = replay-scored</span>`;
  }
  canvas.addEventListener("mousemove", (ev) => {
    const rect = canvas.getBoundingClientRect();
    const mx = ev.clientX - rect.left, my = ev.clientY - rect.top;
    let best = null, bd = 12 * 12;
    for (const m of marks) { const d = (m.x - mx) ** 2 + (m.y - my) ** 2; if (d < bd) { bd = d; best = m; } }
    if (!best) { tip.style.display = "none"; return; }
    const e = best.e;
    tip.innerHTML = `<b>${esc(best.name)}</b> ${(e.ms / 1000).toFixed(2)} s<br>score ${(e.score_x1000 / 1000).toFixed(3)} · peak ${(e.peak_x1000 / 1000).toFixed(3)}<br>${e.replay ? "replay" : "live"} · #${e.n}`;
    tip.style.display = "block"; tip.style.left = ev.clientX + 12 + "px"; tip.style.top = ev.clientY + 12 + "px";
  });
  canvas.addEventListener("mouseleave", () => (tip.style.display = "none"));

  // ---------------------------------------------------------------- audio
  function renderAudio(s) {
    const r = s.run, tb = $("audio").querySelector("tbody");
    $("audio-run").textContent = r.run_dir ? "· " + r.run_dir : "";
    if (!r.run_dir || !(r.audio_files || []).length) { tb.innerHTML = '<tr><td colspan="7" class="muted">no audio saved yet</td></tr>'; return; }
    tb.innerHTML = r.audio_files.map((f) => {
      const url = `/api/runs/${encodeURIComponent(r.run_dir)}/file/${encodeURIComponent(f.file)}`;
      const status = f.complete === false ? '<span class="badge-bad">incomplete (gaps)</span>' : f.reconstructed ? (f.crc_match === false ? '<span class="badge-bad">CRC mismatch</span>' : '<span class="badge-ok">host reconstruction, CRC ' + (f.crc_match ? "verified" : "unverified") + "</span>") : f.replay ? '<span class="badge-ok">device replay output</span>' : '<span class="badge-ok">complete</span>';
      return `<tr><td>${esc(f.file)}</td><td>${esc(f.stream)}</td><td class="num">${fmt(f.duration_s, 2)} s</td><td class="num">${f.sample_rate || 16000}</td><td>${status}</td><td><audio controls preload="none" src="${url}"></audio></td><td><a href="${url}" download>wav</a></td></tr>`;
    }).join("");
  }

  // ---------------------------------------------------------------- history
  async function loadRuns() {
    runs = await getJSON("/api/runs");
    renderHistory();
  }
  function renderHistory() {
    const f = ($("hist-filter").value || "").toLowerCase();
    const tb = $("hist").querySelector("tbody");
    const rows = runs.filter((r) => !f || JSON.stringify([r.label, r.angle_deg, r.distance_cm, r.preset, r.run_dir]).toLowerCase().includes(f));
    tb.innerHTML = rows.map((r) => {
      const det = Object.entries(r.detections || {}).map(([k, v]) => `${k}:${v || 0}`).join(" ");
      const q = r.quality === "valid" ? "badge-ok" : r.quality === "degraded" ? "badge-warn" : "";
      const dl = `/api/runs/${encodeURIComponent(r.run_dir)}/file/`;
      return `<tr><td><input type="checkbox" data-run="${esc(r.run_dir)}" ${selected.has(r.run_dir) ? "checked" : ""}></td>
        <td>${esc(r.created || r.run_dir)}</td><td>${esc(r.label)}</td><td class="num">${r.angle_deg ?? ""}</td><td class="num">${r.distance_cm ?? ""}</td>
        <td>${esc(r.preset)}</td><td>${esc((r.enabled_paths || []).join(","))}</td><td class="num">${r.threshold_x1000 ?? ""}</td><td>${esc(r.bf_lr)}</td>
        <td>${esc(det)}</td><td>${esc((r.replays || []).join(","))}</td><td class="${q}">${esc(r.quality)}${r.warnings ? " (" + r.warnings + " warn)" : ""}</td>
        <td><a href="/api/runs/${encodeURIComponent(r.run_dir)}/report" target="_blank">report</a> · <a href="${dl}run_metadata.json">json</a> · <a href="${dl}run_summary.csv">csv</a> · <a href="${dl}events.jsonl">events</a> · <button data-replay="${esc(r.run_dir)}">replay</button></td></tr>`;
    }).join("") || '<tr><td colspan="13" class="muted">no runs yet</td></tr>';
    tb.querySelectorAll("input[type=checkbox]").forEach((cb) => cb.addEventListener("change", () => { cb.checked ? selected.add(cb.dataset.run) : selected.delete(cb.dataset.run); }));
    tb.querySelectorAll("button[data-replay]").forEach((b) => b.addEventListener("click", async () => {
      const cfgPaths = (runs.find((x) => x.run_dir === b.dataset.replay) || {}).enabled_paths || [];
      const paths = prompt("Paths to replay through the device (comma separated)", cfgPaths.join(","));
      if (!paths) return;
      const res = await postJSON("/api/replay", { run_dir: b.dataset.replay, paths: paths.split(",").map((x) => x.trim()) });
      $("histmsg").textContent = res.ok ? "replay started" : "replay refused: " + res.error;
    }));
  }
  $("hist-filter").addEventListener("input", renderHistory);
  $("btn-compare").addEventListener("click", async () => {
    const [a, b] = [...selected];
    if (!a || !b) { $("histmsg").textContent = "select two runs"; return; }
    const res = await getJSON(`/api/compare?a=${encodeURIComponent(a)}&b=${encodeURIComponent(b)}`);
    const cols = ["name", "scored_by", "detections", "peak_x1000", "mean_x1000", "rms", "clips", "inf_mean_us", "deadline_misses", "crc_match"];
    const side = (k) => {
      const x = res[k]; if (!x) return "<p>missing</p>";
      const c = x.metadata.config || {};
      return `<h3 style="margin:8px 0 4px">${esc(x.run_dir)}</h3><p class="muted">${esc(c.label)} · ${c.angle_deg}° · ${c.distance_cm} cm · ${esc(c.preset)} · thr ${c.threshold_x1000} · quality ${esc(x.metadata.quality)}</p>
        <div class="tblwrap"><table><thead><tr>${cols.map((h) => `<th>${h}</th>`).join("")}</tr></thead><tbody>${x.summary.map((r) => `<tr>${cols.map((h) => `<td>${esc(r[h])}</td>`).join("")}</tr>`).join("")}</tbody></table></div>`;
    };
    $("compare").innerHTML = `<div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:12px">${side("a")}${side("b")}</div>`;
  });

  // ---------------------------------------------------------------- form
  function initForm(s) {
    if (formInit) return;
    const e = s.enums, cfg = s.config || {};
    if (!e) return;
    $("f-preset").innerHTML = e.presets.map((p) => `<option ${p === (cfg.preset || "raw_vs_bf_lr") ? "selected" : ""}>${p}</option>`).join("");
    $("f-live").innerHTML = e.paths.map((p, i) => `<option value="${p}" ${i === (cfg.live_path ?? 3) ? "selected" : ""}>${p}</option>`).join("");
    $("f-capmode").innerHTML = e.capture_modes.map((m) => `<option ${m === (cfg.capture_mode || "event_window") ? "selected" : ""}>${m}</option>`).join("");
    $("f-reduce").innerHTML = e.reduce_modes.map((m) => `<option ${m === (cfg.reduce || "lrc_average") ? "selected" : ""}>${m}</option>`).join("");
    $("pathchecks").innerHTML = "<b class=muted style='width:100%'>paths</b>" + e.paths.map((p, i) => { const pc = (cfg.paths || [])[i] || {}; return `<label><input type="checkbox" data-path="${p}" ${pc.enabled || (!cfg.paths && i < 4) ? "checked" : ""}>${p}</label>`; }).join("");
    $("capchecks").innerHTML = "<b class=muted style='width:100%'>stream audio to host</b>" + ["raw_left", "raw_right", "raw_lr_mix", "bf_lr"].map((p) => `<label><input type="checkbox" data-cap="${p}" ${p.startsWith("raw_l") || p.startsWith("raw_r") ? "checked" : ""}>${p}</label>`).join("");
    if (cfg.duration_s !== undefined) $("f-duration").value = cfg.duration_s;
    if (cfg.threshold_x1000 !== undefined) $("f-threshold").value = cfg.threshold_x1000;
    if (cfg.cooldown_ms !== undefined) $("f-cooldown").value = cfg.cooldown_ms;
    formInit = true;
  }
  $("btn-start").addEventListener("click", async () => {
    const paths = {}, capture = {};
    document.querySelectorAll("#pathchecks input").forEach((c) => (paths[c.dataset.path] = c.checked));
    document.querySelectorAll("#capchecks input").forEach((c) => (capture[c.dataset.cap] = c.checked));
    const body = {
      label: $("f-label").value.trim().replace(/\s+/g, "_"), angle: $("f-angle").value, distance: $("f-distance").value,
      phrase: $("f-phrase").value.trim().replace(/\s+/g, "_"), env: $("f-env").value.trim().replace(/\s+/g, "_"),
      preset: $("f-preset").value, live_path: $("f-live").value, duration: $("f-duration").value,
      threshold: $("f-threshold").value, cooldown: $("f-cooldown").value, capture_mode: $("f-capmode").value,
      capture_seconds: $("f-capsec").value, reduce: $("f-reduce").value,
      delays: { bf_lr: [Number($("f-dl").value) || 0, Number($("f-dr").value) || 0] }, paths, capture,
    };
    // preset first, then explicit path toggles override it
    const res = await postJSON("/api/start", body);
    $("startmsg").textContent = res.ok ? "sent " + res.commands.length + " commands; watch the console for ok/err" : "failed";
  });
  $("btn-stop").addEventListener("click", () => postJSON("/api/stop"));
  $("btn-send").addEventListener("click", sendCmd);
  $("cmd").addEventListener("keydown", (e) => { if (e.key === "Enter") sendCmd(); });
  async function sendCmd() { const v = $("cmd").value.trim(); if (!v) return; await postJSON("/api/command", { line: v }); $("cmd").value = ""; }

  // ---------------------------------------------------------------- loop
  let lastLogLen = -1;
  async function tick() {
    try {
      state = await getJSON("/api/state");
      renderHeader(state); initForm(state); renderLive(state); renderCompare(state); renderTimeline(state); renderAudio(state);
      const log = $("log");
      if (state.log.length !== lastLogLen || (state.log.length && log.dataset.last !== state.log[state.log.length - 1])) {
        const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 4;
        log.textContent = state.log.join("\n");
        log.dataset.last = state.log[state.log.length - 1] || "";
        lastLogLen = state.log.length;
        if (atBottom) log.scrollTop = log.scrollHeight;
      }
    } catch (e) {
      $("conn").textContent = "dashboard server unreachable"; $("conn").className = "pill bad";
    }
  }
  tick(); setInterval(tick, 500);
  loadRuns(); setInterval(loadRuns, 5000);
  window.addEventListener("resize", () => state && renderTimeline(state));
})();
