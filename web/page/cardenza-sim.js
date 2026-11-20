// The CardenzaHAL web target's page: runs a Cardenza firmware built for the browser
// (tools/cardenza_web.py) and is the device around it.
//
// - The firmware's FreeRTOS scheduler runs on this thread (web/src/kernel): the page calls
//   _cardenza_web_run() from the event loop and again when _cardenza_web_next_delay() says, through a
//   MessageChannel (no 4 ms clamp on nested timers). No SharedArrayBuffer, so no COOP/COEP headers:
//   it runs in <iframe sandbox="allow-scripts">.
// - The screen is the ST7789 model's glass (240 x 135 RGB565) under the backlight's light; the keys
//   glow with the light their LEDs give (keyboard/led_light.h); the computer keyboard, the mouse and
//   touch press the 4 x 14 keys and HOME.
// - Sound: the I2S DMA in the module drains on an audio clock this page keeps (the AudioContext once it
//   runs, the performance clock before); its frames carry their position on that clock and an
//   AudioWorklet plays each at its time, a fixed latency later.
// - Storage: /sdcard (and /.cardenza for NVS) in IndexedDB when the page may use it, else in memory.
// - USB: Web MIDI when the page may use it; the firmware's app channel also through postMessage.
// - The parent hears {"cardenza_sim":"ready"} when the firmware's app_main runs, then its key table and
//   {"cardenza_sim":"features","features":["screenshot"]}.
// - Screenshots are of the screen alone, never the board (the community's picture standard, the user
//   2026-11-20: "不要使用模拟器带键盘的截图，可以使用模拟器里面截的屏幕的截图"): SCREENSHOT saves the 240 x 135 frame at 4
//   times, nearest neighbour (960 x 540 PNG); FRAMED the standard picture (1280 x 720: one flat colour, the
//   bezel, that screen; https://cardenza.203.io/community/guide/images). A parent asks with
//   {"cardenza_sim":"screenshot","id":…,"framed":bool,"color":"#RRGGBB"} and gets {"cardenza_sim":"screenshot",
//   "id","framed","color","width","height","name","png":ArrayBuffer} (or "error"); a parent that saves them
//   itself (a sandboxed frame may not download) says {"cardenza_sim":"host","screenshot":true} and the
//   page's own buttons hide.
(function () {
  'use strict';

  const SAMPLE_RATE = 44100;
  const COLUMNS = 14;
  const ROWS = 4;
  const WIDTH = 240;
  const HEIGHT = 135;

  // ---- the keys -------------------------------------------------------------------------------

  // What the keycaps of the board say (the firmware decides what they do).
  const CAPS = [
    ['esc', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 'del'],
    ['tab', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\\'],
    ['fn', 'shift', 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', "'", 'enter'],
    ['ctrl', 'opt', 'alt', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 'space'],
  ];

  // The computer keyboard by physical position (KeyboardEvent.code), whatever its layout.
  const CODES = new Map();
  function bind(row, column, ...codes) { for (const code of codes) CODES.set(code, [column, row]); }
  bind(0, 0, 'Escape', 'Backquote');
  ['Digit1', 'Digit2', 'Digit3', 'Digit4', 'Digit5', 'Digit6', 'Digit7', 'Digit8', 'Digit9', 'Digit0', 'Minus', 'Equal']
    .forEach((code, index) => bind(0, index + 1, code));
  bind(0, 13, 'Backspace', 'Delete', 'NumpadAdd');
  bind(1, 0, 'Tab');
  ['KeyQ', 'KeyW', 'KeyE', 'KeyR', 'KeyT', 'KeyY', 'KeyU', 'KeyI', 'KeyO', 'KeyP', 'BracketLeft', 'BracketRight']
    .forEach((code, index) => bind(1, index + 1, code));
  bind(1, 13, 'Backslash', 'NumpadSubtract');
  bind(2, 0, 'CapsLock', 'ShiftLeft');
  bind(2, 1, 'ShiftRight');
  ['KeyA', 'KeyS', 'KeyD', 'KeyF', 'KeyG', 'KeyH', 'KeyJ', 'KeyK', 'KeyL', 'Semicolon', 'Quote']
    .forEach((code, index) => bind(2, index + 2, code));
  bind(2, 13, 'Enter', 'NumpadEnter');
  bind(3, 0, 'ControlLeft', 'ControlRight');
  bind(3, 1, 'AltLeft');
  bind(3, 2, 'AltRight');
  ['KeyZ', 'KeyX', 'KeyC', 'KeyV', 'KeyB', 'KeyN', 'KeyM', 'Comma', 'Period', 'Slash']
    .forEach((code, index) => bind(3, index + 3, code));
  bind(3, 13, 'Space');
  const HOME_CODE = 'Home';

  const CODE_NAMES = {
    Escape: 'Esc', Backquote: '`', Minus: '-', Equal: '=', Backspace: 'Backspace', Delete: 'Delete', NumpadAdd: 'Num +',
    Tab: 'Tab', BracketLeft: '[', BracketRight: ']', Backslash: '\\', NumpadSubtract: 'Num -', CapsLock: 'Caps Lock',
    ShiftLeft: 'left Shift', ShiftRight: 'right Shift', Semicolon: ';', Quote: "'", Enter: 'Enter', NumpadEnter: 'Num Enter',
    ControlLeft: 'Ctrl', ControlRight: 'right Ctrl', AltLeft: 'left Alt/Option', AltRight: 'right Alt/Option', Comma: ',',
    Period: '.', Slash: '/', Space: 'Space', Home: 'Home',
  };
  function codeName(code) {
    if (CODE_NAMES[code]) return CODE_NAMES[code];
    if (code.startsWith('Key')) return code.slice(3);
    if (code.startsWith('Digit')) return code.slice(5);
    return code;
  }
  // The short label on a keycap (the first code only).
  const SHORT = { Escape: 'esc', Backspace: '⌫', Tab: 'tab', CapsLock: 'caps', ShiftRight: '⇧R', Enter: '⏎', ControlLeft: 'ctrl',
    AltLeft: 'altL', AltRight: 'altR', Space: '␣', Backquote: '`' };

  function codesOf(x, y) {
    const codes = [];
    for (const [code, [cx, cy]] of CODES) if (cx === x && cy === y) codes.push(code);
    return codes;
  }

  // [[device key, computer keys], ...]: what the player shows under Keys (also web/page/keymap.json).
  function keyTable() {
    const rows = [];
    for (let y = 0; y < ROWS; y++) {
      for (let x = 0; x < COLUMNS; x++) rows.push([CAPS[y][x].toUpperCase(), codesOf(x, y).map(codeName).join(' / ')]);
    }
    rows.push(['HOME', codeName(HOME_CODE)]);
    return rows;
  }

  // The light a key gives off (0..1): its duty level times the LED enable PWM (gamma 2), as
  // keyboard/led_light.h and keyboard/internal/led_brightness.h compute it.
  const GLOBAL_DUTY = [];
  for (let level = 0; level < 16; level++) GLOBAL_DUTY.push(Math.floor((level * level * 1023 + 112) / 225) / 1023);

  // ---- the page -------------------------------------------------------------------------------

  const params = new URLSearchParams(location.search);
  const body = document.body;
  const options = {
    module: body.dataset.module || 'firmware.js',
    title: body.dataset.title || 'Cardenza firmware',
    version: body.dataset.version || '',
    card: params.get('sd') !== '0',
    cardMegabytes: Number(params.get('sdmb')) > 0 ? Number(params.get('sdmb')) : 8192,
    showLog: params.get('log') === '1',
  };
  if (options.title.startsWith('@CARDENZA')) options.title = 'Cardenza firmware';
  if (options.version.startsWith('@CARDENZA')) options.version = '';

  function element(tag, className, parent, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    if (parent) parent.appendChild(node);
    return node;
  }

  const stage = element('div', '', body);
  stage.id = 'stage';
  stage.tabIndex = 0;
  const board = element('div', '', stage);
  board.id = 'board';
  const brand = element('div', 'brand', board, 'CARDENZA');
  element('small', '', brand, options.title + (options.version ? ' ' + options.version : ''));
  const hint = element('div', 'hint', board, 'TAP FOR SOUND');
  const irDot = element('div', 'ir', board);
  irDot.title = 'IR LED';
  const bezel = element('div', 'bezel', board);
  const canvas = element('canvas', '', bezel);
  canvas.id = 'screen';
  canvas.width = WIDTH;
  canvas.height = HEIGHT;
  const overlay = element('div', 'overlay', bezel, 'LOADING');
  const homeButton = element('div', 'home', board, 'HOME');
  element('small', '', homeButton, 'Home');
  homeButton.title = 'HOME, the side button (GPIO0)';
  const shots = element('div', 'shots', board);
  const shotButton = element('button', 'shot', shots, 'SCREENSHOT');
  shotButton.title = 'Save the screen as a PNG: 240 x 135 at 4 times (960 x 540), without the board';
  const framedButton = element('button', 'shot', shots, 'FRAMED');
  framedButton.title = "Save the community's standard picture: the screen in a bezel on one colour (1280 x 720)";
  const keysBox = element('div', '', board);
  keysBox.id = 'keys';
  const consoleBox = element('pre', '', body);
  consoleBox.id = 'console';
  consoleBox.hidden = !options.showLog;
  if (options.showLog) consoleBox.style.height = '38%';
  // The board fills the window at 7:4 on black (above the log when it shows).
  function fit() {
    const height = innerHeight - (consoleBox.hidden ? 0 : consoleBox.offsetHeight);
    const scale = Math.min(innerWidth / 700, height / 400);
    board.style.transform = 'translate(' + ((innerWidth - 700 * scale) / 2) + 'px,' + ((height - 400 * scale) / 2) + 'px) scale(' + scale + ')';
  }
  addEventListener('resize', fit);
  fit();
  const context2d = canvas.getContext('2d');
  const image = context2d.createImageData(WIDTH, HEIGHT);

  const keyElements = [];
  for (let y = 0; y < ROWS; y++) {
    for (let x = 0; x < COLUMNS; x++) {
      const key = element('div', 'key', keysBox);
      key.style.left = (x * 48) + 'px';
      key.style.top = (y * 48) + 'px';
      const glow = element('div', 'glow', key);
      element('span', 'cap', key, CAPS[y][x]);
      const codes = codesOf(x, y);
      const label = codes.length ? (SHORT[codes[0]] || codeName(codes[0]).toLowerCase()) : '';
      if (label && label !== CAPS[y][x]) element('span', 'pc', key, label);
      key.setAttribute('role', 'button');
      key.setAttribute('aria-label', CAPS[y][x] + ' (row ' + (y + 1) + ', column ' + (x + 1) + ')');
      key.title = CAPS[y][x] + (codes.length ? '  ·  computer: ' + codes.map(codeName).join(' / ') : '');
      key.dataset.x = x;
      key.dataset.y = y;
      keyElements.push({ key, glow, light: -1 });
    }
  }


  // ---- log -------------------------------------------------------------------------------------

  const logs = [];
  const errorLines = [];
  function log(text, error) {
    const line = String(text);
    logs.push(line);
    if (logs.length > 1000) logs.splice(0, logs.length - 1000);
    if (error) {
      errorLines.push(line);
      if (errorLines.length > 12) errorLines.shift();
    }
    // ESP_LOGE goes to stdout like every log line; the panic overlay wants it too.
    if (!error && line.startsWith('E (')) {
      errorLines.push(line);
      if (errorLines.length > 12) errorLines.shift();
    }
    (error ? console.warn : console.log)(line);
    if (options.showLog) {
      consoleBox.textContent += line + '\n';
      if (consoleBox.textContent.length > 60000) consoleBox.textContent = consoleBox.textContent.slice(-40000);
      consoleBox.scrollTop = consoleBox.scrollHeight;
    }
  }

  // A frame the host did not allow a feature (allow="midi", "microphone") would only log a violation.
  function allowed(feature) {
    const policy = document.permissionsPolicy || document.featurePolicy;
    try { return !policy || policy.allowsFeature(feature); } catch (_) { return true; }
  }

  function post(message, transfer) {
    try { if (parent !== window) parent.postMessage(message, '*', transfer || []); } catch (_) { /* no parent to tell */ }
  }

  // ---- the audio clock and output ----------------------------------------------------------------

  // Seconds, continuous and never going back: the performance clock until the AudioContext runs, then
  // the context's (interpolated between its render quanta); each switch keeps the clock where it was.
  const clock = { last: 0, mode: 'performance', offset: 0, seenContext: -1, seenAt: 0 };
  let audio = null; // { context, node, latencyFrames, micSource, micStream }

  function audioClock() {
    const now = performance.now();
    const running = audio && audio.context.state === 'running';
    if (running !== (clock.mode === 'context')) {
      clock.mode = running ? 'context' : 'performance';
      clock.offset = running ? clock.last - audio.context.currentTime : clock.last - now / 1000;
      clock.seenContext = -1;
      if (running) sendClock();
    }
    let seconds;
    if (running) {
      const contextTime = audio.context.currentTime;
      if (contextTime !== clock.seenContext) {
        clock.seenContext = contextTime;
        clock.seenAt = now;
      }
      // Between quanta the clock moves on with the performance clock, at most 50 ms (a stalled audio
      // thread stops it, as a stopped codec clock stops the device's DMA).
      seconds = contextTime + Math.min((now - clock.seenAt) / 1000, 0.05) + clock.offset;
    } else {
      seconds = now / 1000 + clock.offset;
    }
    if (seconds < clock.last) seconds = clock.last;
    clock.last = seconds;
    return seconds;
  }

  function sendClock() {
    if (!audio) return;
    audio.node.port.postMessage({ offsetFrames: Math.round(clock.offset * SAMPLE_RATE), latencyFrames: audio.latencyFrames });
  }

  // The worklet plays every frame at its position on the clock, a fixed latency later: frames that come
  // too late are dropped (and raise the latency), a position nobody wrote is silence. It also hands the
  // microphone back with positions on the same clock.
  const WORKLET = `
class CardenzaOutput extends AudioWorkletProcessor {
  constructor() {
    super();
    this.capacity = 1 << 16;
    this.ring = new Float32Array(this.capacity);
    this.tags = new Float64Array(this.capacity).fill(-1);
    this.offsetFrames = 0;
    this.latencyFrames = 2646;
    this.step = ${SAMPLE_RATE} / sampleRate;
    this.late = 0;
    this.quanta = 0;
    this.energy = 0;
    this.frames = 0;
    this.mic = false;
    this.port.onmessage = (event) => this.receive(event.data);
  }
  playing() { return currentFrame * this.step + this.offsetFrames - this.latencyFrames; }
  receive(data) {
    if (data.offsetFrames !== undefined) this.offsetFrames = data.offsetFrames;
    if (data.latencyFrames !== undefined) this.latencyFrames = data.latencyFrames;
    if (data.mic !== undefined) this.mic = data.mic;
    if (!data.samples) return;
    const samples = data.samples;
    const position = data.position;
    const due = this.playing();
    if (position < due) this.late += Math.min(samples.length, due - position);
    for (let index = 0; index < samples.length; index++) {
      const at = position + index;
      const slot = at % this.capacity;
      this.ring[slot] = samples[index];
      this.tags[slot] = at;
    }
  }
  sample(at) {
    const slot = at % this.capacity;
    return this.tags[slot] === at ? this.ring[slot] : 0;
  }
  process(inputs, outputs) {
    const output = outputs[0];
    const first = output[0];
    let position = this.playing();
    for (let index = 0; index < first.length; index++, position += this.step) {
      const whole = Math.floor(position);
      const fraction = position - whole;
      const a = this.sample(whole);
      first[index] = fraction === 0 ? a : a + (this.sample(whole + 1) - a) * fraction;
    }
    for (let index = 0; index < first.length; index++) this.energy += first[index] * first[index];
    this.frames += first.length;
    for (let channel = 1; channel < output.length; channel++) output[channel].set(first);
    const input = inputs[0];
    if (this.mic && input && input[0] && input[0].length) {
      this.port.postMessage({ mic: input[0].slice(), position: Math.round(currentFrame * this.step + this.offsetFrames) });
    }
    if (++this.quanta >= 64) {
      // About every 190 ms: the level that went out (for the page's meter and tests) and late frames.
      this.port.postMessage({ rms: Math.sqrt(this.energy / Math.max(1, this.frames)), late: this.late });
      this.quanta = 0;
      this.energy = 0;
      this.frames = 0;
      this.late = 0;
    }
    return true;
  }
}
registerProcessor('cardenza-output', CardenzaOutput);
`;

  let audioStarting = false;
  async function startAudio() {
    if (audio || audioStarting) {
      if (audio && audio.context.state !== 'running') audio.context.resume().catch(() => {});
      return;
    }
    const Context = window.AudioContext || window.webkitAudioContext;
    if (!Context) return;
    audioStarting = true;
    try {
      let context;
      try { context = new Context({ sampleRate: SAMPLE_RATE, latencyHint: 'interactive' }); } catch (_) { context = new Context(); }
      if (!context.audioWorklet) throw new Error('no AudioWorklet');
      const url = URL.createObjectURL(new Blob([WORKLET], { type: 'application/javascript' }));
      await context.audioWorklet.addModule(url);
      const node = new AudioWorkletNode(context, 'cardenza-output', { numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2] });
      node.connect(context.destination);
      audio = { context, node, latencyFrames: 2646, micSource: null, micStream: null, rms: 0, peakRms: 0, lateFrames: 0, posted: 0 };
      node.port.onmessage = (event) => {
        const data = event.data;
        if (data.rms !== undefined) {
          audio.rms = data.rms;
          audio.peakRms = Math.max(audio.peakRms, data.rms);
        }
        if (data.late) {
          audio.lateFrames += data.late;
          // Frames came too late for their time: play everything a little later from now on.
          audio.latencyFrames = Math.min(audio.latencyFrames + 882, 13230);
          sendClock();
        }
        if (data.mic) pushMicrophone(data.mic, data.position);
      };
      context.onstatechange = () => { audioClock(); hint.hidden = context.state === 'running'; };
      await context.resume();
      audioClock();
      sendClock();
      hint.hidden = context.state === 'running';
    } catch (error) {
      log('cardenza-sim: no sound (' + (error && error.message ? error.message : error) + ')', true);
      hint.hidden = true;
    } finally {
      audioStarting = false;
    }
  }

  function drainAudio() {
    if (!instance) return;
    for (let guard = 0; guard < 64; guard++) {
      const count = instance._cardenza_web_audio_take();
      if (count <= 0) break;
      if (!audio) continue; // no output yet: the frames are not heard, the firmware runs on
      const pointer = instance._cardenza_web_audio_buffer() >> 2;
      const samples = instance.HEAPF32.slice(pointer, pointer + count);
      audio.posted += count;
      audio.node.port.postMessage({ samples, position: instance._cardenza_web_audio_position() }, [samples.buffer]);
    }
  }

  // ---- the microphone ---------------------------------------------------------------------------

  let micWanted = false;
  async function updateMicrophone() {
    if (!instance) return;
    const wanted = instance._cardenza_web_mic_wanted() !== 0;
    if (wanted === micWanted) return;
    micWanted = wanted;
    if (!audio) return; // without sound running there is no capture either: the firmware hears silence
    audio.node.port.postMessage({ mic: wanted });
    if (wanted && !audio.micSource) {
      if (!allowed('microphone') || !navigator.mediaDevices) {
        log('cardenza-sim: no microphone in this frame: the firmware hears silence');
        return;
      }
      try {
        const stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false } });
        if (!micWanted) { stream.getTracks().forEach((track) => track.stop()); return; }
        audio.micStream = stream;
        audio.micSource = audio.context.createMediaStreamSource(stream);
        audio.micSource.connect(audio.node);
      } catch (error) {
        log('cardenza-sim: no microphone (' + (error && error.name ? error.name : error) + '): the firmware hears silence', true);
      }
    } else if (!wanted && audio.micSource) {
      audio.micSource.disconnect();
      audio.micStream.getTracks().forEach((track) => track.stop());
      audio.micSource = null;
      audio.micStream = null;
    }
  }

  function pushMicrophone(samples, position) {
    if (!instance) return;
    const pointer = instance._cardenza_web_mic_buffer() >> 2;
    for (let at = 0; at < samples.length; at += 4096) {
      const chunk = samples.subarray(at, at + 4096);
      instance.HEAPF32.set(chunk, pointer);
      instance._cardenza_web_mic_push(position + at, chunk.length);
    }
  }

  // ---- input ------------------------------------------------------------------------------------

  // A key is down while anything holds it: a computer key, a finger, the mouse, a latch, the test API.
  const holders = new Map();
  function holderSet(x, y) {
    const id = y * COLUMNS + x;
    if (!holders.has(id)) holders.set(id, new Set());
    return holders.get(id);
  }
  function press(x, y, source) {
    const set = holderSet(x, y);
    const was = set.size > 0;
    set.add(source);
    if (!was) {
      keyElements[y * COLUMNS + x].key.classList.add('down');
      if (instance) { instance._cardenza_web_key(x, y, 1); pumpSoon(); }
    }
  }
  function release(x, y, source) {
    const set = holderSet(x, y);
    if (!set.delete(source) || set.size > 0) return;
    keyElements[y * COLUMNS + x].key.classList.remove('down', 'latched');
    if (instance) { instance._cardenza_web_key(x, y, 0); pumpSoon(); }
  }
  function releaseEverything(keepLatches) {
    for (const [id, set] of holders) {
      for (const source of [...set]) if (!keepLatches || source !== 'latch') release(id % COLUMNS, Math.floor(id / COLUMNS), source);
    }
    setHome(false);
  }

  let homeDown = false;
  function setHome(down) {
    if (down === homeDown) return;
    homeDown = down;
    homeButton.classList.toggle('down', down);
    if (instance) { instance._cardenza_web_home(down ? 1 : 0); pumpSoon(); }
  }

  addEventListener('keydown', (event) => {
    startAudio();
    if (event.metaKey) return; // ⌘ shortcuts stay the browser's
    if (event.code === HOME_CODE) {
      event.preventDefault();
      if (!event.repeat) setHome(true);
      return;
    }
    const position = CODES.get(event.code);
    if (!position) return;
    event.preventDefault();
    if (event.repeat) return; // the firmware repeats keys itself if it wants to
    press(position[0], position[1], 'code:' + event.code);
  });
  addEventListener('keyup', (event) => {
    if (event.code === HOME_CODE) { setHome(false); return; }
    const position = CODES.get(event.code);
    if (!position) return;
    event.preventDefault();
    release(position[0], position[1], 'code:' + event.code);
  });
  addEventListener('blur', () => releaseEverything(true));
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
      releaseEverything(true);
      persistFiles();
    }
  });

  // Mouse and touch: each pointer holds the key it went down on; a right click latches a key.
  const pointers = new Map();
  keysBox.addEventListener('pointerdown', (event) => {
    const key = event.target.closest('.key');
    if (!key) return;
    event.preventDefault();
    stage.focus({ preventScroll: true });
    startAudio();
    const x = Number(key.dataset.x);
    const y = Number(key.dataset.y);
    if (event.button === 2) {
      const set = holderSet(x, y);
      if (set.has('latch')) release(x, y, 'latch');
      else { press(x, y, 'latch'); key.classList.add('latched'); }
      return;
    }
    try { key.setPointerCapture(event.pointerId); } catch (_) { /* synthetic events */ }
    pointers.set(event.pointerId, [x, y]);
    press(x, y, 'pointer:' + event.pointerId);
  });
  function pointerEnd(event) {
    const position = pointers.get(event.pointerId);
    if (!position) return;
    pointers.delete(event.pointerId);
    release(position[0], position[1], 'pointer:' + event.pointerId);
  }
  keysBox.addEventListener('pointerup', pointerEnd);
  keysBox.addEventListener('pointercancel', pointerEnd);
  board.addEventListener('contextmenu', (event) => event.preventDefault());
  homeButton.addEventListener('pointerdown', (event) => {
    event.preventDefault();
    startAudio();
    try { homeButton.setPointerCapture(event.pointerId); } catch (_) { /* synthetic events */ }
    setHome(true);
  });
  homeButton.addEventListener('pointerup', () => setHome(false));
  homeButton.addEventListener('pointercancel', () => setHome(false));
  stage.addEventListener('pointerdown', () => { stage.focus({ preventScroll: true }); startAudio(); });

  // ---- USB: Web MIDI and the app channel ---------------------------------------------------------

  let midi = null;
  const sysexOut = [];
  // Bytes in a USB-MIDI packet by its code index number.
  const PACKET_BYTES = [0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1];

  function midiToPackets(bytes) {
    const packets = [];
    const status = bytes[0];
    if (status === 0xF0) {
      for (let at = 0; at < bytes.length; at += 3) {
        const chunk = bytes.slice(at, at + 3);
        const last = at + 3 >= bytes.length;
        const cin = last ? 0x4 + chunk.length : 0x4;
        packets.push([cin, chunk[0] || 0, chunk[1] || 0, chunk[2] || 0]);
      }
    } else if (status >= 0x80 && status < 0xF0) {
      packets.push([status >> 4, bytes[0], bytes[1] || 0, bytes[2] || 0]);
    } else if (status === 0xF1 || status === 0xF3) {
      packets.push([0x2, bytes[0], bytes[1] || 0, 0]);
    } else if (status === 0xF2) {
      packets.push([0x3, bytes[0], bytes[1] || 0, bytes[2] || 0]);
    } else if (status >= 0xF4) {
      packets.push([0xF, bytes[0], 0, 0]);
    }
    return packets;
  }

  function sendToHostPorts(bytes) {
    if (!midi) return;
    for (const output of midi.outputs.values()) {
      try { output.send(bytes); } catch (_) { /* a port that went away */ }
    }
  }

  function midiOut(packet) {
    const cin = packet[0] & 0x0F;
    const count = PACKET_BYTES[cin];
    if (cin >= 0x4 && cin <= 0x7) {
      for (let index = 0; index < count; index++) sysexOut.push(packet[1 + index]);
      if (cin !== 0x4 || packet[3] === 0xF7) {
        const message = sysexOut.splice(0, sysexOut.length);
        if (message[0] === 0xF0 && message[message.length - 1] === 0xF7) sendToHostPorts(message);
      }
      return;
    }
    if (count > 0) sendToHostPorts(packet.slice(1, 1 + count));
  }

  function startMidi() {
    if (midi || !navigator.requestMIDIAccess) return;
    if (!allowed('midi')) {
      log('cardenza-sim: no Web MIDI in this frame: USB MIDI goes nowhere');
      return;
    }
    navigator.requestMIDIAccess({ sysex: true }).then((access) => {
      midi = access;
      const listen = (input) => {
        input.onmidimessage = (event) => {
          if (!instance) return;
          for (const packet of midiToPackets(Array.from(event.data))) instance._cardenza_web_midi_in(...packet);
          pumpSoon();
        };
      };
      access.inputs.forEach(listen);
      access.onstatechange = (event) => { if (event.port.type === 'input' && event.port.state === 'connected') listen(event.port); };
      log('cardenza-sim: USB MIDI goes to the computer\'s MIDI ports (' + access.inputs.size + ' in, ' + access.outputs.size + ' out)');
    }, (error) => {
      log('cardenza-sim: no Web MIDI here (' + (error && error.name ? error.name : error) + '): USB MIDI goes nowhere', false);
    });
  }

  const pendingApp = [];
  function appIn(bytes) {
    if (!instance) { pendingApp.push(bytes); return; }
    const pointer = instance._cardenza_web_app_buffer();
    const length = Math.min(bytes.length, 64);
    instance.HEAPU8.set(bytes.slice(0, length), pointer);
    instance._cardenza_web_app_in(length);
    pumpSoon();
  }
  addEventListener('message', (event) => {
    const data = event.data;
    if (!data || typeof data !== 'object') return;
    if (data.cardenza_sim === 'app' && Array.isArray(data.data)) appIn(Uint8Array.from(data.data, (value) => value & 0xFF));
    else if (data.cardenza_sim === 'screenshot') answerScreenshot(data);
    else if (data.cardenza_sim === 'host') shots.hidden = data.screenshot === true;
  });

  // ---- storage ------------------------------------------------------------------------------------

  let persistent = false;
  function probeIndexedDB() {
    return new Promise((resolve) => {
      try {
        if (!window.indexedDB) { resolve(false); return; }
        const request = indexedDB.open('cardenza-probe');
        const timer = setTimeout(() => resolve(false), 1500);
        request.onsuccess = () => { clearTimeout(timer); try { request.result.close(); } catch (_) { /* closed */ } resolve(true); };
        request.onerror = () => { clearTimeout(timer); resolve(false); };
      } catch (_) {
        resolve(false); // an opaque-origin sandbox: no storage of its own
      }
    });
  }

  function mountStorage(module) {
    const FS = module.FS;
    const folders = options.card ? ['/sdcard', '/.cardenza'] : ['/.cardenza'];
    for (const folder of folders) {
      try { FS.mkdir(folder); } catch (_) { /* there already */ }
      if (persistent) FS.mount(module.IDBFS, { autoPersist: true }, folder);
    }
    if (!persistent) return;
    module.addRunDependency('cardenza-storage');
    FS.syncfs(true, (error) => {
      if (error) log('cardenza-sim: could not read the stored card (' + error + ')', true);
      module.removeRunDependency('cardenza-storage');
    });
  }

  function persistFiles(done) {
    if (!instance || !persistent) { if (done) done(); return; }
    try { instance.FS.syncfs(false, () => { if (done) done(); }); } catch (_) { if (done) done(); }
  }

  // ---- the firmware ---------------------------------------------------------------------------------

  let instance = null;      // the running module
  let starting = null;
  let compiled = null;      // the compiled wasm, kept across restarts
  let generation = 0;
  let resetReason = 1;      // ESP_RST_POWERON
  let booted = false;
  let readyResolve;
  const ready = new Promise((resolve) => { readyResolve = resolve; });
  const crashes = [];

  function scriptUrl() { return new URL(options.module, location.href).href; }
  function wasmUrl() { return scriptUrl().replace(/\.js(\?.*)?$/, '.wasm'); }

  function loadScript() {
    if (window.createCardenzaFirmware) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const script = document.createElement('script');
      script.src = options.module;
      script.onload = () => resolve();
      script.onerror = () => reject(new Error('could not load ' + options.module));
      document.head.appendChild(script);
    });
  }

  async function compile() {
    if (compiled) return compiled;
    const url = wasmUrl();
    try {
      compiled = await WebAssembly.compileStreaming(fetch(url));
    } catch (_) {
      // A server that does not say application/wasm: compile from the bytes.
      const response = await fetch(url);
      if (!response.ok) throw new Error(url + ': ' + response.status);
      compiled = await WebAssembly.compile(await response.arrayBuffer());
    }
    return compiled;
  }

  async function start() {
    const mine = ++generation;
    overlay.hidden = false;
    overlay.className = 'overlay';
    overlay.textContent = booted ? 'RESTART' : 'LOADING';
    try {
      await loadScript();
      persistent = await probeIndexedDB();
      const wasm = await compile();
      const config = { resetReason, card: options.card ? 1 : 0, cardMegabytes: options.cardMegabytes };
      const module = await window.createCardenzaFirmware({
        cardenzaConfig: config,
        noExitRuntime: true,
        print: (text) => log(text, false),
        printErr: (text) => log(text, true),
        locateFile: (path) => new URL(path, scriptUrl()).href,
        instantiateWasm: (imports, receive) => {
          WebAssembly.instantiate(wasm, imports).then((made) => receive(made, wasm), (error) => crash(error));
          return {};
        },
        preRun: [(module) => mountStorage(module)],
        onAbort: (what) => { if (mine === generation) crash(what); },
        cardenzaAudioClock: audioClock,
        cardenzaBooted: () => onBooted(mine),
        cardenzaRestart: (reason) => setTimeout(() => restart(reason), 0),
        cardenzaFilesChanged: () => persistFiles(),
        cardenzaIr: (address, command, nec) => onIr(address, command, nec),
        cardenzaMidiOut: midiOut,
        cardenzaAppReport: (bytes) => post({ cardenza_sim: 'app_report', data: Array.from(bytes) }),
        cardenzaUsbStarted: () => startMidi(),
      });
      if (mine !== generation) return;
      instance = module;
      starting = null;
      while (pendingApp.length) appIn(pendingApp.shift());
      // Keys still held from before a restart are held on the new instance too.
      for (const [id, set] of holders) if (set.size) instance._cardenza_web_key(id % COLUMNS, Math.floor(id / COLUMNS), 1);
      if (homeDown) instance._cardenza_web_home(1);
      pumpNow();
    } catch (error) {
      crash(error);
    }
  }

  function onBooted(mine) {
    if (mine !== generation) return;
    overlay.hidden = true;
    if (!booted) {
      booted = true;
      readyResolve();
      post({ cardenza_sim: 'ready' });
      post({ cardenza_sim: 'keys', keys: keyTable() });
      post({ cardenza_sim: 'features', features: ['screenshot'] });
    } else {
      post({ cardenza_sim: 'restarted', reason: resetReason });
    }
  }

  function stopInstance() {
    generation++;
    pumpTimer.cancel();
    instance = null;
    micWanted = false;
    if (audio && audio.micSource) {
      audio.micSource.disconnect();
      audio.micStream.getTracks().forEach((track) => track.stop());
      audio.micSource = null;
      audio.micStream = null;
    }
  }

  // esp_restart(), a crash or a USB command: a new instance of the firmware on the same page (the
  // sound, the screen's frame and MIDI stay), told why it started.
  function restart(reason) {
    if (instance === null && starting) return;
    const old = instance;
    stopInstance();
    resetReason = reason;
    starting = true;
    const go = () => start();
    if (old && persistent) {
      try { old.FS.syncfs(false, go); } catch (_) { go(); }
    } else {
      go();
    }
  }

  function crash(error) {
    const message = error && error.message ? error.message : String(error || 'abort()');
    log('cardenza-sim: the firmware stopped: ' + message, true);
    stopInstance();
    const now = performance.now();
    crashes.push(now);
    while (crashes.length && now - crashes[0] > 10000) crashes.shift();
    overlay.hidden = false;
    overlay.className = 'overlay panic';
    const lines = errorLines.slice(-6).join('\n');
    if (crashes.length >= 3) {
      overlay.textContent = 'The firmware keeps crashing; it stays stopped.\n\n' + message + '\n' + lines;
      post({ cardenza_sim: 'stopped', error: message });
      return;
    }
    post({ cardenza_sim: 'crashed', error: message });
    overlay.textContent = 'Guru Meditation: the firmware stopped. Restarting...\n\n' + message + '\n' + lines;
    setTimeout(() => restart(4), 1000); // ESP_RST_PANIC
  }

  function onIr(address, command, nec) {
    irDot.classList.add('on');
    setTimeout(() => irDot.classList.remove('on'), 120);
    post({ cardenza_sim: 'ir', address, command, nec: !!nec });
  }

  // ---- the scheduler pump ------------------------------------------------------------------------------

  // One pending wake-up at a time. A delay is set from a MessageChannel message, not from inside a
  // timer, so the browser does not stretch it to 4 ms after a few nested timers.
  const pumpTimer = (() => {
    const channel = new MessageChannel();
    let messagePending = false;
    let timer = 0;
    let wantDelay = -1;
    channel.port1.onmessage = () => {
      messagePending = false;
      if (wantDelay <= 0) { pump(); return; }
      timer = setTimeout(() => { timer = 0; pump(); }, wantDelay);
    };
    return {
      schedule(delay) {
        if (timer) { clearTimeout(timer); timer = 0; }
        wantDelay = delay;
        if (!messagePending) { messagePending = true; channel.port2.postMessage(0); }
      },
      cancel() {
        if (timer) { clearTimeout(timer); timer = 0; }
        wantDelay = -2;
      },
    };
  })();

  function pump() {
    const running = instance;
    if (!running) return;
    try {
      running._cardenza_web_run(8);
    } catch (error) {
      // An abort reported itself already (onAbort); anything else stops the firmware here.
      if (instance === running) crash(error);
      return;
    }
    if (!instance) return; // restarting
    drainAudio();
    updateMicrophone();
    const delay = instance._cardenza_web_next_delay();
    // Nothing pending: look again now and then anyway (the audio clock moves on its own).
    pumpTimer.schedule(delay < 0 ? 50 : Math.min(delay, 50));
  }
  function pumpNow() { pumpTimer.schedule(0); }
  function pumpSoon() { if (instance) pumpTimer.schedule(0); }

  // ---- drawing --------------------------------------------------------------------------------------

  let frameSerial = -1;
  let ledSerial = -1;
  let shownBrightness = -1;

  function drawScreen() {
    const pointer = instance._cardenza_web_display();
    const words = instance.HEAPU32;
    const serial = words[pointer >> 2];
    if (serial === frameSerial) return;
    frameSerial = serial;
    shotButton.disabled = framedButton.disabled = false;
    const permille = words[(pointer >> 2) + 1];
    const pixels = new Uint16Array(instance.HEAPU8.buffer, pointer + 12, WIDTH * HEIGHT);
    const data = image.data;
    for (let index = 0, out = 0; index < pixels.length; index++, out += 4) {
      const value = pixels[index];
      const red = value >> 11;
      const green = (value >> 5) & 63;
      const blue = value & 31;
      data[out] = (red << 3) | (red >> 2);
      data[out + 1] = (green << 2) | (green >> 4);
      data[out + 2] = (blue << 3) | (blue >> 2);
      data[out + 3] = 255;
    }
    context2d.putImageData(image, 0, 0);
    // The backlight's light is linear in its duty; the screen's values are not.
    const brightness = Math.round(Math.pow(permille / 1000, 1 / 2.2) * 1000) / 1000;
    if (brightness !== shownBrightness) {
      shownBrightness = brightness;
      canvas.style.filter = 'brightness(' + brightness + ')';
    }
  }

  function drawLeds() {
    const pointer = instance._cardenza_web_leds();
    const bytes = instance.HEAPU8;
    const serial = instance.HEAPU32[pointer >> 2];
    if (serial === ledSerial) return;
    ledSerial = serial;
    const global = GLOBAL_DUTY[Math.min(bytes[pointer + 4 + 56], 15)];
    const enabled = bytes[pointer + 4 + 57] !== 0;
    for (let index = 0; index < ROWS * COLUMNS; index++) {
      const light = enabled ? (Math.min(bytes[pointer + 4 + index], 15) / 15) * global : 0;
      const entry = keyElements[index];
      if (entry.light === light) continue;
      entry.light = light;
      // How bright it looks: the eye is closer to light^(1/2.2) than to the light itself.
      entry.glow.style.opacity = light > 0 ? Math.pow(light, 1 / 2.2).toFixed(3) : '0';
    }
  }

  function animate() {
    requestAnimationFrame(animate);
    const running = instance;
    if (!running) return;
    try {
      drawScreen();
      drawLeds();
      drainAudio();
    } catch (error) {
      if (instance === running) crash(error);
    }
  }

  // ---- screenshots ----------------------------------------------------------------------------------

  // The community's standard picture (https://cardenza.203.io/community/guide/images; 203cli images frame draws the
  // same): 1280 x 720, one flat colour, a #1A1C1E bezel 32 px wide with round outer corners, the screen at 4 times
  // at (160, 90).
  const SHOT = { scale: 4, width: 1280, height: 720, x: 160, y: 90, band: 32, radius: 32, bezel: '#1A1C1E', color: '#E85A2C' };
  shotButton.disabled = framedButton.disabled = true;

  // '#RRGGBB' from '#RGB', '#RRGGBB' (the # optional), else null.
  function shotColor(text) {
    const match = /^#?([0-9a-f]{3}|[0-9a-f]{6})$/i.exec(String(text === undefined || text === null ? '' : text).trim());
    if (!match) return null;
    const hex = match[1].length === 3 ? match[1].replace(/./g, '$&$&') : match[1];
    return '#' + hex.toUpperCase();
  }

  // The file's name: the firmware's title in plain letters, what it is, the time.
  function shotName(framed, date) {
    const slug = options.title.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 40) || 'cardenza';
    const two = (n) => String(n).padStart(2, '0');
    const stamp = date.getFullYear() + two(date.getMonth() + 1) + two(date.getDate()) + '-' + two(date.getHours()) + two(date.getMinutes()) + two(date.getSeconds());
    return slug + '-' + (framed ? 'framed' : 'screen') + '-' + stamp + '.png';
  }

  // The screen as the panel holds it (the backlight's dimming is a style, not in the canvas), enlarged pixel for pixel.
  function shotCanvas(framed, color) {
    const out = document.createElement('canvas');
    const width = WIDTH * SHOT.scale;
    const height = HEIGHT * SHOT.scale;
    out.width = framed ? SHOT.width : width;
    out.height = framed ? SHOT.height : height;
    const c = out.getContext('2d');
    c.imageSmoothingEnabled = false;
    if (framed) {
      c.fillStyle = color;
      c.fillRect(0, 0, SHOT.width, SHOT.height);
      const x = SHOT.x - SHOT.band;
      const y = SHOT.y - SHOT.band;
      const w = width + 2 * SHOT.band;
      const h = height + 2 * SHOT.band;
      const r = SHOT.radius;
      c.fillStyle = SHOT.bezel;
      c.beginPath();
      c.moveTo(x + r, y);
      c.arcTo(x + w, y, x + w, y + h, r);
      c.arcTo(x + w, y + h, x, y + h, r);
      c.arcTo(x, y + h, x, y, r);
      c.arcTo(x, y, x + w, y, r);
      c.closePath();
      c.fill();
    }
    c.drawImage(canvas, framed ? SHOT.x : 0, framed ? SHOT.y : 0, width, height);
    return out;
  }

  // {name, width, height, framed, color, blob}; rejects before the firmware drew its first frame.
  function screenshot(request) {
    const framed = !!(request && request.framed);
    const color = shotColor(request && request.color) || SHOT.color;
    if (frameSerial === -1) return Promise.reject(new Error('no_frame'));
    const out = shotCanvas(framed, color);
    return new Promise((resolve, reject) => {
      out.toBlob((blob) => {
        if (!blob) { reject(new Error('encode_failed')); return; }
        resolve({ name: shotName(framed, new Date()), width: out.width, height: out.height, framed, color: framed ? color : null, blob });
      }, 'image/png');
    });
  }

  function answerScreenshot(request) {
    const id = typeof request.id === 'string' || typeof request.id === 'number' ? request.id : null;
    screenshot(request).then((shot) => shot.blob.arrayBuffer().then((png) => {
      post({ cardenza_sim: 'screenshot', id, framed: shot.framed, color: shot.color, width: shot.width, height: shot.height, name: shot.name, png }, [png]);
    })).catch((error) => post({ cardenza_sim: 'screenshot', id, error: error && error.message ? error.message : 'failed' }));
  }

  function saveScreenshot(button, framed) {
    screenshot({ framed }).then((shot) => {
      const link = document.createElement('a');
      link.href = URL.createObjectURL(shot.blob);
      link.download = shot.name;
      document.body.appendChild(link);
      link.click();
      link.remove();
      setTimeout(() => URL.revokeObjectURL(link.href), 10000);
      log('cardenza-sim: saved ' + shot.name + ' (' + shot.width + ' x ' + shot.height + ')', false);
      const label = button.dataset.label || (button.dataset.label = button.textContent);
      button.classList.add('saved');
      button.textContent = 'SAVED';
      setTimeout(() => { button.classList.remove('saved'); button.textContent = label; }, 1200);
    }, (error) => log('cardenza-sim: no screenshot (' + (error && error.message ? error.message : error) + ')', true));
  }

  for (const [button, framed] of [[shotButton, false], [framedButton, true]]) {
    button.tabIndex = -1;
    // The keyboard stays the firmware's: the button never takes the focus.
    button.addEventListener('pointerdown', (event) => { event.preventDefault(); event.stopPropagation(); stage.focus({ preventScroll: true }); });
    button.addEventListener('click', () => saveScreenshot(button, framed));
  }

  // ---- the test and diagnostics API -------------------------------------------------------------

  window.cardenzaSim = {
    ready,
    logs,
    keyTable,
    module: () => instance,
    key(x, y, down) { if (down) press(x, y, 'api'); else release(x, y, 'api'); },
    home(down) { setHome(!!down); },
    frame() {
      if (!instance) return null;
      const pointer = instance._cardenza_web_display();
      const words = instance.HEAPU32;
      return {
        serial: words[pointer >> 2],
        backlight: words[(pointer >> 2) + 1] / 1000,
        showing: words[(pointer >> 2) + 2] !== 0,
        pixels: new Uint16Array(instance.HEAPU8.buffer, pointer + 12, WIDTH * HEIGHT).slice(),
      };
    },
    leds() {
      if (!instance) return null;
      const pointer = instance._cardenza_web_leds();
      const bytes = instance.HEAPU8;
      return {
        levels: Array.from(bytes.subarray(pointer + 4, pointer + 4 + 56)),
        global: bytes[pointer + 60],
        enabled: bytes[pointer + 61] !== 0,
      };
    },
    tasks: () => (instance ? instance.UTF8ToString(instance._cardenza_web_tasks()) : ''),
    audio: () => (audio ? { state: audio.context.state, sampleRate: audio.context.sampleRate, latencyMs: audio.latencyFrames / 44.1,
      rms: audio.rms, peakRms: audio.peakRms, postedFrames: audio.posted, lateFrames: audio.lateFrames } : null),
    restart: () => restart(3), // ESP_RST_SW
    app: (bytes) => appIn(Uint8Array.from(bytes)),
    // A screenshot as the buttons make it: {name, width, height, framed, color, blob}.
    screenshot: (request) => screenshot(request || {}),
  };

  addEventListener('pointerdown', () => startAudio(), { capture: true });
  if (!window.AudioContext && !window.webkitAudioContext) hint.hidden = true;
  stage.focus({ preventScroll: true });
  requestAnimationFrame(animate);
  start();
})();
