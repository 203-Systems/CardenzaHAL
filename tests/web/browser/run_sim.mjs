// Runs a CardenzaHAL web build (the folder tools/cardenza_web.py writes) in headless Chrome the way the
// community shows it, and checks that it boots.
//
//   node tests/web/browser/run_sim.mjs --site <dir> [--out <screenshot dir>] [--name <label>]
//        [--keys '<json steps>'] [--expect-led x,y] [--expect-pixels] [--expect-sound] [--app '<json bytes>']
//        [--timeout <ms>]
//
// 1. The site is served from one origin (Access-Control-Allow-Origin: *, application/wasm) and shown
//    from another, in <iframe sandbox="allow-scripts" allow="autoplay">: it must post
//    {"cardenza_sim":"ready"} within the timeout (15 s, the community's limit) and then its key table,
//    which must equal the site's keymap in sim.json when there is one. With --app the host sends
//    {"cardenza_sim":"app","data":[...]} into the frame and records the app_report answers.
// 2. The same site opened top-level (cross-origin frames cannot be driven) runs the key steps through
//    window.cardenzaSim: [{"key":[x,y],"down":true,"waitMs":300,"shot":"name"},{"home":true},{"waitMs":500}].
// Screenshots go to the out folder; a JSON summary goes to stdout; the exit code is 1 on a failure.
// Playwright comes from PLAYWRIGHT_DIR (default /tmp/claude-1000/site-check/node_modules/playwright),
// Chrome from CHROME (default /usr/bin/google-chrome).

import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';

const args = {};
for (let index = 2; index < process.argv.length; index++) {
  const name = process.argv[index];
  if (!name.startsWith('--')) continue;
  const next = process.argv[index + 1];
  if (next === undefined || next.startsWith('--')) args[name.slice(2)] = true;
  else { args[name.slice(2)] = next; index++; }
}
if (!args.site) {
  console.error('usage: run_sim.mjs --site <dir> [--out <dir>] [--keys <json>] [--expect-led x,y] [--expect-pixels] [--timeout ms]');
  process.exit(2);
}
const site = path.resolve(args.site);
const out = path.resolve(args.out || 'sim-shots');
const label = args.name || path.basename(site);
const timeout = Number(args.timeout || 15000);
const playwrightDir = process.env.PLAYWRIGHT_DIR || '/tmp/claude-1000/site-check/node_modules/playwright';
const { chromium } = await import(path.join(playwrightDir, 'index.mjs'));
fs.mkdirSync(out, { recursive: true });

const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.wasm': 'application/wasm', '.json': 'application/json',
  '.data': 'application/octet-stream', '.png': 'image/png', '.css': 'text/css' };

function serve(handler) {
  return new Promise((resolve) => {
    const server = http.createServer(handler);
    server.listen(0, '127.0.0.1', () => resolve(server));
  });
}

const siteServer = await serve((request, response) => {
  const url = new URL(request.url, 'http://x');
  const file = path.join(site, decodeURIComponent(url.pathname === '/' ? '/index.html' : url.pathname));
  if (!file.startsWith(site) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    response.writeHead(404, { 'Access-Control-Allow-Origin': '*' });
    response.end('not found');
    return;
  }
  response.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream', 'Access-Control-Allow-Origin': '*',
    'Cache-Control': 'no-store' });
  fs.createReadStream(file).pipe(response);
});
const siteUrl = `http://127.0.0.1:${siteServer.address().port}/`;

// The player: another origin, the build in a sandboxed frame, letterboxed on black.
const hostServer = await serve((request, response) => {
  response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
  response.end(`<!doctype html><html><head><meta charset="utf-8"><style>html,body{margin:0;height:100%;background:#000}
iframe{border:0;width:100vw;height:100vh;display:block}</style></head><body>
<iframe id="sim" sandbox="allow-scripts" allow="autoplay" src="${siteUrl}index.html"></iframe>
<script>window.__start = performance.now(); window.__messages = [];
addEventListener('message', (e) => window.__messages.push({ at: performance.now() - window.__start, data: e.data }));</script>
</body></html>`);
});
const hostUrl = `http://127.0.0.1:${hostServer.address().port}/`;

const summary = { name: label, site, ok: true, failures: [], screenshots: [] };
function fail(message) { summary.ok = false; summary.failures.push(message); }

const browser = await chromium.launch({ executablePath: process.env.CHROME || '/usr/bin/google-chrome', headless: true,
  args: ['--autoplay-policy=no-user-gesture-required'] });
