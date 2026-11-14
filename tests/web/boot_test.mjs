// Boots a web build (the folder tools/cardenza_web.py writes) in Node, without a browser: the module
// runs its scheduler for a while, and the test checks that app_main ran and, optionally, that the log
// says what it should. CI runs it on the HAL's examples; tests/web/browser/run_sim.mjs is the full check
// in Chrome.
//
//   node tests/web/boot_test.mjs <site dir> [--seconds 2] [--expect "<log text>"]... [--no-card]

import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';

const args = process.argv.slice(2);
const site = args[0];
let seconds = 2;
const expected = [];
let card = 1;
for (let index = 1; index < args.length; index++) {
  if (args[index] === '--seconds') seconds = Number(args[++index]);
  else if (args[index] === '--expect') expected.push(args[++index]);
  else if (args[index] === '--no-card') card = 0;
}
if (!site) {
  console.error('usage: boot_test.mjs <site dir> [--seconds N] [--expect "<log text>"]... [--no-card]');
  process.exit(2);
}

const manifest = JSON.parse(fs.readFileSync(path.join(site, 'sim.json'), 'utf8'));
const script = manifest.files.map((file) => file.path).find((name) => name.endsWith('.js') && name !== 'cardenza-sim.js');
const factory = createRequire(import.meta.url)(path.resolve(site, script));
const lines = [];
let booted = false;
let aborted = null;
const module = await factory({
  cardenzaConfig: { card, resetReason: 1 },
  cardenzaBooted() { booted = true; },
  cardenzaRestart(reason) { lines.push(`[restart ${reason}]`); },
  print(text) { lines.push(text); },
  printErr(text) { lines.push(text); },
  onAbort(reason) { aborted = String(reason); },
});

const start = performance.now();
await new Promise((resolve) => {
  const pump = () => {
    try {
      module._cardenza_web_run(8);
    } catch (error) {
      aborted = String(error);
      resolve();
      return;
    }
    if (performance.now() - start > seconds * 1000) {
      resolve();
      return;
    }
    const delay = module._cardenza_web_next_delay();
    setTimeout(pump, delay < 0 ? 20 : Math.min(delay, 20));
  };
  pump();
});

const failures = [];
if (!booted) failures.push('app_main never ran');
if (aborted) failures.push(`aborted: ${aborted}`);
for (const text of expected) if (!lines.some((line) => line.includes(text))) failures.push(`the log never said "${text}"`);
console.log(lines.join('\n'));
console.log(failures.length ? `FAILED ${manifest.firmware.name}: ${failures.join('; ')}` : `booted ${manifest.firmware.name}`);
process.exit(failures.length ? 1 : 0);
