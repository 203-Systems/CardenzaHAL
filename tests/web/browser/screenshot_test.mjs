// The page's screenshots (web/page/cardenza-sim.js), in headless Chrome against a stand-in firmware (no web build
// needed): its screen holds a known pattern, and the pictures must hold exactly that screen, never the board.
//
//   node tests/web/browser/screenshot_test.mjs
//
// 1. In the player's <iframe sandbox="allow-scripts"> from another origin: the page says
//    {"cardenza_sim":"features","features":["screenshot"]}; the host asks for the screen and for the framed picture
//    and checks every pixel of both (decoded in the host page); {"cardenza_sim":"host","screenshot":true} hides the
//    page's own buttons.
// 2. Top-level: SCREENSHOT and FRAMED download PNGs of 960 x 540 and 1280 x 720, and the keyboard stays the
//    firmware's (the buttons never take the focus).
// Playwright comes from PLAYWRIGHT_DIR (default /tmp/claude-1000/site-check/node_modules/playwright), Chrome from
// CHROME (default /usr/bin/google-chrome).
import assert from 'node:assert/strict';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const page = path.join(here, '..', '..', '..', 'web', 'page');
const site = fs.mkdtempSync(path.join(os.tmpdir(), 'cardenza-shot-'));

// The site as tools/cardenza_web.py writes it, with a stand-in firmware.
fs.writeFileSync(path.join(site, 'index.html'), fs.readFileSync(path.join(page, 'index.html'), 'utf8')
  .replaceAll('@CARDENZA_MODULE@', 'fake.js').replaceAll('@CARDENZA_TITLE@', 'Shot Test').replaceAll('@CARDENZA_VERSION@', '1.0.0'));
fs.copyFileSync(path.join(page, 'cardenza-sim.js'), path.join(site, 'cardenza-sim.js'));
fs.writeFileSync(path.join(site, 'fake.wasm'), Uint8Array.of(0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00));
// Its screen: red = x, green = y, blue = x ^ y (RGB565), at half backlight (the picture must not be dimmed).
fs.writeFileSync(path.join(site, 'fake.js'), `
window.createCardenzaFirmware = async (config) => {
  const HEAPU8 = new Uint8Array(1 << 20), HEAPU32 = new Uint32Array(HEAPU8.buffer), HEAPF32 = new Float32Array(HEAPU8.buffer);
  const DISPLAY = 4096, LEDS = 1024;
  HEAPU32[DISPLAY >> 2] = 1; HEAPU32[(DISPLAY >> 2) + 1] = 500; HEAPU32[(DISPLAY >> 2) + 2] = 1;
  const pixels = new Uint16Array(HEAPU8.buffer, DISPLAY + 12, 240 * 135);
  for (let y = 0; y < 135; y++) for (let x = 0; x < 240; x++) pixels[y * 240 + x] = ((x & 31) << 11) | ((y & 63) << 5) | ((x ^ y) & 31);
  const module = {
    HEAPU8, HEAPU32, HEAPF32, FS: { mkdir() {}, mount() {}, syncfs(_, done) { if (typeof _ === 'function') _(null); else if (done) done(null); } }, IDBFS: {},
    addRunDependency() {}, removeRunDependency() {}, UTF8ToString: () => '',
    _cardenza_web_display: () => DISPLAY, _cardenza_web_leds: () => LEDS, _cardenza_web_run() {}, _cardenza_web_next_delay: () => -1,
    _cardenza_web_audio_take: () => 0, _cardenza_web_audio_buffer: () => 0, _cardenza_web_audio_position: () => 0,
    _cardenza_web_mic_wanted: () => 0, _cardenza_web_mic_buffer: () => 0, _cardenza_web_mic_push() {}, _cardenza_web_midi_in() {},
    _cardenza_web_key(x, y, down) { window.__keys = (window.__keys || []).concat([[x, y, down]]); }, _cardenza_web_home() {},
    _cardenza_web_app_buffer: () => 0, _cardenza_web_app_in() {}, _cardenza_web_tasks: () => 0,
  };
  setTimeout(() => config.cardenzaBooted(), 50);
  return module;
};
`);

