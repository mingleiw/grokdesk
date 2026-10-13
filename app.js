// grokdesk simulator.
// Default: scripted demo. Every visible label says SIMULATED.
// ?live or ?live=<url> polls a status.json document every 5s (default: ./status.json).
(() => {
  const POLL_MS = 5000;
  const KNOWN = ["healthy", "failed", "analyzing", "resolved"];
  const $ = id => document.getElementById(id);
  const screen = $("screen"), logEl = $("log"), led = $("led");
  const params = new URLSearchParams(location.search);
  if (params.has("kiosk")) document.body.classList.add("kiosk");

  let timers = [], current = null;
  const clear = () => { timers.forEach(clearTimeout); timers = []; };
  const later = (ms, fn) => timers.push(setTimeout(fn, ms));
  const esc = s => String(s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const cls = l => /ERROR|FATAL|error:/.test(l) ? "err" : /Warning|WARN/.test(l) ? "warn" : /successfully|OK|HEALTHY/.test(l) ? "ok" : "";

  function setLog(lines, hit = -1) {
    logEl.innerHTML = lines.map((l, i) => `<div class="${cls(l)} ${i === hit ? "hit" : ""}">${esc(l)}</div>`).join("");
  }
  function streamLog(lines, ms) {
    lines.forEach((_, i) => later(i * ms, () => setLog(lines.slice(0, i + 1))));
  }
  function render(state, o = {}) {
    current = state;
    const visual = o.visual || state;
    screen.className = "screen state-" + visual;
    screen.dataset.state = state;
    screen.dataset.visual = visual;
    $("scr-icon").textContent = o.icon || "";
    $("scr-title").textContent = o.title || "";
    const sub = $("scr-sub");
    sub.textContent = o.sub || "";
    sub.classList.toggle("long", (o.sub || "").length > 36);
    $("scr-explain").innerHTML = o.explain || "";
    $("scr-foot-r").textContent = o.foot || "sim";
    led.className = "led" + (o.led ? " " + o.led : "");
    document.querySelectorAll("#controls [data-state]").forEach(b => b.classList.toggle("active", b.dataset.state === state));
  }

  const S = window.SAMPLE_LOGS;
  const states = {
    healthy(fast) {
      render("healthy", { icon: "✔", title: "DEPLOY HEALTHY", sub: "checkout-api v2.14.0", foot: "3/3 pods" });
      fast ? setLog(S.healthy) : streamLog(S.healthy, 150);
    },
    failed(fast) {
      render("failed", { icon: "✖", title: "DEPLOY FAILED", sub: "checkout-api v2.15.0", led: "red", foot: "CrashLoop" });
      fast ? setLog(S.failed) : streamLog(S.failed, 90);
    },
    analyzing() {
      render("analyzing", { icon: "◌", title: "Grok Bot analyzing…", sub: `${S.failed.length} log lines`, led: "blue", foot: "simulated analysis" });
      setLog(S.failed);
    },
    explained() {
      const r = window.DeployAnalyzer.analyze(S.failed);
      window.lastAnalysis = r;
      render("explained", { title: "✖ DEPLOY FAILED", led: "red", foot: "simulated analysis",
        explain: `<b>Grok Bot:</b> ${esc(r.cause)}<br>${esc(r.effect)}<br><span class="fix">Suggest: ${esc(r.suggest)}</span>` });
      setLog(S.failed, r.keyLine);
    },
    resolved(fast) {
      render("resolved", { icon: "✔", title: "DEPLOY HEALTHY", sub: "rolled back → v2.14.0", foot: "Fixed ✓" });
      const all = S.failed.concat(S.resolved);
      fast ? setLog(all) : S.resolved.forEach((_, i) => later(i * 150, () => setLog(S.failed.concat(S.resolved.slice(0, i + 1)))));
    }
  };
  function go(state) {
    clear();
    if (state === "explained") { states.analyzing(); later(1500, states.explained); }
    else states[state]();
  }
  // ~12.5 s total: healthy 2.5 / failed 2.5 / analyzing 1.8 / explained 3.7 / resolved 2.0
  const SCRIPT = [["healthy", 0], ["failed", 2500], ["analyzing", 5000], ["explained", 6800], ["resolved", 10500]];
  function auto() {
    clear();
    SCRIPT.forEach(([s, t]) => later(t, () => states[s]()));
    later(12500, () => { window.demoDone = true; if (params.has("loop")) auto(); });
  }

  function statusLog(d) {
    const rows = ["state", "service", "summary", "fix", "run_url", "updated_at"];
    logEl.innerHTML = rows.map(k => {
      const raw = d[k] == null ? "" : String(d[k]);
      const val = /^https?:\/\/\S+$/.test(raw) ? `<a href="${esc(raw)}">${esc(raw)}</a>` : esc(raw);
      return `<div><span class="k">${k}</span> ${val}</div>`;
    }).join("");
  }

  function applyStatus(d) {
    const state = String(d.state || "").trim();
    const service = String(d.service || "").trim();
    const summary = String(d.summary || "").trim();
    const fix = String(d.fix || "").trim();
    $("log-title").textContent = "status.json · " + (service || "grokdesk");
    statusLog(d);
    if (!KNOWN.includes(state)) {
      render(state || "unknown", { visual: "error", icon: "?", title: "UNKNOWN STATE", sub: state || "(missing)", foot: "live", led: "off" });
      return;
    }
    if (state === "healthy") {
      render("healthy", { icon: "✔", title: "DEPLOY HEALTHY", sub: summary || service, foot: "live" });
    } else if (state === "analyzing") {
      render("analyzing", { icon: "◌", title: "Grok Bot analyzing…", sub: summary || service, led: "blue", foot: "live" });
    } else if (state === "resolved") {
      render("resolved", { icon: "✔", title: "DEPLOY HEALTHY", sub: summary || service || "Fixed", foot: "Fixed ✓" });
    } else if (summary || fix) {
      const parts = [];
      if (summary) parts.push(`<b>Grok Bot:</b> ${esc(summary)}`);
      if (fix) parts.push(`<span class="fix">Suggest: ${esc(fix)}</span>`);
      render("failed", { visual: "explained", title: "✖ DEPLOY FAILED", led: "red", foot: service || "live", explain: parts.join("<br>") });
    } else {
      render("failed", { icon: "✖", title: "DEPLOY FAILED", sub: service, led: "red", foot: "live" });
    }
  }

  function liveUrl() {
    const v = params.get("live");
    if (!v || v === "1" || v === "true") return "./status.json";
    return v;
  }

  function bustedStatusUrl(url) {
    const abs = new URL(url, location.href);
    // New query value every poll. GitHub Pages sends Cache-Control: max-age=600;
    // Safari will reuse that response for the same URL unless the query changes.
    abs.searchParams.set("t", String(Date.now()));
    return abs;
  }

  async function poll() {
    const url = liveUrl();
    const note = $("analysis-note");
    const busted = bustedStatusUrl(url);
    const sameOrigin = busted.origin === location.origin;
    try {
      const res = await fetch(busted.href, {
        cache: "no-store",
        headers: sameOrigin ? { "Cache-Control": "no-cache", "Pragma": "no-cache" } : undefined,
      });
      if (!res.ok) throw new Error("HTTP " + res.status);
      const data = await res.json();
      applyStatus(data);
      const hhmmss = new Date().toTimeString().slice(0, 8);
      note.textContent = "Live status from " + url + " — polled every 5s — last ok " + hhmmss;
      return data;
    } catch (e) {
      const message = e && e.message ? e.message : String(e);
      render("error", { visual: "error", icon: "!", title: "STATUS UNREACHABLE", sub: message, foot: "live", led: "off" });
      $("log-title").textContent = "status.json · grokdesk";
      logEl.textContent = message + "\n" + url;
      note.textContent = "Live poll failed: " + message;
      throw e;
    }
  }

  function startLive() {
    document.body.classList.add("live");
    const tag = $("mode-tag");
    tag.textContent = "LIVE";
    tag.classList.add("live");
    const badge = $("mode-badge");
    badge.textContent = "LIVE";
    badge.classList.add("live");
    $("analysis-note").textContent = "Live status from " + liveUrl() + " — polled every 5s";
    const tickPoll = () => { poll().catch(() => {}); };
    tickPoll();
    setInterval(tickPoll, POLL_MS);
    window.DeskSim = { poll, live: true, url: liveUrl(), states: () => current };
  }

  function startDemo() {
    document.querySelectorAll("#controls [data-state]").forEach(b => b.onclick = () => go(b.dataset.state));
    $("auto").onclick = auto;
    window.DeskSim = { go, auto, states: () => current, live: false };
    states.healthy(true);
    if (params.has("autostart")) auto();
  }

  const tick = () => { $("scr-clock").textContent = new Date().toTimeString().slice(0, 5); };
  tick(); setInterval(tick, 10000);

  if (params.has("live")) startLive();
  else startDemo();
})();
