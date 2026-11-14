// The page runtime's pure parts, without a browser: the key table against web/page/keymap.json (the
// build copies that into sim.json; the page posts the table itself) and the USB-MIDI packing of the
// Web MIDI bridge. Run: node tests/web/browser/page_logic_test.mjs
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const page = path.join(here, '..', '..', '..', 'web', 'page');
const source = fs.readFileSync(path.join(page, 'cardenza-sim.js'), 'utf8');

function section(start, end) {
  const from = source.indexOf(start);
  const to = source.indexOf(end, from);
  assert.ok(from >= 0 && to > from, `section ${start}`);
  return source.slice(from, to);
}

// The key table.
const keyTable = new Function('const COLUMNS = 14, ROWS = 4;\n' + section('  const CAPS = [', '  // The light a key gives off') + '\nreturn keyTable;')();
const table = keyTable();
assert.equal(table.length, 57);
assert.ok(table.length <= 64, 'the community shows at most 64 rows');
assert.deepEqual(table, JSON.parse(fs.readFileSync(path.join(page, 'keymap.json'), 'utf8')), 'keymap.json is the table the page posts');
for (const [device, computer] of table) assert.ok(typeof device === 'string' && typeof computer === 'string' && device && computer);

// USB-MIDI packets.
const sent = [];
const usb = new Function('log', 'pumpSoon', 'instance',
  section('  let midi = null;', '  function startMidi()') +
  '\nmidi = { outputs: new Map([[1, { send: (bytes) => SENT.push(Array.from(bytes)) }]]) };\nreturn { midiToPackets, midiOut };'
    .replace('SENT', 'arguments[3]'));
const { midiToPackets, midiOut } = usb(() => {}, () => {}, null, sent);
assert.deepEqual(midiToPackets([0x90, 60, 100]), [[0x9, 0x90, 60, 100]]);
assert.deepEqual(midiToPackets([0xC3, 5]), [[0xC, 0xC3, 5, 0]]);
assert.deepEqual(midiToPackets([0xF8]), [[0xF, 0xF8, 0, 0]]);
assert.deepEqual(midiToPackets([0xF0, 0x23, 0x7F, 0x47, 0x3F, 0xF7]), [[0x4, 0xF0, 0x23, 0x7F], [0x7, 0x47, 0x3F, 0xF7]]);
assert.deepEqual(midiToPackets([0xF0, 0x23, 0x44, 0xF7]), [[0x4, 0xF0, 0x23, 0x44], [0x5, 0xF7, 0, 0]]);
assert.deepEqual(midiToPackets([0xF0, 0x23, 0x44, 0x01, 0xF7]), [[0x4, 0xF0, 0x23, 0x44], [0x6, 0x01, 0xF7, 0]]);
// The firmware's packets back to bytes: a SysEx goes out whole, a note as it is.
midiOut([0x4, 0xF0, 0x24, 0xC4]);
midiOut([0x4, 0x01, 0x43, 0x61]);
midiOut([0x6, 0x72, 0xF7, 0]);
midiOut([0x9, 0x91, 64, 90]);
assert.deepEqual(sent, [[0xF0, 0x24, 0xC4, 0x01, 0x43, 0x61, 0x72, 0xF7], [0x91, 64, 90]]);
console.log('page logic: ok');
