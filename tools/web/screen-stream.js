// The Cardenza's screen stream, read in a browser (WebUSB) or in Node (the decoder alone): the reference reader for
// CardenzaHAL's USB camera (usb/camera.h). Protocol: docs/screen-stream.md. No dependencies; an ES module.
//
//   import { ScreenStreamDevice } from './screen-stream.js';
//   const device = await ScreenStreamDevice.request();          // the browser's chooser (a click), or .find()
//   await device.open();                                        // claims the "Cardenza WebUSB" interface
//   const view = device.attach(canvas);                         // 240 x 135, exact pixels (scale it with CSS)
//   await device.start();                                       // frames until device.stop()
//   device.stats  // { frames, keyframes, bytes, fps, kbps, ... }
//
// A desktop program reads the same bytes from the same endpoint (libusb, WinUSB): docs/screen-stream.md.

export const WIDTH = 240;
export const HEIGHT = 135;
export const MAGIC = 0x535a4443; // "CDZS"
export const END_MAGIC = 0x455a4443; // "CDZE"
export const VERSION = 1;
export const FORMAT_RGB565 = 1;
export const HEADER_BYTES = 32;
export const RECT_HEADER_BYTES = 16;
export const TRAILER_BYTES = 4;
export const MAX_FRAME_BYTES = HEADER_BYTES + RECT_HEADER_BYTES + WIDTH * HEIGHT * 2 + TRAILER_BYTES; // 64852
export const FLAG_KEYFRAME = 0x01;
export const ENCODING_RAW = 0;
export const ENCODING_RUNS = 1;
export const REQUEST = Object.freeze({ START: 0x20, STOP: 0x21, KEYFRAME: 0x22, INFO: 0x23, STATS: 0x24 });
export const START_ALLOW_RUNS = 0x0001;
// Every 203 device's USB vendor ID (the HAL's firmware: 0x0203).
export const USB_FILTERS = Object.freeze([{ vendorId: 0x0203 }]);
export const INTERFACE_CLASS_VENDOR = 0xff;

// RGB565 to 8-bit channels: each channel's top bits repeated into its low ones (0 -> 0, full -> 255).
export function rgb565ToRgb(value) {
  const r = (value >> 11) & 31, g = (value >> 5) & 63, b = value & 31;
  return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)];
}

// The run code (docs/screen-stream.md): chunks of a u16 control whose top two bits say literal (00), run (10) or
// pair (01), and whose low 14 bits count the pixels. Fills `out` (count pixels); false when the code is broken.
export function decodeRuns(view, offset, length, out, count) {
  let at = offset;
  const end = offset + length;
  let filled = 0;
  while (at < end) {
    if (at + 2 > end) return false;
    const control = view.getUint16(at, true);
    at += 2;
    const n = control & 0x3fff;
    if (n === 0 || filled + n > count) return false;
    switch (control & 0xc000) {
      case 0x0000:
        if (at + 2 * n > end) return false;
        for (let k = 0; k < n; k++, at += 2) out[filled++] = view.getUint16(at, true);
        break;
      case 0x8000: {
        if (at + 2 > end) return false;
        const value = view.getUint16(at, true);
        at += 2;
        out.fill(value, filled, filled + n);
        filled += n;
        break;
      }
      case 0x4000: {
        if (at + 4 > end) return false;
        const a = view.getUint16(at, true), b = view.getUint16(at + 2, true);
        at += 4;
        for (let k = 0; k < n; k++) out[filled++] = k & 1 ? b : a;
        break;
      }
      default:
        return false;
    }
  }
  return filled === count;
}

// Rebuilds the screen from the stream's bytes, whatever pieces they come in.
export class ScreenStreamDecoder {
  constructor() {
    this.width = WIDTH;
    this.height = HEIGHT;
    this.pixels = new Uint16Array(WIDTH * HEIGHT); // RGB565, the screen as the device last sent it
    this.haveKeyframe = false;
    this.needsKeyframe = false; // ask the device for one (REQUEST.KEYFRAME): a delta with nothing under it, a gap, a broken frame
    this.sequence = null;
    this.buffer = new Uint8Array(MAX_FRAME_BYTES * 2);
    this.buffered = 0;
    this.scratch = new Uint16Array(WIDTH * HEIGHT);
    this.stats = { frames: 0, keyframes: 0, rects: 0, bytes: 0, skippedBytes: 0, brokenFrames: 0, gaps: 0, presents: 0 };
  }

