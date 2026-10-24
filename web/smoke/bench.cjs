// Smoke test for the page's ?bench mode in headless Chromium (Playwright).
//
//   node bench.cjs <url with ?bench=N> <out-dir> [--phone]
//
// Checks that the scripted run finishes, that it timed frames, and that the
// results card is on the screen with its numbers and a "Copy results" button
// that fits a finger (44 CSS px) and puts the results on the clipboard. On a
// phone (the iPhone 13 profile) the card must fit the screen's width.
//
// The numbers themselves are not checked or kept: headless Chromium draws
// with SwiftShader, on the CPU, so its frame rate says nothing about a GPU.
'use strict';
const fs = require('fs');
const path = require('path');
const { chromium, devices } = require('playwright');

const [url, outDir] = process.argv.slice(2);
const phone = process.argv.includes('--phone');
if (!url || !outDir || !/[?&]bench/.test(url)) {
  console.error('usage: bench.cjs <url with ?bench=N> <out-dir> [--phone]');
  process.exit(2);
}
fs.mkdirSync(outDir, { recursive: true });

(async () => {
  const browser = await chromium.launch({ args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
  const base = phone ? devices['iPhone 13'] : { viewport: { width: 1280, height: 800 } };
  const context = await browser.newContext({ ...base, userAgent: (base.userAgent || 'Mozilla/5.0') + ' bench-smoke headless' });
  const origin = new URL(url).origin;
  await context.grantPermissions(['clipboard-read', 'clipboard-write'], { origin }).catch(() => {});
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  page.on('console', (m) => { if (m.type() === 'error' && !/version\.txt|404/.test(m.text())) errors.push('console: ' + m.text()); });
  const fail = async (why) => {
    console.error('FAIL: ' + why);
    await page.screenshot({ path: path.join(outDir, 'failure.png') }).catch(() => {});
    await browser.close();
    process.exit(1);
  };

  // An Emscripten main loop can keep the load event from firing: commit only.
  await page.goto(url, { waitUntil: 'commit' });
  try {
    await page.waitForFunction(() => window.__strataBench && window.__strataBench.state === 'done', null, { timeout: 420000 });
  } catch (e) {
    await fail('the bench run never finished: ' + errors.join(' | '));
  }
  const r = await page.evaluate(() => window.__strataBench.result);
  console.log(r.text.split('\n').map((l) => '  ' + l).join('\n'));
  if (!(r.frames > 0) || !(r.fps > 0)) await fail(`no frames were timed (${r.frames} frames, ${r.fps} fps)`);
  if (!r.interval || !(r.interval.median > 0) || !(r.interval.p95 >= r.interval.median)) await fail('no frame-time statistics');
  if (!r.cpu || !(r.cpu.n > 0)) await fail("the page's own frames were not timed (is bench.js loaded before the engine?)");
  for (const key of ['renderer', 'canvas', 'version', 'userAgent']) if (!r[key]) await fail(`the results have no ${key}`);
  for (const want of ['fps:', 'frame time: median', 'canvas:', 'renderer:', 'user agent:']) {
    if (!r.text.includes(want)) await fail(`the results text has no "${want}"`);
  }

  const card = page.locator('#strata-bench');
  if (!(await card.isVisible())) await fail('the results card is not shown');
  const cardText = await card.innerText();
  if (!/\d+(\.\d)? fps/.test(cardText)) await fail('the card shows no fps');
  const copy = card.getByRole('button', { name: 'Copy results' });
  const box = await copy.boundingBox();
  if (!box || box.height < 44) await fail(`the Copy results button is ${box ? box.height : 0} px tall (under 44)`);
  const vh = page.viewportSize().height;
  if (box.y < 0 || box.y + box.height > vh) await fail(`the Copy results button (y ${box.y}) is off the ${vh} px screen without scrolling`);
  const cardBox = await card.boundingBox();
  const vw = page.viewportSize().width;
  if (cardBox.x < 0 || cardBox.x + cardBox.width > vw + 0.5) await fail(`the card (${cardBox.x}..${cardBox.x + cardBox.width}) does not fit the ${vw} px screen`);
  await page.screenshot({ path: path.join(outDir, 'card.png'), timeout: 120000 });

  if (phone) await copy.tap();
  else await copy.click();
  await page.waitForTimeout(500);
  const note = await card.locator('.note[aria-live]').innerText();
  const clip = await page.evaluate(() => navigator.clipboard.readText().catch(() => null));
  console.log(`copy: "${note}"${clip === null ? ' (clipboard not readable here)' : ''}`);
  if (!/Copied|Selected/.test(note)) await fail('Copy results said nothing');
  if (clip !== null && /Copied/.test(note) && clip !== r.text) await fail('the clipboard does not hold the results');

  if (errors.length) await fail('page errors: ' + errors.join(' | '));
  await browser.close();
  console.log('ok');
})().catch((e) => { console.error(e); process.exit(1); });
