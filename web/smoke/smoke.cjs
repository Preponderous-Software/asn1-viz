// Smoke test for the browser build in headless Chromium (Playwright).
//
//   node web/smoke/smoke.cjs <url> <out-dir> [--phone [--dpr <n>]]
//
// Checks: the page starts with no page errors; every dataset opens from its
// button and draws (the canvas is not one flat colour) with its default
// selection; a real click (or tap) on the canvas goes through SDL and
// strata_raycast_meshes and changes the selection; and a click (or tap) on a
// block picks the same block as the page's own pick call at that point (CSS
// pixels, no SDL), so SDL's pointer units are scaled right at the profile's
// device pixel ratio (1 on the desktop, 3 on the iPhone 13; --dpr overrides
// the phone's). The compilation replay plays, pauses, steps a stage, and
// seeks from the page's range and from a click (or tap) on the canvas's own
// scrubber. Writes a PNG per dataset.
'use strict';
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { chromium, devices } = require('playwright');

const [url, outDir] = process.argv.slice(2);
const phone = process.argv.includes('--phone');
const dprArg = process.argv.indexOf('--dpr');
const dpr = dprArg > 0 ? Number(process.argv[dprArg + 1]) : 0;
if (!url || !outDir || (dprArg > 0 && !(dpr > 0))) {
  console.error('usage: smoke.cjs <url> <out-dir> [--phone [--dpr <n>]]');
  process.exit(2);
}
fs.mkdirSync(outDir, { recursive: true });

// Minimal PNG decoder (8-bit RGB/RGBA, non-interlaced: what Chromium writes).
function decodePng(buf) {
  let pos = 8, width = 0, height = 0, channels = 0;
  const idat = [];
  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos);
    const type = buf.toString('ascii', pos + 4, pos + 8);
    const data = buf.subarray(pos + 8, pos + 8 + len);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      channels = { 2: 3, 6: 4 }[data[9]];
      if (data[8] !== 8 || !channels || data[12] !== 0) throw new Error('unsupported PNG');
    } else if (type === 'IDAT') idat.push(data);
    pos += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = width * channels;
  const out = Buffer.alloc(height * stride);
  for (let y = 0; y < height; y++) {
    const f = raw[y * (stride + 1)];
    for (let x = 0; x < stride; x++) {
      const v = raw[y * (stride + 1) + 1 + x];
      const a = x >= channels ? out[y * stride + x - channels] : 0;
      const b = y > 0 ? out[(y - 1) * stride + x] : 0;
      const c = x >= channels && y > 0 ? out[(y - 1) * stride + x - channels] : 0;
      let p;
      if (f === 0) p = 0;
      else if (f === 1) p = a;
      else if (f === 2) p = b;
      else if (f === 3) p = (a + b) >> 1;
      else {
        const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c);
        p = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      out[y * stride + x] = (v + p) & 255;
    }
  }
  return { width, height, channels, data: out };
}

// Distinct colours (quantised to 4 bits per channel) in a PNG.
function colourCount(png) {
  const img = decodePng(png);
  const seen = new Set();
  for (let i = 0; i < img.data.length; i += img.channels)
    seen.add(((img.data[i] >> 4) << 8) | ((img.data[i + 1] >> 4) << 4) | (img.data[i + 2] >> 4));
  return seen.size;
}

const expected = [
  ['strips', 'Certificate.tbsCertificate.validity'],
  ['canonical', 'Certificate.tbsCertificate.serialNumber'],
  ['demo', 'Msg.period'],
  ['city', 'Certificate'],
  ['recursion', 'Everything'],
  // The replay opens at its start: two footprints and the empty library
  // plots draw fewer colours than a finished scene.
  ['compile', 'Msg', 20],
];