const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.wasm': 'application/wasm' };
function serve(handler) {
  return new Promise((resolve) => { const server = http.createServer(handler); server.listen(0, '127.0.0.1', () => resolve(server)); });
}
const siteServer = await serve((request, response) => {
  const file = path.join(site, decodeURIComponent(new URL(request.url, 'http://x').pathname));
  if (!file.startsWith(site) || !fs.existsSync(file)) { response.writeHead(404); response.end(); return; }
  response.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream', 'Access-Control-Allow-Origin': '*' });
  fs.createReadStream(file).pipe(response);
});
const siteUrl = `http://127.0.0.1:${siteServer.address().port}/index.html`;
// The host: another origin (localhost against 127.0.0.1), the page in a sandboxed frame; it decodes the pictures it
// gets back and reports what it found.
const hostServer = await serve((request, response) => {
  response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
  response.end(`<!doctype html><html><body style="margin:0;background:#000">
<iframe id="sim" sandbox="allow-scripts" style="border:0;width:1000px;height:572px" src="${siteUrl}"></iframe>
<script>
window.__messages = [];
addEventListener('message', (e) => window.__messages.push(e.data));
window.ask = (request) => new Promise((resolve) => {
  const id = 'r' + Math.random();
  const listen = async (e) => {
    if (!e.data || e.data.cardenza_sim !== 'screenshot' || e.data.id !== id) return;
    removeEventListener('message', listen);
    if (e.data.error) { resolve({ error: e.data.error }); return; }
    const bitmap = await createImageBitmap(new Blob([e.data.png], { type: 'image/png' }));
    const c = new OffscreenCanvas(bitmap.width, bitmap.height).getContext('2d');
    c.drawImage(bitmap, 0, 0);
    const data = c.getImageData(0, 0, bitmap.width, bitmap.height).data;
    const at = (x, y) => Array.from(data.slice((y * bitmap.width + x) * 4, (y * bitmap.width + x) * 4 + 3));
    resolve({ ...e.data, png: e.data.png.byteLength, bitmap: [bitmap.width, bitmap.height], pixel: (x, y) => at(x, y), data: Array.from(data) });
  };
  addEventListener('message', listen);
  document.getElementById('sim').contentWindow.postMessage({ cardenza_sim: 'screenshot', id, ...request }, '*');
});
</script></body></html>`);
});
const hostUrl = `http://localhost:${hostServer.address().port}/`;

