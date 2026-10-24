// ?bench: a fixed, scripted frame-rate measurement for a Strata page.
//
// Open the page with ?bench and, once it has loaded, it flies a fixed camera
// path for 15 seconds (a 1-second warm-up first), timing every frame, then
// shows a card with the results and a "Copy results" button. Nothing is sent
// anywhere: the numbers stay on the screen and on the clipboard.
//
// What is measured, per frame:
//  - the frame interval: the time between requestAnimationFrame callbacks
//    (capped by the display's refresh rate, so 60 or 120 fps is the ceiling);
//  - the page's own work: how long the page's requestAnimationFrame
//    callbacks (the engine's frame) ran on the main thread;
//  - GPU time: what the engine's frame took on the GPU, from WebGL 2 timer
//    queries (EXT_disjoint_timer_query_webgl2), where the browser offers them.
//
// This file must load before the engine's script: in bench mode it wraps
// window.requestAnimationFrame so the page's frames can be timed.
//
// The page calls StrataBench.run({...}) once its engine is ready:
//   name     the page's name, for the results
//   scene    what is drawn (a dataset, a world, a replay)
//   canvas   the engine's canvas
//   setup()  puts the page in its fixed starting state (optional)
//   path(u)  puts the camera where the path is at u (0 at the start of the
//            warm-up, 1 at the end of the run); called once per frame
//   onDone() (optional) after the card is shown
// It returns false when the page was not opened with ?bench.
'use strict';
(function () {
  const params = new URLSearchParams(location.search);
  const requested = params.has('bench');
  const WARMUP_MS = 1000;
  const RUN_MS = Math.min(60000, Math.max(3000, (Number(params.get('bench')) || 15) * 1000));

  const nativeRaf = window.requestAnimationFrame.bind(window);
  // The page's frames: [{ts, cpu}] while measuring, with the GPU queries.
  let measuring = false;
  let gl = null, timer = null, activeQuery = null;
  const pendingQueries = [];  // [query, rAF timestamp]
  const gpuByFrame = new Map();  // rAF timestamp -> GPU ms of the page's callbacks
  let gpuDisjoint = 0;
  const cpuByFrame = new Map();  // rAF timestamp -> ms of the page's callbacks

  if (requested) {
    window.requestAnimationFrame = function (cb) {
      return nativeRaf(function (ts) {
        if (!measuring) return cb(ts);
        let query = null;
        if (timer && !activeQuery) {
          query = gl.createQuery();
          gl.beginQuery(timer.TIME_ELAPSED_EXT, query);
          activeQuery = query;
        }
        const t0 = performance.now();
        try {
          return cb(ts);
        } finally {
          cpuByFrame.set(ts, (cpuByFrame.get(ts) || 0) + performance.now() - t0);
          if (query) {
            gl.endQuery(timer.TIME_ELAPSED_EXT);
            activeQuery = null;
            pendingQueries.push([query, ts]);
          }
        }
      });
    };
  }

  function pollQueries() {
    if (!timer) return;
    for (let i = 0; i < pendingQueries.length;) {
      const [q, ts] = pendingQueries[i];
      if (!gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE)) { i++; continue; }
      if (gl.getParameter(timer.GPU_DISJOINT_EXT)) gpuDisjoint++;
      else gpuByFrame.set(ts, (gpuByFrame.get(ts) || 0) + gl.getQueryParameter(q, gl.QUERY_RESULT) / 1e6);
      gl.deleteQuery(q);
      pendingQueries.splice(i, 1);
    }
  }

  function stats(values) {
    if (!values.length) return null;
    const s = values.slice().sort((a, b) => a - b);
    const at = (p) => s[Math.min(s.length - 1, Math.max(0, Math.ceil(p * s.length) - 1))];
    return { n: s.length, median: at(0.5), p95: at(0.95), p99: at(0.99), max: s[s.length - 1] };
  }
  const ms = (v) => (v === null || v === undefined ? 'n/a' : v.toFixed(2) + ' ms');

  function rendererInfo(context) {
    if (!context) return { renderer: 'n/a', vendor: 'n/a' };
    const dbg = context.getExtension('WEBGL_debug_renderer_info');
    if (dbg) {
      return { renderer: String(context.getParameter(dbg.UNMASKED_RENDERER_WEBGL)),
        vendor: String(context.getParameter(dbg.UNMASKED_VENDOR_WEBGL)) };
    }
    return { renderer: String(context.getParameter(context.RENDERER)) + ' (unmasked name not exposed)',
      vendor: String(context.getParameter(context.VENDOR)) };
  }

  async function pageVersion() {
    // The arcade serves the deployed version next to the page.
    try {
      const r = await fetch('version.txt', { cache: 'no-store' });
      const t = r.ok ? (await r.text()).trim() : '';
      if (t && t.length < 80 && !/[<\s]/.test(t)) return t;
    } catch (e) { /* a local build */ }
    return 'local build';
  }

  // ---- the card ----
  const CSS = `
#strata-bench, #strata-bench-pill { position: fixed; z-index: 2147483000; font: 15px/1.4 system-ui, -apple-system, "Segoe UI", sans-serif;
  color: #e9edf4; background: #0e1118; border: 1px solid #39425a; border-radius: 12px;
  -webkit-user-select: text; user-select: text; box-shadow: 0 8px 28px rgba(0,0,0,0.45); }
#strata-bench-pill { top: max(10px, env(safe-area-inset-top)); left: 50%; transform: translateX(-50%); padding: 6px 12px;
  font-size: 13px; pointer-events: none; white-space: nowrap; }
#strata-bench { left: max(12px, env(safe-area-inset-left)); right: max(12px, env(safe-area-inset-right));
  top: max(12px, env(safe-area-inset-top)); max-width: 560px; margin: 0 auto; padding: 16px;
  max-height: calc(100% - 24px); overflow-y: auto; -webkit-overflow-scrolling: touch; }
#strata-bench h2 { margin: 0 0 4px; font-size: 18px; }
#strata-bench .big { font-size: 30px; font-weight: 700; font-variant-numeric: tabular-nums; }
#strata-bench .dim { color: #a7b0c2; font-size: 13px; }
#strata-bench dl { display: grid; grid-template-columns: auto 1fr; gap: 4px 12px; margin: 12px 0; }
#strata-bench dt { color: #a7b0c2; }
#strata-bench dd { margin: 0; font-variant-numeric: tabular-nums; overflow-wrap: anywhere; }
#strata-bench textarea { width: 100%; box-sizing: border-box; min-height: 9em; font: 12px/1.35 ui-monospace, Menlo, Consolas, monospace;
  color: inherit; background: #0a0c11; border: 1px solid #39425a; border-radius: 8px; padding: 8px; resize: vertical; }
#strata-bench .row { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 10px; }
#strata-bench-shade { position: fixed; inset: 0; z-index: 2147482999; background: rgba(0, 0, 0, 0.55); }
@media (max-width: 640px) { #strata-bench { padding: 12px 14px; } #strata-bench .big { font-size: 26px; } #strata-bench dl { margin: 8px 0; } }
#strata-bench button { font: 600 15px system-ui, sans-serif; min-height: 44px; padding: 0 16px; border-radius: 8px; cursor: pointer;
  color: #e9edf4; background: #232a3a; border: 1px solid #4a5572; touch-action: manipulation; }
#strata-bench button.primary { background: #2f6fde; border-color: #2f6fde; color: #fff; flex: 1 1 auto; }
#strata-bench button:focus-visible { outline: 2px solid #ffc23a; outline-offset: 2px; }
#strata-bench .note { margin-top: 8px; min-height: 1.4em; color: #ffc23a; font-size: 13px; }`;

  function el(tag, attrs, text) {
    const e = document.createElement(tag);
    for (const k in attrs || {}) e.setAttribute(k, attrs[k]);
    if (text !== undefined) e.textContent = text;
    return e;
  }

  function showCard(r) {
    const card = el('div', { id: 'strata-bench', role: 'dialog', 'aria-label': 'Benchmark results' });
    card.append(el('h2', {}, `${r.name} benchmark`));
    card.append(el('div', { class: 'dim' }, `${r.scene} · ${r.version}`));
    card.append(el('div', { class: 'big' }, `${r.fps.toFixed(1)} fps`));
    // The button first: on a phone it is on the screen without scrolling.
    const row = el('div', { class: 'row' });
    const copy = el('button', { type: 'button', class: 'primary' }, 'Copy results');
    const again = el('button', { type: 'button' }, 'Run again');
    const close = el('button', { type: 'button' }, 'Close');
    row.append(copy, again, close);
    card.append(row);
    const note = el('div', { class: 'note', 'aria-live': 'polite' });
    card.append(note);
    const dl = el('dl');
    const add = (k, v) => dl.append(el('dt', {}, k), el('dd', {}, v));
    add('Frame time', `median ${ms(r.interval && r.interval.median)}, p95 ${ms(r.interval && r.interval.p95)}`);
    add('Frames', `${r.frames} in ${(r.elapsedMs / 1000).toFixed(1)} s, ${r.slowFrames} over 2× median`);
    add('Page work', r.cpu ? `median ${ms(r.cpu.median)}, p95 ${ms(r.cpu.p95)}` : 'n/a');
    add('GPU time', r.gpu ? `median ${ms(r.gpu.median)}, p95 ${ms(r.gpu.p95)}` : r.gpuNote);
    add('Canvas', r.canvas);
    add('Renderer', r.renderer);
    card.append(dl);
    if (r.warning) card.append(el('div', { class: 'note' }, r.warning));
    const text = el('textarea', { readonly: '', rows: '8', 'aria-label': 'Results as text', spellcheck: 'false' });
    text.value = r.text;
    card.append(text);
    const selectAll = () => { text.focus(); text.select(); text.setSelectionRange(0, text.value.length); };
    copy.addEventListener('click', () => {
      const fallback = () => {
        selectAll();
        let ok = false;
        try { ok = document.execCommand('copy'); } catch (e) { ok = false; }
        note.textContent = ok ? 'Copied.' : 'Selected: copy it with Ctrl+C or the Copy menu.';
      };
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(r.text).then(() => { note.textContent = 'Copied.'; }, fallback);
      } else {
        fallback();
      }
    });
    again.addEventListener('click', () => location.reload());
    const shade = el('div', { id: 'strata-bench-shade' });
    close.addEventListener('click', () => { card.remove(); shade.remove(); });
    // The engine's canvas takes key and pointer events; the card keeps its own.
    for (const type of ['keydown', 'keyup', 'pointerdown', 'mousedown', 'touchstart', 'wheel'])
      card.addEventListener(type, (e) => e.stopPropagation());
    document.body.append(shade, card);
    return card;
  }

  async function run(opts) {
    if (!requested) return false;
    const state = { state: 'running', result: null, text: '' };
    window.__strataBench = state;
    const style = el('style');
    style.textContent = CSS;
    document.head.append(style);
    const pill = el('div', { id: 'strata-bench-pill', 'aria-live': 'polite' }, 'Benchmark: starting…');
    document.body.append(pill);

    gl = opts.canvas.getContext('webgl2');
    timer = gl && gl.getExtension('EXT_disjoint_timer_query_webgl2');
    const info = rendererInfo(gl);
    const versionP = pageVersion();
    try { if (opts.setup) opts.setup(); } catch (e) { console.warn('bench setup:', e); }

    const intervals = [];
    let hidden = false;
    const onHidden = () => { if (document.hidden) hidden = true; };
    document.addEventListener('visibilitychange', onHidden);
    const total = WARMUP_MS + RUN_MS;
    let t0 = null, last = null, lastSecond = -1, runStart = null, runFrames = 0;

    await new Promise((resolve) => {
      const tick = (ts) => {
        const now = performance.now();
        if (t0 === null) t0 = now;
        const elapsed = now - t0;
        try { opts.path(Math.min(1, elapsed / total)); } catch (e) { console.warn('bench path:', e); }
        if (elapsed >= WARMUP_MS) {
          if (runStart === null) { runStart = ts; measuring = true; cpuByFrame.clear(); }
          else { intervals.push(ts - last); runFrames++; }
        }
        last = ts;
        pollQueries();
        const left = Math.ceil((total - elapsed) / 1000);
        if (left !== lastSecond) {
          lastSecond = left;
          pill.textContent = elapsed < WARMUP_MS ? 'Benchmark: warming up…' : `Benchmark: ${left} s left`;
        }
        if (elapsed >= total) { measuring = false; resolve(); return; }
        nativeRaf(tick);
      };
      nativeRaf(tick);
    });
    // The last GPU results arrive a frame or two later.
    for (let i = 0; i < 30 && pendingQueries.length; i++) {
      await new Promise((r) => setTimeout(r, 16));
      pollQueries();
    }
    document.removeEventListener('visibilitychange', onHidden);
    pill.remove();

    const interval = stats(intervals);
    const cpu = stats([...cpuByFrame.values()]);
    // A callback that drew nothing (not the engine's) times 0 on the GPU.
    const gpu = stats([...gpuByFrame.values()].filter((v) => v > 0));
    const elapsedMs = intervals.reduce((a, b) => a + b, 0);
    const fps = elapsedMs > 0 ? (1000 * runFrames) / elapsedMs : 0;
    const slowFrames = interval ? intervals.filter((v) => v > 2 * interval.median).length : 0;
    const dpr = window.devicePixelRatio || 1;
    const rect = opts.canvas.getBoundingClientRect();
    const drawW = gl ? gl.drawingBufferWidth : opts.canvas.width, drawH = gl ? gl.drawingBufferHeight : opts.canvas.height;
    const canvas = `${Math.round(rect.width)}×${Math.round(rect.height)} CSS px × DPR ${+dpr.toFixed(3)} → drawn at ${drawW}×${drawH}`;
    const gpuNote = timer ? 'no results (timer queries disjoint)' : 'n/a (no EXT_disjoint_timer_query_webgl2)';
    const version = await versionP;
    let warning = '';
    if (hidden) warning = 'The tab was hidden during the run: these numbers are not comparable. Run again with the page in view.';
    else if (runFrames < 30) warning = 'Very few frames were drawn: the page may have stalled.';
    const result = {
      name: opts.name, scene: opts.scene, version, fps, frames: runFrames, elapsedMs, interval, cpu, gpu, gpuNote,
      slowFrames, canvas, renderer: info.renderer, vendor: info.vendor, warning,
      gpuDisjoint, userAgent: navigator.userAgent, when: new Date().toISOString(), url: location.href,
    };
    const lines = [
      `strata-bench 1 · ${opts.name} ${version}`,
      `scene: ${opts.scene} (${RUN_MS / 1000} s path after a ${WARMUP_MS / 1000} s warm-up)`,
      `fps: ${fps.toFixed(1)} (${runFrames} frames in ${(elapsedMs / 1000).toFixed(2)} s)`,
      `frame time: median ${ms(interval && interval.median)}, p95 ${ms(interval && interval.p95)}, p99 ${ms(interval && interval.p99)}, max ${ms(interval && interval.max)}; ${slowFrames} over 2× median`,
      `page work: ${cpu ? `median ${ms(cpu.median)}, p95 ${ms(cpu.p95)}, max ${ms(cpu.max)}` : 'n/a'}`,
      `GPU time: ${gpu ? `median ${ms(gpu.median)}, p95 ${ms(gpu.p95)}, max ${ms(gpu.max)} (${gpu.n} frames${gpuDisjoint ? `, ${gpuDisjoint} disjoint` : ''})` : gpuNote}`,
      `canvas: ${canvas}`,
      `renderer: ${info.renderer}`,
      `vendor: ${info.vendor}`,
      `user agent: ${navigator.userAgent}`,
      `screen: ${screen.width}×${screen.height}, hardware threads: ${navigator.hardwareConcurrency || 'n/a'}`,
      `when: ${result.when}`,
    ];
    if (warning) lines.push(`warning: ${warning}`);
    result.text = lines.join('\n');
    state.result = result;
    state.text = result.text;
    console.log(result.text);
    showCard(result);
    state.state = 'done';
    if (opts.onDone) opts.onDone(result);
    return true;
  }

  window.StrataBench = { requested, run, durationMs: WARMUP_MS + RUN_MS };
})();