  // Takes the next bytes (a Uint8Array, an ArrayBuffer or a DataView). Returns the frames they completed, each
  // { sequence, keyframe, rects: [{ x, y, w, h }], timeMs, presents, bytes }, in order.
  push(chunk) {
    const bytes = chunk instanceof Uint8Array ? chunk : chunk instanceof DataView
      ? new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength) : new Uint8Array(chunk);
    const frames = [];
    let offset = 0;
    while (offset < bytes.length) {
      const take = Math.min(bytes.length - offset, this.buffer.length - this.buffered);
      this.buffer.set(bytes.subarray(offset, offset + take), this.buffered);
      this.buffered += take;
      offset += take;
      this.#scan(frames);
    }
    return frames;
  }

  #drop(count) {
    this.buffer.copyWithin(0, count, this.buffered);
    this.buffered -= count;
  }

  #scan(frames) {
    const view = new DataView(this.buffer.buffer, this.buffer.byteOffset, this.buffer.byteLength);
    let start = 0;
    while (this.buffered - start >= 12) {
      const magic = view.getUint32(start, true);
      const frameBytes = view.getUint32(start + 8, true);
      if (magic !== MAGIC || frameBytes < HEADER_BYTES + TRAILER_BYTES || frameBytes > MAX_FRAME_BYTES) {
        if (magic === MAGIC) this.stats.brokenFrames++;
        start++;
        this.stats.skippedBytes++;
        continue;
      }
      if (this.buffered - start < frameBytes) break;
      const frame = this.#apply(view, start, frameBytes);
      if (frame) {
        frames.push(frame);
        start += frameBytes;
      } else {
        this.stats.brokenFrames++;
        this.needsKeyframe = true;
        start++;
        this.stats.skippedBytes++;
      }
    }
    if (start > 0) this.#drop(start);
  }

  #apply(view, at, length) {
    const end = at + length;
    if (view.getUint8(at + 4) !== VERSION || view.getUint16(at + 6, true) !== HEADER_BYTES ||
        view.getUint16(at + 16, true) !== WIDTH || view.getUint16(at + 18, true) !== HEIGHT ||
        view.getUint8(at + 20) !== FORMAT_RGB565 || view.getUint32(end - 4, true) !== END_MAGIC) return null;
    const keyframe = (view.getUint8(at + 5) & FLAG_KEYFRAME) !== 0;
    const sequence = view.getUint32(at + 12, true);
    if (!keyframe && !this.haveKeyframe) {
      this.needsKeyframe = true;
      return null;
    }
    if (!keyframe && this.sequence !== null && sequence !== ((this.sequence + 1) >>> 0)) {
      // A frame is missing: what it changed is not on this screen. Ask for a keyframe and draw on meanwhile.
      this.stats.gaps++;
      this.needsKeyframe = true;
    }
    const count = view.getUint16(at + 22, true);
    const rects = [];
    let p = at + HEADER_BYTES;
    // Check every rectangle before painting any: a broken frame paints nothing.
    for (let i = 0; i < count; i++) {
      if (p + RECT_HEADER_BYTES > end - TRAILER_BYTES) return null;
      const x = view.getUint16(p, true), y = view.getUint16(p + 2, true);
      const w = view.getUint16(p + 4, true), h = view.getUint16(p + 6, true);
      const encoding = view.getUint8(p + 8);
      const bytes = view.getUint32(p + 12, true);
      const padded = (bytes + 3) & ~3;
      if (x + w > WIDTH || y + h > HEIGHT || p + RECT_HEADER_BYTES + padded > end - TRAILER_BYTES) return null;
      if (encoding === ENCODING_RAW ? bytes !== w * h * 2 : encoding !== ENCODING_RUNS) return null;
      rects.push({ x, y, w, h, encoding, data: p + RECT_HEADER_BYTES, bytes });
      p += RECT_HEADER_BYTES + padded;
    }
    for (const r of rects) {
      if (r.encoding === ENCODING_RAW) {
        for (let row = 0; row < r.h; row++) {
          const line = (r.y + row) * WIDTH + r.x;
          const src = r.data + row * r.w * 2;
          for (let column = 0; column < r.w; column++) this.pixels[line + column] = view.getUint16(src + 2 * column, true);
        }
      } else {
        if (!decodeRuns(view, r.data, r.bytes, this.scratch, r.w * r.h)) return null;
        for (let row = 0; row < r.h; row++)
          this.pixels.set(this.scratch.subarray(row * r.w, (row + 1) * r.w), (r.y + row) * WIDTH + r.x);
      }
    }
    this.sequence = sequence;
    if (keyframe) {
      this.haveKeyframe = true;
      this.needsKeyframe = false;
      this.stats.keyframes++;
    }
    const presents = view.getUint16(at + 28, true);
    this.stats.frames++;
    this.stats.rects += count;
    this.stats.bytes += length;
    this.stats.presents += presents;
    return {
      sequence, keyframe, presents, bytes: length, timeMs: view.getUint32(at + 24, true),
      rects: rects.map(({ x, y, w, h }) => ({ x, y, w, h })),
    };
  }

  // The screen (or `rects` of it) as RGBA into `rgba` (a Uint8ClampedArray of WIDTH * HEIGHT * 4: ImageData.data).
  toRGBA(rgba, rects = null) {
    const areas = rects ?? [{ x: 0, y: 0, w: WIDTH, h: HEIGHT }];
    for (const { x, y, w, h } of areas)
      for (let row = y; row < y + h; row++)
        for (let column = x; column < x + w; column++) {
          const index = row * WIDTH + column;
          const value = this.pixels[index];
          const r = (value >> 11) & 31, g = (value >> 5) & 63, b = value & 31;
          const o = index * 4;
          rgba[o] = (r << 3) | (r >> 2);
          rgba[o + 1] = (g << 2) | (g >> 4);
          rgba[o + 2] = (b << 3) | (b >> 2);
          rgba[o + 3] = 255;
        }
  }
}