const { chromium } = await import(path.join(process.env.PLAYWRIGHT_DIR || '/tmp/claude-1000/site-check/node_modules/playwright', 'index.mjs'));
const browser = await chromium.launch({ executablePath: process.env.CHROME || '/usr/bin/google-chrome', headless: true });
// The screen pixel (x, y) of the stand-in, as 8-bit RGB the way the page widens RGB565.
const expected = (x, y) => { const r = x & 31, g = y & 63, b = (x ^ y) & 31; return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)]; };
function checkScreen(data, width, ox, oy, label) {
  for (let y = 0; y < 135 * 4; y++) {
    for (let x = 0; x < 240 * 4; x++) {
      const i = ((oy + y) * width + ox + x) * 4;
      const want = expected(x >> 2, y >> 2);
      if (data[i] !== want[0] || data[i + 1] !== want[1] || data[i + 2] !== want[2]) {
        assert.fail(`${label}: pixel ${x},${y} is ${data.slice(i, i + 3)}, not ${want} (screen pixel ${x >> 2},${y >> 2})`);
      }
    }
  }
}
try {
  const context = await browser.newContext({ viewport: { width: 1000, height: 572 }, acceptDownloads: true });

  // ---- 1. in a sandboxed frame ----
  const host = await context.newPage();
  await host.goto(hostUrl);
  await host.waitForFunction(() => window.__messages.some((m) => m && m.cardenza_sim === 'features'), null, { timeout: 15000 });
  const said = await host.evaluate(() => window.__messages.map((m) => m.cardenza_sim));
  assert.deepEqual(said.slice(0, 3), ['ready', 'keys', 'features'], 'ready, its keys, then what it can do');
  assert.deepEqual(await host.evaluate(() => window.__messages.find((m) => m.cardenza_sim === 'features').features), ['screenshot']);
  await host.waitForTimeout(200);   // a frame drawn

  const bare = await host.evaluate(async () => { const r = await window.ask({}); delete r.pixel; return r; });
  assert.equal(bare.error, undefined);
  assert.deepEqual([bare.width, bare.height, bare.framed, bare.color], [960, 540, false, null]);
  assert.deepEqual(bare.bitmap, [960, 540]);
  assert.match(bare.name, /^shot-test-screen-\d{8}-\d{6}\.png$/);
  checkScreen(bare.data, 960, 0, 0, 'the screenshot');

  const framed = await host.evaluate(async () => { const r = await window.ask({ framed: true, color: '#3b82e6' }); delete r.pixel; return r; });
  assert.deepEqual([framed.width, framed.height, framed.framed, framed.color], [1280, 720, true, '#3B82E6']);
  assert.match(framed.name, /^shot-test-framed-\d{8}-\d{6}\.png$/);
  checkScreen(framed.data, 1280, 160, 90, 'the framed picture');
  const px = (x, y) => framed.data.slice((y * 1280 + x) * 4, (y * 1280 + x) * 4 + 3);
  for (const [x, y] of [[0, 0], [1279, 719], [127, 360], [640, 57], [128, 58], [1151, 661]]) assert.deepEqual(px(x, y), [0x3b, 0x82, 0xe6], `background at ${x},${y}`);
  for (const [x, y] of [[128, 360], [159, 360], [640, 58], [640, 89], [1120, 400], [640, 661], [160, 58], [128, 90]]) assert.deepEqual(px(x, y), [0x1a, 0x1c, 0x1e], `bezel at ${x},${y}`);
  const fallback = await host.evaluate(async () => (await window.ask({ framed: true, color: 'not a colour' })).color);
  assert.equal(fallback, '#E85A2C', 'a colour it cannot read: the default');

  // The host saves them: the page's buttons go.
  const frame = host.frames().find((f) => f.url().startsWith(siteUrl));
  assert.equal(await frame.evaluate(() => document.querySelector('.shots').hidden), false);
  await host.evaluate(() => document.getElementById('sim').contentWindow.postMessage({ cardenza_sim: 'host', screenshot: true }, '*'));
  await host.waitForTimeout(100);
  assert.equal(await frame.evaluate(() => document.querySelector('.shots').hidden), true);

  // ---- 2. top-level ----
  const top = await context.newPage();
  await top.goto(siteUrl);
  await top.evaluate(() => window.cardenzaSim.ready);
  await top.waitForTimeout(200);
  for (const [label, size] of [['SCREENSHOT', [960, 540]], ['FRAMED', [1280, 720]]]) {
    const [download] = await Promise.all([top.waitForEvent('download'), top.click(`.shot >> text=${label}`)]);
    const file = await download.path();
    const bytes = fs.readFileSync(file);
    assert.equal(bytes.readUInt32BE(16), size[0], `${label}: width`);
    assert.equal(bytes.readUInt32BE(20), size[1], `${label}: height`);
    assert.match(download.suggestedFilename(), new RegExp(`^shot-test-${label === 'FRAMED' ? 'framed' : 'screen'}-\\d{8}-\\d{6}\\.png$`));
  }
  // The keyboard is still the firmware's after a press on a button.
  assert.equal(await top.evaluate(() => document.activeElement.id), 'stage');
  await top.keyboard.down('KeyQ');
  await top.keyboard.up('KeyQ');
  assert.deepEqual(await top.evaluate(() => window.__keys), [[1, 1, 1], [1, 1, 0]], 'Q pressed the key, not a button');
  const viaApi = await top.evaluate(async () => { const s = await window.cardenzaSim.screenshot({ framed: true }); return [s.width, s.height, s.color, s.blob.type]; });
  assert.deepEqual(viaApi, [1280, 720, '#E85A2C', 'image/png']);
  await top.screenshot({ path: path.join(site, 'top.png') });
  console.log('screenshots: ok (' + path.join(site, 'top.png') + ')');
} finally {
  await browser.close();
  siteServer.close();
  hostServer.close();
}
