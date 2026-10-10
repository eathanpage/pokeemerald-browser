// Frontend for the pokeemerald WebAssembly port: drives the frame loop,
// draws the screen, handles input and audio, and stores the save file.
'use strict';

(() => {
  // GBA timing: 280896 cycles per frame at 16.78 MHz (~59.73 frames/s).
  const FRAME_MS = (1000 * 280896) / 16777216;
  const FLASH_SIZE = 0x20000;
  const AUDIO_MAX_BUFFERED = 0.12; // seconds of audio queued before dropping

  const KEY_BITS = {
    A: 1, B: 2, SELECT: 4, START: 8, RIGHT: 16, LEFT: 32, UP: 64, DOWN: 128, R: 256, L: 512,
  };

  const KEYBOARD_MAP = {
    ArrowUp: 'UP', ArrowDown: 'DOWN', ArrowLeft: 'LEFT', ArrowRight: 'RIGHT',
    KeyX: 'A', KeyZ: 'B', KeyA: 'L', KeyS: 'R',
    Enter: 'START', Backspace: 'SELECT', ShiftLeft: 'SELECT', ShiftRight: 'SELECT',
  };

  // Standard gamepad mapping, Nintendo-style: the east face button is A.
  const GAMEPAD_MAP = [
    [0, 'B'], [1, 'A'], [2, 'B'], [3, 'A'], [4, 'L'], [5, 'R'],
    [8, 'SELECT'], [9, 'START'], [12, 'UP'], [13, 'DOWN'], [14, 'LEFT'], [15, 'RIGHT'],
  ];

  const $ = (id) => document.getElementById(id);
  const canvas = $('canvas');
  const ctx = canvas.getContext('2d', { alpha: false });
  const overlay = $('overlay');

  let mod = null;
  let wasmBinary = null;
  let running = false;
  let imageData = null;
  let frameView = null;
  let keyboardKeys = 0;
  let touchKeys = 0;
  let gamepadKeys = 0;
  let fastForwardHeld = false;
  let fastForwardToggle = false;
  let touchFastForward = false;
  let gamepadFastForward = false;
  let lastTime = 0;
  let accumulator = 0;
  let saveTimer = null;
  let paused = false;

  // Exposed for debugging and automated tests.
  const stats = { frames: 0, frameMs: 0, audioBuffered: 0 };
  window.emeraldStats = stats;

  // ------------------------------------------------------------------------
  // UI helpers

  function setOverlay(title, detail = '', progress = null) {
    overlay.hidden = false;
    $('overlay-title').textContent = title;
    $('overlay-detail').textContent = detail;
    $('progress').hidden = progress === null;
    if (progress !== null) $('progress-bar').style.width = `${Math.round(progress * 100)}%`;
  }

  function setStatus(text) {
    $('status').textContent = text;
  }

  function setMenuOpen(open) {
    $('toolbar').classList.toggle('open', open);
    $('btn-menu').setAttribute('aria-expanded', String(open));
  }

  function showPanel(id, show) {
    if (show) setMenuOpen(false);
    for (const panel of document.querySelectorAll('.panel')) {
      const visible = panel.id === id && show;
      panel.hidden = !visible;
    }
    $('btn-save-menu').setAttribute('aria-expanded', String(id === 'save-menu' && show));
    $('btn-help').setAttribute('aria-expanded', String(id === 'help' && show));
  }

  // ------------------------------------------------------------------------
  // Save storage (IndexedDB, with localStorage as a fallback)

  const SaveStore = {
    dbPromise: null,

    open() {
      if (!this.dbPromise) {
        this.dbPromise = new Promise((resolve) => {
          if (!('indexedDB' in window)) {
            resolve(null);
            return;
          }
          const req = indexedDB.open('pokeemerald', 1);
          req.onupgradeneeded = () => req.result.createObjectStore('saves');
          req.onsuccess = () => resolve(req.result);
          req.onerror = () => resolve(null);
        });
      }
      return this.dbPromise;
    },

    async load() {
      const db = await this.open();
      if (db) {
        const data = await new Promise((resolve) => {
          const req = db.transaction('saves').objectStore('saves').get('flash');
          req.onsuccess = () => resolve(req.result || null);
          req.onerror = () => resolve(null);
        });
        if (data) return new Uint8Array(data);
      }
      try {
        const b64 = localStorage.getItem('pokeemerald-flash');
        if (b64) return Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
      } catch (e) { /* storage unavailable */ }
      return null;
    },

    async store(bytes) {
      const db = await this.open();
      if (db) {
        const ok = await new Promise((resolve) => {
          const tx = db.transaction('saves', 'readwrite');
          tx.objectStore('saves').put(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength), 'flash');
          tx.oncomplete = () => resolve(true);
          tx.onerror = () => resolve(false);
        });
        if (ok) return true;
      }
      try {
        let s = '';
        for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
        localStorage.setItem('pokeemerald-flash', btoa(s));
        return true;
      } catch (e) {
        return false;
      }
    },

    async clear() {
      const db = await this.open();
      if (db) {
        await new Promise((resolve) => {
          const tx = db.transaction('saves', 'readwrite');
          tx.objectStore('saves').delete('flash');
          tx.oncomplete = tx.onerror = () => resolve();
        });
      }
      try { localStorage.removeItem('pokeemerald-flash'); } catch (e) { /* ignore */ }
    },
  };

  function currentFlash() {
    const ptr = mod._Port_GetFlash();
    return mod.HEAPU8.slice(ptr, ptr + FLASH_SIZE);
  }

  function scheduleSave() {
    if (saveTimer) return;
    saveTimer = setTimeout(flushSave, 400);
  }

  async function flushSave() {
    if (saveTimer) {
      clearTimeout(saveTimer);
      saveTimer = null;
    }
    if (!mod) return;
    const ok = await SaveStore.store(currentFlash());
    setStatus(ok ? 'Game saved to this browser.' : 'Could not store the save file in this browser!');
  }

  // ------------------------------------------------------------------------
  // Audio: an AudioWorklet (or ScriptProcessor fallback) playing from a ring
  // buffer that the frame loop fills.

  const WORKLET_SOURCE = `
    class EmeraldAudio extends AudioWorkletProcessor {
      constructor() {
        super();
        this.size = sampleRate;               // one second, stereo interleaved
        this.buf = new Float32Array(this.size * 2);
        this.read = 0;
        this.count = 0;
        this.port.onmessage = (e) => {
          const data = e.data;
          const frames = data.length / 2;
          if (this.count + frames > this.size) return;
          let w = (this.read + this.count) % this.size;
          for (let i = 0; i < frames; i++) {
            this.buf[w * 2] = data[i * 2];
            this.buf[w * 2 + 1] = data[i * 2 + 1];
            w = (w + 1) % this.size;
          }
          this.count += frames;
        };
        this.reportCounter = 0;
      }
      process(inputs, outputs) {
        const out = outputs[0];
        const left = out[0], right = out[1] || out[0];
        for (let i = 0; i < left.length; i++) {
          if (this.count > 0) {
            left[i] = this.buf[this.read * 2];
            right[i] = this.buf[this.read * 2 + 1];
            this.read = (this.read + 1) % this.size;
            this.count--;
          } else {
            left[i] = right[i] = 0;
          }
        }
        if (++this.reportCounter >= 8) {
          this.reportCounter = 0;
          this.port.postMessage(this.count);
        }
        return true;
      }
    }
    registerProcessor('emerald-audio', EmeraldAudio);
  `;

  const Audio = {
    ctx: null,
    node: null,
    gain: null,
    buffered: 0,
    fallbackQueue: [],
    muted: false,
    volume: 0.8,

    async start() {
      if (this.ctx) {
        if (this.ctx.state !== 'running') await this.ctx.resume();
        return;
      }
      const AudioContextClass = window.AudioContext || window.webkitAudioContext;
      if (!AudioContextClass) return;
      this.ctx = new AudioContextClass({ latencyHint: 'interactive' });
      this.gain = this.ctx.createGain();
      this.gain.connect(this.ctx.destination);
      this.applyVolume();

      if (this.ctx.audioWorklet) {
        try {
          const url = URL.createObjectURL(new Blob([WORKLET_SOURCE], { type: 'application/javascript' }));
          await this.ctx.audioWorklet.addModule(url);
          this.node = new AudioWorkletNode(this.ctx, 'emerald-audio', { outputChannelCount: [2] });
          this.node.port.onmessage = (e) => { this.buffered = e.data; };
          this.node.connect(this.gain);
        } catch (e) {
          this.node = null;
        }
      }
      if (!this.node) {
        // ScriptProcessorNode works in insecure contexts (e.g. LAN testing).
        const proc = this.ctx.createScriptProcessor(1024, 0, 2);
        proc.onaudioprocess = (e) => this.fallbackProcess(e);
        proc.connect(this.gain);
        this.node = proc;
      }
      if (this.ctx.state !== 'running') await this.ctx.resume();
    },

    fallbackProcess(e) {
      const left = e.outputBuffer.getChannelData(0);
      const right = e.outputBuffer.getChannelData(1);
      let i = 0;
      while (i < left.length && this.fallbackQueue.length) {
        const chunk = this.fallbackQueue[0];
        const pos = chunk.pos || 0;
        const frames = Math.min(chunk.length / 2 - pos, left.length - i);
        for (let j = 0; j < frames; j++) {
          left[i + j] = chunk[(pos + j) * 2];
          right[i + j] = chunk[(pos + j) * 2 + 1];
        }
        i += frames;
        chunk.pos = pos + frames;
        this.buffered -= frames;
        if (chunk.pos >= chunk.length / 2) this.fallbackQueue.shift();
      }
      for (; i < left.length; i++) left[i] = right[i] = 0;
    },

    push(samples) {
      if (!this.ctx || !this.node) return;
      const frames = samples.length / 2;
      if (this.buffered > this.ctx.sampleRate * AUDIO_MAX_BUFFERED) return;
      if (this.node instanceof AudioWorkletNode) {
        this.node.port.postMessage(samples, [samples.buffer]);
        this.buffered += frames;
      } else {
        this.fallbackQueue.push(samples);
        this.buffered += frames;
      }
    },

    applyVolume() {
      if (this.gain) this.gain.gain.value = this.muted ? 0 : this.volume;
    },
  };

  // ------------------------------------------------------------------------
  // Input

  function isTyping(e) {
    const t = e.target;
    return t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA');
  }

  window.addEventListener('keydown', (e) => {
    if (!running || isTyping(e) || e.ctrlKey || e.metaKey || e.altKey) return;
    const key = KEYBOARD_MAP[e.code];
    if (key) {
      keyboardKeys |= KEY_BITS[key];
      e.preventDefault();
    } else if (e.code === 'Space') {
      fastForwardHeld = true;
      e.preventDefault();
    } else if (e.code === 'KeyF' && !e.repeat) {
      toggleFullscreen();
    } else if (e.code === 'KeyM' && !e.repeat) {
      toggleMute();
    } else if (e.code === 'KeyP' && !e.repeat) {
      setPaused(!paused);
    } else if (e.code === 'Escape') {
      showPanel(null, false);
    }
  });

  window.addEventListener('keyup', (e) => {
    const key = KEYBOARD_MAP[e.code];
    if (key) {
      keyboardKeys &= ~KEY_BITS[key];
      e.preventDefault();
    } else if (e.code === 'Space') {
      fastForwardHeld = false;
    }
  });

  window.addEventListener('blur', () => {
    keyboardKeys = 0;
    fastForwardHeld = false;
  });

  function pollGamepads() {
    gamepadKeys = 0;
    gamepadFastForward = false;
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    for (const pad of pads) {
      if (!pad) continue;
      for (const [index, key] of GAMEPAD_MAP) {
        if (pad.buttons[index] && pad.buttons[index].pressed) gamepadKeys |= KEY_BITS[key];
      }
      if (pad.buttons[7] && pad.buttons[7].value > 0.5) gamepadFastForward = true;
      const x = pad.axes[0] || 0;
      const y = pad.axes[1] || 0;
      if (x < -0.5) gamepadKeys |= KEY_BITS.LEFT;
      if (x > 0.5) gamepadKeys |= KEY_BITS.RIGHT;
      if (y < -0.5) gamepadKeys |= KEY_BITS.UP;
      if (y > 0.5) gamepadKeys |= KEY_BITS.DOWN;
    }
  }

  const TOUCH_MODES = ['auto', 'on', 'off'];

  function touchMode() {
    try {
      const mode = localStorage.getItem('pokeemerald-touch');
      if (TOUCH_MODES.includes(mode)) return mode;
    } catch (e) { /* ignore */ }
    return 'auto';
  }

  function applyTouchMode() {
    const mode = touchMode();
    const isTouchDevice = window.matchMedia('(pointer: coarse)').matches || navigator.maxTouchPoints > 0;
    const show = mode === 'on' || (mode === 'auto' && isTouchDevice);
    document.body.classList.toggle('touch', show);
    $('btn-touch').textContent = `Touch controls: ${mode}`;
  }

  // The D-pad is a single touch area: the direction comes from the angle of
  // the touch relative to its center, with narrow diagonal zones. A finger
  // that started on the D-pad keeps steering it even if it drifts off.
  function dpadKeys(x, y) {
    const rect = $('dpad').getBoundingClientRect();
    const dx = x - (rect.left + rect.width / 2);
    const dy = y - (rect.top + rect.height / 2);
    if (Math.hypot(dx, dy) < rect.width * 0.1) return 0;
    const angle = (Math.atan2(-dy, dx) * 180) / Math.PI; // 0 = right, 90 = up
    const a = (angle + 360) % 360;
    const DIAGONAL = 15; // half-width of the diagonal zones, in degrees
    let keys = 0;
    if (a < 45 + DIAGONAL || a > 315 - DIAGONAL) keys |= KEY_BITS.RIGHT;
    if (a > 45 - DIAGONAL && a < 135 + DIAGONAL) keys |= KEY_BITS.UP;
    if (a > 135 - DIAGONAL && a < 225 + DIAGONAL) keys |= KEY_BITS.LEFT;
    if (a > 225 - DIAGONAL && a < 315 + DIAGONAL) keys |= KEY_BITS.DOWN;
    return keys;
  }

  function setupTouch() {
    applyTouchMode();
    $('btn-touch').addEventListener('click', () => {
      const next = TOUCH_MODES[(TOUCH_MODES.indexOf(touchMode()) + 1) % TOUCH_MODES.length];
      try { localStorage.setItem('pokeemerald-touch', next); } catch (e) { /* ignore */ }
      applyTouchMode();
    });

    const stage = $('stage');
    const dpad = $('dpad');
    const buttons = Array.from(document.querySelectorAll('.pad-side .pad-btn'));
    // pointerId -> { dpad: true } or { button: element or null }
    const pointers = new Map();
    let lastKeys = 0;

    function buttonAt(x, y) {
      const el = document.elementFromPoint(x, y);
      return el && el.closest ? el.closest('.pad-side .pad-btn') : null;
    }

    function update(e) {
      let keys = 0;
      let fast = false;
      const pressed = new Set();
      for (const [id, p] of pointers) {
        if (p.dpad) {
          if (e && e.pointerId === id) p.keys = dpadKeys(e.clientX, e.clientY);
          keys |= p.keys;
        } else if (p.button) {
          pressed.add(p.button);
          if (p.button.dataset.action === 'fast') fast = true;
          else keys |= KEY_BITS[p.button.dataset.key];
        }
      }
      for (const btn of buttons) btn.classList.toggle('pressed', pressed.has(btn));
      dpad.classList.toggle('p-up', !!(keys & KEY_BITS.UP));
      dpad.classList.toggle('p-down', !!(keys & KEY_BITS.DOWN));
      dpad.classList.toggle('p-left', !!(keys & KEY_BITS.LEFT));
      dpad.classList.toggle('p-right', !!(keys & KEY_BITS.RIGHT));
      // A short buzz when something new gets pressed (Android only).
      if ((keys & ~lastKeys) && navigator.vibrate) navigator.vibrate(8);
      lastKeys = keys;
      touchKeys = keys;
      touchFastForward = fast;
    }

    stage.addEventListener('pointerdown', (e) => {
      if (!document.body.classList.contains('touch')) return;
      const onDpad = e.target.closest && e.target.closest('#dpad');
      const button = buttonAt(e.clientX, e.clientY);
      if (!onDpad && !button) return;
      e.preventDefault();
      pointers.set(e.pointerId, onDpad ? { dpad: true, keys: 0 } : { button });
      if (stage.setPointerCapture) stage.setPointerCapture(e.pointerId);
      update(e);
      Audio.start();
    });
    stage.addEventListener('pointermove', (e) => {
      const p = pointers.get(e.pointerId);
      if (!p) return;
      // Sliding onto another button presses that one instead.
      if (!p.dpad) p.button = buttonAt(e.clientX, e.clientY);
      update(e);
    });
    const release = (e) => {
      if (pointers.delete(e.pointerId)) update();
    };
    stage.addEventListener('pointerup', release);
    stage.addEventListener('pointercancel', release);
    stage.addEventListener('contextmenu', (e) => {
      if (document.body.classList.contains('touch')) e.preventDefault();
    });
    // iOS Safari ignores user-scalable=no; block pinch zoom while playing.
    document.addEventListener('gesturestart', (e) => e.preventDefault());
    window.addEventListener('blur', () => {
      pointers.clear();
      update();
    });
  }

  // Keeps the screen from turning off while the game runs.
  const WakeLock = {
    lock: null,
    async acquire() {
      if (this.lock || !('wakeLock' in navigator) || document.visibilityState !== 'visible') return;
      try {
        this.lock = await navigator.wakeLock.request('screen');
        this.lock.addEventListener('release', () => { this.lock = null; });
      } catch (e) { /* not allowed right now */ }
    },
    release() {
      if (this.lock) this.lock.release().catch(() => {});
      this.lock = null;
    },
  };

  // ------------------------------------------------------------------------
  // Emulation

  async function fetchWasm() {
    const response = await fetch('emerald.wasm');
    if (!response.ok) throw new Error(`Could not download emerald.wasm (${response.status})`);
    const total = Number(response.headers.get('content-length')) || 0;
    if (!response.body || !total) return new Uint8Array(await response.arrayBuffer());

    const reader = response.body.getReader();
    const chunks = [];
    let received = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value);
      received += value.length;
      setOverlay('Downloading game…', `${(received / 1048576).toFixed(1)} MB`, Math.min(received / total, 1));
    }
    const bytes = new Uint8Array(received);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.length;
    }
    return bytes;
  }

  async function createInstance(flash) {
    const instance = await createEmerald({
      wasmBinary,
      print: (text) => console.log(text),
      printErr: (text) => console.warn(text),
    });
    const ptr = instance._Port_GetFlash();
    if (flash) {
      instance.HEAPU8.set(flash.subarray(0, FLASH_SIZE), ptr);
      instance._Port_InitFlash(1);
    } else {
      instance._Port_InitFlash(0);
    }
    instance._Port_SetVolume(1.0);
    if (Audio.ctx) instance._Port_SetAudioRate(Audio.ctx.sampleRate);
    return instance;
  }

  function bindFramebuffer() {
    const ptr = mod._Port_GetFramebuffer();
    frameView = new Uint8ClampedArray(mod.HEAPU8.buffer, ptr, 240 * 160 * 4);
    imageData = new ImageData(frameView, 240, 160);
  }

  function draw() {
    if (frameView.buffer !== mod.HEAPU8.buffer) bindFramebuffer();
    ctx.putImageData(imageData, 0, 0);
  }

  function runFrame(withAudio) {
    const t0 = performance.now();
    mod._Port_SetKeys(keyboardKeys | touchKeys | gamepadKeys);
    mod._Port_Frame();
    stats.frames++;
    stats.frameMs = stats.frameMs * 0.95 + (performance.now() - t0) * 0.05;
    stats.audioBuffered = Audio.buffered;
    if (withAudio && Audio.ctx) {
      const count = mod._Port_GetAudioSampleCount();
      if (count > 0) {
        const ptr = mod._Port_GetAudioBuffer() >> 2;
        Audio.push(mod.HEAPF32.slice(ptr, ptr + count * 2));
      }
    }
    if (mod._Port_TakeFlashDirty()) scheduleSave();
  }

  async function restartGame(flash) {
    running = false;
    setOverlay('Restarting…');
    if (flash === undefined) flash = mod ? currentFlash() : await SaveStore.load();
    mod = await createInstance(flash);
    bindFramebuffer();
    paused = false;
    $('btn-start').hidden = true;
    overlay.hidden = true;
    accumulator = 0;
    lastTime = performance.now();
    running = true;
  }

  function handleCrash(error) {
    if (error === 'PortSoftReset') {
      running = false;
      (saveTimer ? flushSave() : Promise.resolve()).then(() => restartGame());
      return;
    }
    running = false;
    console.error(error);
    setOverlay('The game stopped unexpectedly', String(error && error.message ? error.message : error));
    const btn = $('btn-start');
    btn.textContent = 'Restart';
    btn.hidden = false;
    btn.onclick = () => {
      btn.hidden = true;
      restartGame();
    };
  }

  function tick(now) {
    requestAnimationFrame(tick);
    pollGamepads();
    if (!running || paused) {
      lastTime = now;
      return;
    }

    let dt = now - lastTime;
    lastTime = now;
    if (dt > 250) dt = FRAME_MS; // the tab was in the background

    const fast = fastForwardHeld || fastForwardToggle || gamepadFastForward || touchFastForward;
    const speed = fast ? 4 : 1;
    accumulator += dt * speed;

    let frames = 0;
    const maxFrames = 4 * speed;
    try {
      while (accumulator >= FRAME_MS && frames < maxFrames) {
        runFrame(true);
        accumulator -= FRAME_MS;
        frames++;
      }
    } catch (e) {
      handleCrash(e);
      return;
    }
    if (frames >= maxFrames) accumulator = 0;
    if (frames > 0) draw();
  }

  // ------------------------------------------------------------------------
  // Toolbar

  function setPaused(value) {
    if (!running || paused === value) return;
    paused = value;
    if (paused) {
      WakeLock.release();
      setOverlay('Paused', 'Press P or tap Resume to continue.');
      const btn = $('btn-start');
      btn.textContent = 'Resume';
      btn.hidden = false;
      btn.onclick = () => setPaused(false);
      keyboardKeys = 0;
    } else {
      overlay.hidden = true;
      $('btn-start').hidden = true;
      lastTime = performance.now();
      Audio.start();
      WakeLock.acquire();
    }
  }

  function toggleMute() {
    Audio.muted = !Audio.muted;
    Audio.applyVolume();
    $('btn-mute').textContent = Audio.muted ? 'Sound off' : 'Sound on';
  }

  function toggleFullscreen() {
    // With on-screen controls, the whole page goes fullscreen so they stay.
    const el = document.body.classList.contains('touch') ? document.documentElement : $('screen');
    if (document.fullscreenElement || document.webkitFullscreenElement) {
      (document.exitFullscreen || document.webkitExitFullscreen).call(document);
    } else if (el.requestFullscreen) {
      el.requestFullscreen({ navigationUI: 'hide' }).catch(() => {});
    } else if (el.webkitRequestFullscreen) {
      el.webkitRequestFullscreen();
    }
  }

  function setupToolbar() {
    $('btn-menu').addEventListener('click', (e) => {
      e.stopPropagation();
      setMenuOpen(!$('toolbar').classList.contains('open'));
    });
    document.addEventListener('pointerdown', (e) => {
      if (!e.target.closest('#toolbar, #btn-menu')) setMenuOpen(false);
    });
    $('btn-fullscreen').addEventListener('click', () => setMenuOpen(false));
    // Browsers without the Fullscreen API (iPhone Safari) go fullscreen when
    // the page is added to the home screen instead.
    if (!document.documentElement.requestFullscreen && !document.documentElement.webkitRequestFullscreen)
      $('btn-fullscreen').hidden = true;

    $('btn-mute').addEventListener('click', toggleMute);
    $('volume').addEventListener('input', (e) => {
      Audio.volume = e.target.value / 100;
      Audio.applyVolume();
      try { localStorage.setItem('pokeemerald-volume', e.target.value); } catch (err) { /* ignore */ }
    });
    try {
      const saved = localStorage.getItem('pokeemerald-volume');
      if (saved !== null) {
        $('volume').value = saved;
        Audio.volume = saved / 100;
      }
    } catch (err) { /* ignore */ }

    $('btn-fast').addEventListener('click', () => {
      fastForwardToggle = !fastForwardToggle;
      $('btn-fast').textContent = fastForwardToggle ? 'Speed 4×' : 'Speed 1×';
    });
    $('btn-fullscreen').addEventListener('click', toggleFullscreen);
    $('btn-save-menu').addEventListener('click', () => showPanel('save-menu', $('save-menu').hidden));
    $('btn-help').addEventListener('click', () => showPanel('help', $('help').hidden));
    for (const btn of document.querySelectorAll('.panel .close')) {
      btn.addEventListener('click', () => showPanel(null, false));
    }

    $('btn-export').addEventListener('click', () => {
      if (!mod) return;
      const blob = new Blob([currentFlash()], { type: 'application/octet-stream' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = 'pokeemerald.sav';
      document.body.appendChild(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    });

    $('file-import').addEventListener('change', async (e) => {
      const file = e.target.files[0];
      e.target.value = '';
      if (!file) return;
      const bytes = new Uint8Array(await file.arrayBuffer());
      if (bytes.length < FLASH_SIZE) {
        setStatus(`That file is ${bytes.length} bytes; an Emerald save is 128 KB.`);
        return;
      }
      if (!window.confirm('Replace the current save with this file? The game will restart.')) return;
      const flash = bytes.slice(0, FLASH_SIZE);
      await SaveStore.store(flash);
      showPanel(null, false);
      await restartGame(flash);
      setStatus(`Imported ${file.name}.`);
    });

    $('btn-delete').addEventListener('click', async () => {
      if (!window.confirm('Delete the save file stored in this browser? This cannot be undone.')) return;
      if (saveTimer) {
        clearTimeout(saveTimer);
        saveTimer = null;
      }
      await SaveStore.clear();
      showPanel(null, false);
      await restartGame(null);
      setStatus('Save deleted.');
    });

    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'hidden') {
        if (saveTimer) flushSave();
        setPaused(true);
      } else if (Audio.ctx && Audio.ctx.state !== 'running') {
        Audio.ctx.resume().catch(() => {});
      }
    });
    window.addEventListener('pagehide', () => {
      if (saveTimer) flushSave();
    });
  }

  // ------------------------------------------------------------------------

  async function main() {
    setupToolbar();
    setupTouch();
    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, 240, 160);

    if (typeof WebAssembly !== 'object') {
      setOverlay('This browser does not support WebAssembly.');
      return;
    }

    try {
      setOverlay('Downloading game…', '', 0);
      wasmBinary = await fetchWasm();
      setOverlay('Starting…');
      const flash = await SaveStore.load();
      mod = await createInstance(flash);
      bindFramebuffer();
    } catch (e) {
      console.error(e);
      setOverlay('Could not load the game', String(e && e.message ? e.message : e));
      return;
    }

    setOverlay('Ready', 'Click Start (or press Enter) to begin. Sound needs a click to start in most browsers.');
    const startBtn = $('btn-start');
    startBtn.hidden = false;
    startBtn.focus();
    const begin = async () => {
      startBtn.hidden = true;
      overlay.hidden = true;
      await Audio.start();
      if (Audio.ctx) mod._Port_SetAudioRate(Audio.ctx.sampleRate);
      lastTime = performance.now();
      running = true;
      WakeLock.acquire();
      canvas.focus();
    };
    startBtn.onclick = begin;
    requestAnimationFrame(tick);
  }

  main();
})();