// INFO's 16 bytes.
export function parseInfo(view) {
  return {
    version: view.getUint8(0), format: view.getUint8(1), width: view.getUint16(2, true), height: view.getUint16(4, true),
    started: (view.getUint8(6) & 1) !== 0, stalled: (view.getUint8(6) & 2) !== 0,
    nextSequence: view.getUint32(8, true), maxFrameBytes: view.getUint32(12, true),
  };
}

// STATS' 64 bytes (usb/camera.h Stats).
export function parseStats(view) {
  const flags = view.getUint8(0);
  const names = ['frames', 'keyframes', 'rects', 'presents', 'coalesced', 'bytes', 'lastFrameBytes', 'sequence',
    'captureUs', 'maxCaptureUs', 'encodeUs', 'maxEncodeUs', 'errors'];
  const stats = { enabled: !!(flags & 1), open: !!(flags & 2), streaming: !!(flags & 4), stalled: !!(flags & 8) };
  names.forEach((name, i) => { stats[name] = view.getUint32(4 + 4 * i, true); });
  return stats;
}

// A Cardenza over WebUSB, its stream read into a decoder and drawn on a canvas.
export class ScreenStreamDevice {
  // The browser's chooser: once per device and site, then find() has it.
  static async request(usb = globalThis.navigator?.usb) {
    if (!usb) throw new Error('WebUSB is not available here (Chrome or Edge, https or localhost)');
    return new ScreenStreamDevice(await usb.requestDevice({ filters: [...USB_FILTERS] }));
  }

  // The 203 devices this site may already use (no chooser).
  static async find(usb = globalThis.navigator?.usb) {
    if (!usb) return [];
    const devices = await usb.getDevices();
    return devices.filter((d) => USB_FILTERS.some((f) => d.vendorId === f.vendorId)).map((d) => new ScreenStreamDevice(d));
  }

  constructor(device) {
    this.device = device;
    this.decoder = new ScreenStreamDecoder();
    this.interfaceNumber = null;
    this.endpointNumber = null;
    this.running = false;
    this.canvas = null;
    this.image = null;
    this.stats = { frames: 0, keyframes: 0, bytes: 0, fps: 0, kbps: 0 };
    this.onframe = null; // (frame) => {}
    this.window = []; // [time, bytes] of the last second's frames
  }

