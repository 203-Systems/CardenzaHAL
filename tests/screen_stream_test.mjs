// tools/web/screen-stream.js on the streams tests/camera_test.cpp recorded (its directory is the argument): the
// screen rebuilt byte for byte after every frame, whatever pieces the bytes come in, a reader joining mid-stream,
// broken bytes, and the RGBA it draws.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const { ScreenStreamDecoder, WIDTH, HEIGHT, rgb565ToRgb, parseInfo, parseStats, MAX_FRAME_BYTES } =
  await import(join(here, '..', 'tools', 'web', 'screen-stream.js'));

const dir = process.argv[2];
assert.ok(dir, 'usage: node screen_stream_test.mjs <directory with stream.bin and frames.bin>');
const stream = new Uint8Array(readFileSync(join(dir, 'stream.bin')));
const framesFile = readFileSync(join(dir, 'frames.bin'));
const expected = new Uint16Array(framesFile.buffer, framesFile.byteOffset, framesFile.byteLength / 2);
const screen = WIDTH * HEIGHT;
const frameCount = expected.length / screen;
assert.equal(MAX_FRAME_BYTES, 64852);

// Every frame, in pieces of every size.
for (const pieceSize of [1, 7, 64, 4096, 65536, stream.length]) {
  const decoder = new ScreenStreamDecoder();
  let seen = 0;
  for (let at = 0; at < stream.length; at += pieceSize) {
    const frames = decoder.push(stream.subarray(at, Math.min(stream.length, at + pieceSize)));
    for (const frame of frames) assert.ok(frame.rects.length > 0 && frame.bytes > 0);
    seen += frames.length;
    assert.ok(seen <= frameCount);
    // The screen after the last frame these bytes completed (one push may complete several).
    if (frames.length) {
      const want = expected.subarray((seen - 1) * screen, seen * screen);
      assert.ok(decoder.pixels.every((v, i) => v === want[i]), `frame ${seen - 1} differs (pieces of ${pieceSize})`);
    }
  }
  assert.equal(seen, frameCount, `all ${frameCount} frames (pieces of ${pieceSize})`);
  assert.equal(decoder.stats.brokenFrames, 0);
  assert.equal(decoder.stats.gaps, 0);
}

// The frames' offsets, to join mid-stream.
const starts = [];
for (let at = 0; at < stream.length;) {
  const view = new DataView(stream.buffer, stream.byteOffset + at);
  assert.equal(view.getUint32(0, true), 0x535a4443);
  starts.push(at);
  at += view.getUint32(8, true);
}
assert.equal(starts.length, frameCount);
const keyframes = starts.map((at, i) => [at, i]).filter(([at]) => stream[at + 5] & 1);
assert.ok(keyframes.length >= 2, 'the recording has more than one keyframe');

// Joined inside a frame: deltas before the next keyframe are not applied (and ask for one); from it on, exact.
{
  const [keyAt, keyIndex] = keyframes[1];
  const decoder = new ScreenStreamDecoder();
  const joinAt = starts[1] + 100;
  assert.equal(decoder.push(stream.subarray(joinAt, keyAt)).length, 0);
  assert.equal(decoder.needsKeyframe, keyIndex > 2, 'a delta with nothing under it asks for a keyframe');
  let seen = 0;
  for (let i = keyIndex; i < frameCount; i++) {
    const end = i + 1 < frameCount ? starts[i + 1] : stream.length;
    assert.equal(decoder.push(stream.subarray(starts[i], end)).length, 1);
    const want = expected.subarray(i * screen, (i + 1) * screen);
    assert.ok(decoder.pixels.every((v, k) => v === want[k]), `frame ${i} after joining`);
    seen++;
  }
  assert.equal(seen, frameCount - keyIndex);
}

// A frame missing in the middle: the next ones are still drawn, the gap is counted and a keyframe asked for.
if (frameCount > 4 && !(stream[starts[2] + 5] & 1)) {
  const decoder = new ScreenStreamDecoder();
  decoder.push(stream.subarray(0, starts[2]));
  decoder.push(stream.subarray(starts[3]));
  assert.ok(decoder.stats.gaps >= 1 || decoder.needsKeyframe);
}

// Broken bytes between frames, and a frame with a broken trailer: skipped, the rest exact.
{
  const decoder = new ScreenStreamDecoder();
  const junk = new Uint8Array(1000).fill(0x43);
  junk.set([0x43, 0x44, 0x5a, 0x53, 1, 0, 32, 0, 100, 0, 0, 0], 500); // a false magic with a plausible length
  decoder.push(junk);
  const broken = stream.slice(starts[0], starts[1]);
  broken[broken.length - 1] ^= 0xff;
  decoder.push(broken);
  assert.ok(decoder.stats.brokenFrames >= 1);
  let seen = 0;
  for (const frame of decoder.push(stream)) seen++;
  assert.equal(seen, frameCount);
  assert.ok(decoder.pixels.every((v, i) => v === expected[(frameCount - 1) * screen + i]));
}

// RGBA: each channel's top bits repeated.
assert.deepEqual(rgb565ToRgb(0xffff), [255, 255, 255]);
assert.deepEqual(rgb565ToRgb(0x0000), [0, 0, 0]);
assert.deepEqual(rgb565ToRgb(0xf800), [255, 0, 0]);
assert.deepEqual(rgb565ToRgb(0x07e0), [0, 255, 0]);
assert.deepEqual(rgb565ToRgb(0x001f), [0, 0, 255]);
assert.deepEqual(rgb565ToRgb(0x8410), [132, 130, 132]);
{
  const decoder = new ScreenStreamDecoder();
  decoder.push(stream);
  const rgba = new Uint8ClampedArray(screen * 4);
  decoder.toRGBA(rgba);
  for (const i of [0, 1234, screen - 1]) assert.deepEqual([...rgba.subarray(i * 4, i * 4 + 4)], [...rgb565ToRgb(decoder.pixels[i]), 255]);
}

// INFO and STATS as the device writes them (camera_logic.h WriteInfo, WriteStats).
{
  const info = new DataView(new Uint8Array([1, 1, 240, 0, 135, 0, 3, 0, 42, 0, 0, 0, 0x54, 0xfd, 0, 0]).buffer);
  assert.deepEqual(parseInfo(info), { version: 1, format: 1, width: 240, height: 135, started: true, stalled: true, nextSequence: 42, maxFrameBytes: 64852 });
  const bytes = new Uint8Array(64);
  bytes[0] = 4;
  new DataView(bytes.buffer).setUint32(4, 5, true);
  new DataView(bytes.buffer).setUint32(52, 9, true);
  const stats = parseStats(new DataView(bytes.buffer));
  assert.equal(stats.streaming, true);
  assert.equal(stats.frames, 5);
  assert.equal(stats.errors, 9);
}

console.log(`screen stream reader: ${frameCount} frames exact, ${keyframes.length} keyframes`);