(async () => {
  const browser = await chromium.launch({ args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
  const base = phone ? { ...devices['iPhone 13'], ...(dpr ? { deviceScaleFactor: dpr } : {}) } : { viewport: { width: 1280, height: 800 } };
  const context = await browser.newContext({ ...base, userAgent: (base.userAgent || 'Mozilla/5.0') + ' asn1viz-smoke headless' });
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  page.on('console', (m) => { if (m.type() === 'error') errors.push('console: ' + m.text()); });
  const fail = async (why) => {
    console.error('FAIL: ' + why);
    await page.screenshot({ path: path.join(outDir, 'failure.png') }).catch(() => {});
    await browser.close();
    process.exit(1);
  };

  // The main loop keeps the load event from ever firing: wait for commit only.
  await page.goto(url, { waitUntil: 'commit' });
  try {
    await page.waitForFunction(() => window.asn1viz, null, { timeout: 90000 });
  } catch (e) {
    await fail('the viewer never became ready: ' + errors.join(' | '));
  }
  const status = async () => JSON.parse(await page.evaluate(() => window.asn1viz.status()));
  const canvas = page.locator('#canvas');

  for (let i = 0; i < expected.length; i++) {
    await page.locator(`[data-set="${i}"]`).click();
    await page.waitForTimeout(1500);
    const s = await status();
    if (s.dataset !== expected[i][0]) await fail(`button ${i} opened ${s.dataset}, not ${expected[i][0]}`);
    if (s.selected !== expected[i][1]) await fail(`${s.dataset}: selected ${s.selected}, not ${expected[i][1]}`);
    const shot = await canvas.screenshot({ path: path.join(outDir, `${expected[i][0]}.png`), timeout: 180000 });
    const colours = colourCount(shot);
    console.log(`${s.dataset}: selected ${s.selected}, ${colours} colours`);
    if (colours < (expected[i][2] || 40)) await fail(`${s.dataset} drew only ${colours} colours`);
  }

  // The compilation replay (RFC 0034 K1): it plays on opening; the page's
  // buttons pause it and step a stage; the range seeks; and a click or tap on
  // the scrubber drawn along the canvas's bottom seeks through SDL's pointer
  // (not a pick: the selection stays).
  {
    await page.locator('[data-set="5"]').click();
    await page.waitForTimeout(300);
    const r0 = (await status()).replay;
    await page.waitForTimeout(1500);
    const r1 = (await status()).replay;
    if (!r0 || !r1) await fail('the compilation view reports no replay state');
    console.log(`compile: ${r0.steps} steps, ${r0.stage} ${r0.pos} -> ${r1.stage} ${r1.pos}, playing ${r1.playing}`);
    if (!(r1.pos > r0.pos) || !r1.playing) await fail('the compilation replay is not playing');
    await page.locator('#play').click();
    await page.waitForTimeout(400);
    const paused = (await status()).replay;
    await page.waitForTimeout(600);
    const still = (await status()).replay;
    if (paused.playing || still.pos !== paused.pos) await fail('Pause did not stop the replay: ' + JSON.stringify([paused, still]));
    await page.locator('[data-key="48"]').click();  // next stage
    await page.waitForTimeout(400);
    const next = (await status()).replay;
    console.log(`next stage: ${paused.stage} -> ${next.stage}`);
    if (next.stage === paused.stage) await fail('the next-stage button did not move to another stage');
    await page.locator('#scrub').fill('500');
    await page.waitForTimeout(400);
    const mid = (await status()).replay;
    console.log(`range at 0.5: bar ${mid.bar.toFixed(3)} (${mid.stage})`);
    if (Math.abs(mid.bar - 0.5) > 0.01) await fail('the range did not seek: ' + JSON.stringify(mid));
    const b = await canvas.boundingBox();
    const sel = (await status()).selected;
    const x = b.x + b.width * 0.85, y = b.y + b.height - 12;
    if (phone) await page.touchscreen.tap(x, y);
    else await page.mouse.click(x, y);
    await page.waitForTimeout(600);
    const tapped = await status();
    console.log(`${phone ? 'tap' : 'click'} on the canvas scrubber at 85%: bar ${tapped.replay.bar.toFixed(3)} (${tapped.replay.stage}), selected ${tapped.selected}`);
    if (Math.abs(tapped.replay.bar - 0.85) > 0.08) await fail('a click on the canvas scrubber did not seek there: ' + JSON.stringify(tapped.replay));
    if (tapped.selected !== sel) await fail('a click on the scrubber changed the selection');
    await canvas.screenshot({ path: path.join(outDir, 'compile-seeked.png'), timeout: 180000 });
  }

  // A real click or tap on the canvas, through SDL and the ray cast. On the
  // demo the middle of the canvas is between the lanes: the selection clears.
  // A little left of the middle is the UPER lane: something is selected again.
  await page.locator('[data-set="2"]').click();
  await page.waitForTimeout(1000);
  const box = await canvas.boundingBox();
  const at = async (fx, fy) => {
    const x = box.x + box.width * fx, y = box.y + box.height * fy;
    if (phone) await page.touchscreen.tap(x, y);
    else await page.mouse.click(x, y);
    await page.waitForTimeout(800);
    return (await status()).selected;
  };
  const before = (await status()).selected;
  let after = before;
  for (const [fx, fy] of [[0.5, 0.25], [0.5, 0.5], [0.5, 0.75], [0.97, 0.97]]) {
    after = await at(fx, fy);
    if (after !== before) break;
  }
  console.log(`click: ${before || '(nothing)'} -> ${after || '(nothing)'}`);
  if (after === before) await fail('clicking the canvas never changed the selection');

  // A click or tap picks the block the page's own pick call finds at that
  // point. SDL reports the pointer in its window's units, which on a phone
  // with a capped device pixel ratio (3, capped at 2) are not CSS pixels: a
  // wrong scale picks a block up and to the left, or nothing. Re-opening the
  // demo before each probe selects Msg.period with the camera homed, so every
  // probe sees the same layout (on a phone, clearing the selection hides the
  // sheet and re-frames the view). Only points well inside a block are used:
  // the direct pick must agree 2 CSS pixels either way, so touch rounding at a
  // block's edge cannot fail the check.
  const devicePixelRatio = await page.evaluate(() => window.devicePixelRatio);
  const reopen = async () => {
    await page.evaluate(() => window.asn1viz.open(2));
    await page.waitForTimeout(400);
    return canvas.boundingBox();
  };
  const pickDirect = (x, y) => page.evaluate(([px, py]) => {
    window.asn1viz.click(px, py);
    return JSON.parse(window.asn1viz.status()).selected;
  }, [x, y]);
  let probed = 0;
  for (const fy of [0.47, 0.5, 0.53]) {
    for (let fx = 0.1; fx < 0.92 && probed < 3; fx += 0.08) {
      let b = await reopen();
      const cx = Math.round(b.width * fx), cy = Math.round(b.height * fy);
      const want = await pickDirect(cx, cy);
      if (!want || want === 'Msg.period') continue;
      let solid = true;
      for (const [dx, dy] of [[-2, 0], [2, 0], [0, -2], [0, 2]]) {
        if ((await pickDirect(cx + dx, cy + dy)) !== want) { solid = false; break; }
      }
      if (!solid) continue;
      b = await reopen();
      const x = b.x + cx, y = b.y + cy;
      if (phone) await page.touchscreen.tap(x, y);
      else await page.mouse.click(x, y);
      await page.waitForTimeout(800);
      const got = (await status()).selected;
      console.log(`pointer at DPR ${devicePixelRatio}: (${cx}, ${cy}) direct ${want}, ${phone ? 'tap' : 'click'} ${got || '(nothing)'}`);
      if (got !== want)
        await fail(`a ${phone ? 'tap' : 'click'} at (${cx}, ${cy}) picked ${got || 'nothing'}, but that point is ${want} (DPR ${devicePixelRatio})`);
      probed++;
    }
  }
  if (probed < 2) await fail(`only ${probed} points well inside a block were found on the demo to check the pointer with`);

  // The loading shell finished; and without WebGL 2 it shows the trailer (or
  // its still) and says why, and the engine never downloads.
  {
    await page.waitForFunction(() => { const r = document.querySelector('.sk-loader'); return r && r.hidden; }, null, { timeout: 10000 }).catch(() => {});
    const shell = await page.evaluate(() => {
      const r = document.querySelector('.sk-loader');
      return { hidden: !!(r && r.hidden), state: window.StrataLoader && StrataLoader.state };
    });
    if (!shell.hidden || shell.state !== 'done') await fail('the loading shell did not finish: ' + JSON.stringify(shell));
    console.log('ok: the loading shell showed and is gone');
    const bare = await browser.newContext({ ...base, userAgent: (base.userAgent || 'Mozilla/5.0') + ' asn1viz-smoke headless' });
    await bare.addInitScript(() => {
      const get = HTMLCanvasElement.prototype.getContext;
      HTMLCanvasElement.prototype.getContext = function (t, o) { return t === 'webgl2' ? null : get.call(this, t, o); };
    });
    const p2 = await bare.newPage();
    const engine = [];
    p2.on('request', (r) => { if (/asn1-viz\.(wasm|js)$/.test(r.url())) engine.push(r.url()); });
    await p2.goto(url, { waitUntil: 'commit' });
    await p2.waitForFunction(() => window.StrataLoader && StrataLoader.state === 'fallback', null, { timeout: 30000 });
    const fb = await p2.evaluate(() => {
      const r = document.querySelector('.sk-loader');
      return { text: r.innerText, media: !!r.querySelector('video, img'), wide: document.documentElement.scrollWidth > innerWidth };
    });
    await p2.screenshot({ path: path.join(outDir, 'no-webgl2.png') });
    if (!/WebGL 2/.test(fb.text) || fb.wide) await fail('without WebGL 2 the shell did not say why: ' + JSON.stringify(fb));
    if (engine.length) await fail('without WebGL 2 the engine still downloaded: ' + engine.join(', '));
    console.log('ok: without WebGL 2 the shell ' + (fb.media ? 'shows the trailer and ' : '') + 'says why, and nothing of the engine downloads');
    await bare.close();
  }

  if (errors.length) await fail('page errors: ' + errors.join(' | '));
  await page.screenshot({ path: path.join(outDir, 'page.png') });
  await browser.close();
  console.log('ok');
})().catch((e) => { console.error(e); process.exit(1); });