  // Opens the device and claims the "Cardenza WebUSB" interface, the one with the stream's bulk IN endpoint.
  async open() {
    const d = this.device;
    if (!d.opened) await d.open();
    if (d.configuration === null) await d.selectConfiguration(1);
    for (const itf of d.configuration.interfaces) {
      for (const alt of itf.alternates) {
        if (alt.interfaceClass !== INTERFACE_CLASS_VENDOR) continue;
        const endpoint = alt.endpoints.find((e) => e.direction === 'in' && e.type === 'bulk');
        if (!endpoint) continue;
        await d.claimInterface(itf.interfaceNumber);
        this.interfaceNumber = itf.interfaceNumber;
        this.endpointNumber = endpoint.endpointNumber;
        return this;
      }
    }
    throw new Error('this device shows no screen stream: its firmware has not turned the camera on (USB::Camera::Enable)');
  }

  #setup(request, value = 0) {
    return { requestType: 'vendor', recipient: 'interface', request, value, index: this.interfaceNumber };
  }

  async info() {
    const result = await this.device.controlTransferIn(this.#setup(REQUEST.INFO), 16);
    return parseInfo(result.data);
  }

  async deviceStats() {
    const result = await this.device.controlTransferIn(this.#setup(REQUEST.STATS), 64);
    return parseStats(result.data);
  }

  async keyframe() {
    await this.device.controlTransferOut(this.#setup(REQUEST.KEYFRAME));
  }

  // Draws every frame on `canvas` at 240 x 135 (scale it with CSS: image-rendering: pixelated keeps it exact).
  attach(canvas) {
    canvas.width = WIDTH;
    canvas.height = HEIGHT;
    this.canvas = canvas;
    this.context = canvas.getContext('2d');
    this.image = this.context.createImageData(WIDTH, HEIGHT);
    return canvas;
  }

  // Starts the stream and reads it until stop(). `runs`: let the device use the run code (fewer bytes).
  async start({ runs = true } = {}) {
    if (this.interfaceNumber === null) await this.open();
    const result = await this.device.controlTransferOut(this.#setup(REQUEST.START, runs ? START_ALLOW_RUNS : 0));
    if (result.status !== 'ok') throw new Error('the device did not start the stream (it needs 130 KB of PSRAM)');
    this.running = true;
    if (!this.loop) this.loop = this.#read().finally(() => { this.loop = null; });
    return this;
  }

  // Stops the stream. A read already waiting on the endpoint stays until the device sends again (after the next
  // start) and is then dropped: the decoder asks for a keyframe if that frame was one it needed.
  async stop() {
    this.running = false;
    try {
      await this.device.controlTransferOut(this.#setup(REQUEST.STOP));
    } catch {
      // unplugged
    }
    await Promise.race([this.loop?.catch(() => {}), new Promise((done) => setTimeout(done, 200))]);
  }

  async #read() {
    while (this.running) {
      let result;
      try {
        result = await this.device.transferIn(this.endpointNumber, 65536);
      } catch (error) {
        if (!this.running) return;
        throw error;
      }
      if (!this.running) return;
      if (result.status === 'stall') {
        await this.device.clearHalt('in', this.endpointNumber).catch(() => {});
        continue;
      }
      if (result.status !== 'ok' || !result.data) continue;
      const frames = this.decoder.push(result.data);
      if (this.decoder.needsKeyframe) {
        this.decoder.needsKeyframe = false;
        this.keyframe().catch(() => {});
      }
      const now = performance.now();
      for (const frame of frames) {
        this.window.push([now, frame.bytes]);
        if (this.image) {
          this.decoder.toRGBA(this.image.data, frame.keyframe ? null : frame.rects);
          this.context.putImageData(this.image, 0, 0);
        }
        this.onframe?.(frame);
      }
      while (this.window.length && now - this.window[0][0] > 1000) this.window.shift();
      const s = this.decoder.stats;
      this.stats = {
        frames: s.frames, keyframes: s.keyframes, bytes: s.bytes, gaps: s.gaps, brokenFrames: s.brokenFrames,
        fps: this.window.length, kbps: this.window.reduce((sum, [, b]) => sum + b, 0) / 1024,
      };
    }
  }
}