try {
  const context = await browser.newContext({ viewport: { width: 1050, height: 600 } });

  // ---- 1. in the player's sandboxed frame ----
  const host = await context.newPage();
  const hostErrors = [];
  host.on('console', (message) => { if (message.type() === 'error') hostErrors.push(message.text()); });
  host.on('pageerror', (error) => hostErrors.push(String(error)));
  await host.goto(hostUrl);
  const deadline = Date.now() + timeout;
  let ready = null;
  while (Date.now() < deadline) {
    const messages = await host.evaluate(() => window.__messages);
    ready = messages.find((message) => message.data && message.data.cardenza_sim === 'ready');
    if (ready) break;
    await host.waitForTimeout(100);
  }
  if (!ready) fail(`no {"cardenza_sim":"ready"} within ${timeout} ms in the sandboxed frame`);
  else summary.readyMs = Math.round(ready.at);
  await host.waitForTimeout(1200);
  const messages = await host.evaluate(() => window.__messages);
  const keys = messages.find((message) => message.data && message.data.cardenza_sim === 'keys');
  if (!keys) fail('no {"cardenza_sim":"keys"} message');
  else {
    summary.keyRows = keys.data.keys.length;
    if (keys.data.keys.length > 64) fail('the key table has more than 64 rows');
    const simJson = path.join(site, 'sim.json');
    if (fs.existsSync(simJson)) {
      const manifest = JSON.parse(fs.readFileSync(simJson, 'utf8'));
      if (manifest.keys && JSON.stringify(manifest.keys) !== JSON.stringify(keys.data.keys)) fail('the posted key table differs from sim.json');
    }
  }
  if (args.app) {
    const bytes = JSON.parse(args.app);
    await host.evaluate((data) => document.getElementById('sim').contentWindow.postMessage({ cardenza_sim: 'app', data }, '*'), bytes);
    await host.waitForTimeout(500);
    const replies = (await host.evaluate(() => window.__messages)).filter((message) => message.data && message.data.cardenza_sim === 'app_report');
    summary.appReplies = replies.map((message) => message.data.data);
    if (!replies.length) fail('no app_report answer to the app message');
  }
  const frameShot = path.join(out, `${label}-sandboxed.png`);
  await host.screenshot({ path: frameShot });
  summary.screenshots.push(frameShot);
  summary.sandboxErrors = hostErrors;

  // ---- 2. top-level, driven through window.cardenzaSim ----
  const page = await context.newPage();
  const pageErrors = [];
  page.on('console', (message) => { if (message.type() === 'error') pageErrors.push(message.text()); });
  page.on('pageerror', (error) => pageErrors.push(String(error)));
  await page.goto(siteUrl + 'index.html');
  await page.evaluate((limit) => Promise.race([window.cardenzaSim.ready, new Promise((_, reject) => setTimeout(() => reject(new Error('not ready')), limit))]), timeout);
  await page.mouse.click(5, 5); // a gesture: sound may start
  await page.waitForTimeout(500);
  const steps = args.keys ? JSON.parse(args.keys) : [];
  for (const step of steps) {
    if (step.key) await page.evaluate(([x, y, down]) => window.cardenzaSim.key(x, y, down), [step.key[0], step.key[1], step.down !== false]);
    if (step.home !== undefined) await page.evaluate((down) => window.cardenzaSim.home(down), !!step.home);
    if (step.waitMs) await page.waitForTimeout(step.waitMs);
    if (step.shot) {
      const shot = path.join(out, `${label}-${step.shot}.png`);
      await page.screenshot({ path: shot });
      summary.screenshots.push(shot);
    }
    if (step.ledsName) summary[step.ledsName] = await page.evaluate(() => window.cardenzaSim.leds());
  }
  const state = await page.evaluate(() => {
    const frame = window.cardenzaSim.frame();
    let lit = 0;
    for (const pixel of frame.pixels) if (pixel !== 0) lit++;
    return { lit, backlight: frame.backlight, showing: frame.showing, leds: window.cardenzaSim.leds(), audio: window.cardenzaSim.audio(),
      tasks: window.cardenzaSim.tasks(), logs: window.cardenzaSim.logs.slice(-25), resets: window.cardenzaSim.logs.filter((line) => /reset reason|restart|stopped/.test(line)) };
  });
  Object.assign(summary, { litPixels: state.lit, backlight: state.backlight, showing: state.showing, audio: state.audio,
    tasks: state.tasks.trim().split("\n"), logTail: state.logs, lifecycle: state.resets, pageErrors });
  const topShot = path.join(out, `${label}-top.png`);
  await page.screenshot({ path: topShot });
  summary.screenshots.push(topShot);
  if (args['expect-pixels'] && !(state.lit > 0 && state.showing && state.backlight > 0)) fail('the screen shows nothing');
  if (args['expect-sound'] && !(state.audio && state.audio.peakRms > 0.001)) fail('no sound came out');
  if (args['expect-led']) {
    const [x, y] = String(args['expect-led']).split(',').map(Number);
    const level = (summary.ledsHeld || state.leds).levels[y * 14 + x];
    if (!(level > 0)) fail(`the LED of key ${x},${y} did not light while it was held`);
  }
  const errors = [...hostErrors, ...pageErrors].filter((text) => !/favicon/.test(text));
  if (errors.length) fail('console errors: ' + errors.join(' | '));
} catch (error) {
  fail(String(error && error.stack ? error.stack : error));
} finally {
  await browser.close();
  siteServer.close();
  hostServer.close();
}
console.log(JSON.stringify(summary, null, 2));
process.exit(summary.ok ? 0 : 1);
