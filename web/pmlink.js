/* PadMapper: profilleri kartın anladığı biçime çevir + karta bağlan (bilgisayar: WebSerial, Android: WebUSB).
 * Biçim firmware/src/engine.h'de. */
const PM = (() => {
  const HID = {};
  'ABCDEFGHIJKLMNOPQRSTUVWXYZ'.split('').forEach((c, i) => HID['Key' + c] = 4 + i);
  '1234567890'.split('').forEach((c, i) => HID['Digit' + c] = 30 + i);
  for (let i = 1; i <= 12; i++) HID['F' + i] = 57 + i;
  Object.assign(HID, {
    Enter: 40, Escape: 41, Backspace: 42, Tab: 43, Space: 44, Minus: 45, Equal: 46, BracketLeft: 47,
    BracketRight: 48, Backslash: 49, Semicolon: 51, Quote: 52, Backquote: 53, Comma: 54, Period: 55,
    Slash: 56, CapsLock: 57, Insert: 73, Home: 74, PageUp: 75, Delete: 76, End: 77, PageDown: 78,
    ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82, IntlBackslash: 100,
    ControlLeft: 0xe0, ShiftLeft: 0xe1, AltLeft: 0xe2, MetaLeft: 0xe3,
    ControlRight: 0xe4, ShiftRight: 0xe5, AltRight: 0xe6,
  });
  for (let i = 1; i <= 9; i++) HID['Numpad' + i] = 88 + i;
  HID.Numpad0 = 98;

  const BTN = { A: 0, B: 1, X: 3, Y: 4, LB: 6, RB: 7, LT: 8, RT: 9, SELECT: 10, START: 11, HOME: 12,
    L3: 13, R3: 14, SHARE: 15, UP: 16, DOWN: 17, LEFT: 18, RIGHT: 19, M1: 20, M2: 21 };
  const MOUSE = { left: 0, right: 1, middle: 2, back: 3, fwd: 4, wheelUp: 5, wheelDown: 6 };
  const OUT = { kbm: 0, kb: 1, pad: 2, all: 3 };
  const SM = { inherit: 0, off: 1, mouse: 2, keys: 3, scroll: 4, pad: 5 };
  const TRIG = { single: 0, double: 1, long: 2, start: 3, release: 4 };
  const MODE = { normal: 0, turbo: 1, toggle: 2 };
  const ACTS = { press: 0, down: 1, up: 2 };

  function compile(S) {
    const b = [];
    const u8 = v => b.push(v & 255);
    const u16 = v => { v = Math.max(0, Math.min(65535, Math.round(+v || 0))); b.push(v & 255, v >> 8); };
    const d = S.device;
    const profs = S.profiles.map(p => p._applied ? JSON.parse(p._applied) : p).slice(0, 8);
    b.push(80, 77, 1);
    u8(d.startMode === 'pad' ? 1 : 0);
    u8(BTN[d.comboA] ?? 12); u8(BTN[d.comboB] ?? 10);
    u16(d.holdMs || 1500);
    u8((d.profSwitch ? 1 : 0) | (d.passUnmapped !== false ? 2 : 0));
    u8(profs.length); u8(0); u8(0); /* seçili profil ayrıca 'cs' ile gider */
    for (const p of profs) {
      u8(OUT[p.output || 'all']); u8(p.trigThr ?? 30); u16(p.longMs || 300); u16(p.doubleMs || 250);
      for (const l of p.layers) for (const s of ['LS', 'RS']) {
        const st = l.sticks[s] || { mode: 'off' };
        u8(SM[st.mode] ?? 1); u8(st.sens || 8); u8(st.dead ?? 12);
        for (const k of ['up', 'down', 'left', 'right']) u8(HID[st.keys && st.keys[k]] || 0);
      }
      const binds = [];
      p.layers.forEach((l, li) => {
        for (const id in l.btn) {
          if (BTN[id] === undefined) continue;
          for (const k in l.btn[id]) {
            const a = l.btn[id][k];
            if (!a || !(a.kind === 'layer' || (a.steps && a.steps.length))) continue;
            let trig, other = 255;
            if (k.startsWith('c_')) { trig = 5; other = BTN[k.slice(2)]; if (other === undefined) continue; }
            else { trig = TRIG[k]; if (trig === undefined) continue; }
            binds.push({ li, btn: BTN[id], trig, other, a });
          }
        }
      });
      u16(binds.length);
      for (const { li, btn, trig, other, a } of binds) {
        const steps = a.kind === 'layer' ? [] : a.steps.slice(0, 255);
        u8(li); u8(btn); u8(trig); u8(other);
        u8(a.kind === 'layer' ? 1 : 0); u8(MODE[a.mode] || 0); u16(a.turbo || 80);
        u8(a.layer || 0); u8(a.how === 'toggle' ? 1 : 0); u8(steps.length);
        for (const s of steps) {
          if (s.t === 'key') { u8(0); u8(HID[s.k] || 0); }
          else if (s.t === 'mouse') { u8(1); u8(MOUSE[s.b] ?? 0); }
          else { u8(2); u8(BTN[s.p] ?? 0); }
          u8(ACTS[s.a] ?? 0); u16(s.after || 0);
        }
      }
    }
    return new Uint8Array(b);
  }

  const CRC = (() => { const t = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
  function crc32(a) { let c = 0xffffffff; for (const x of a) c = CRC[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; }
  const hex = a => Array.from(a, x => x.toString(16).padStart(2, '0')).join('');

  /* ---------- bağlantı ---------- */
  const link = { kind: null, onLine: () => {}, onClose: () => {}, info: null };
  let buf = '', writer = null, usb = null, closing = false;
  const OLD = 'Kart komutlara cevap vermiyor. Karttaki yazılım eski olabilir: 0.3.0 padmapper.uf2 dosyasını yükle.';
  const enc = new TextEncoder(), dec = new TextDecoder();

  function feed(text) {
    buf += text;
    let i;
    while ((i = buf.indexOf('\n')) >= 0) {
      const l = buf.slice(0, i).trim(); buf = buf.slice(i + 1);
      if (!l.startsWith('{')) continue;
      try { const ev = JSON.parse(l); if (ev.ev === 'hello') link.info = ev; link.onLine(ev); } catch (e) {}
    }
  }

  async function openSerial() {
    const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x2fe3 }] }).catch(() => navigator.serial.requestPort());
    await port.open({ baudRate: 115200 });
    try { await port.setSignals({ dataTerminalReady: true }); } catch (e) {}
    writer = port.writable.getWriter();
    link.kind = 'serial'; link.port = port;
    (async () => {
      const r = port.readable.getReader(); link.reader = r;
      try { for (;;) { const { value, done } = await r.read(); if (done) break; feed(dec.decode(value)); } }
      catch (e) {} finally { r.releaseLock(); if (!closing) link.onClose(); }
    })();
  }

  async function openUsb() {
    const dev = await navigator.usb.requestDevice({ filters: [] }); /* listede "PadMapper"ı seç */
    await dev.open();
    if (!dev.configuration) await dev.selectConfiguration(1);
    let ctrl = null, data = null, epIn = 0, epOut = 0;
    for (const itf of dev.configuration.interfaces) {
      const alt = itf.alternates[0];
      if (alt.interfaceClass === 2 && ctrl === null) ctrl = itf.interfaceNumber;
      if (alt.interfaceClass === 10 && data === null) {
        data = itf.interfaceNumber;
        for (const ep of alt.endpoints) { if (ep.type === 'bulk') { if (ep.direction === 'in') epIn = ep.endpointNumber; else epOut = ep.endpointNumber; } }
      }
    }
    if (data === null) throw new Error('Kartın seri bağlantısı bulunamadı');
    try { await dev.claimInterface(ctrl); } catch (e) {}
    await dev.claimInterface(data);
    const ctl = (req, val, body) => dev.controlTransferOut({ requestType: 'class', recipient: 'interface', request: req, value: val, index: ctrl }, body);
    try { await ctl(0x20, 0, new Uint8Array([0x00, 0xc2, 0x01, 0x00, 0, 0, 8])); } catch (e) {}
    try { await ctl(0x22, 1); } catch (e) {}
    usb = { dev, epIn, epOut };
    link.kind = 'usb';
    (async () => {
      try { for (;;) { const r = await dev.transferIn(epIn, 64); if (r.data && r.data.byteLength) feed(dec.decode(r.data)); } }
      catch (e) {} finally { if (!closing) link.onClose(); }
    })();
  }

  async function connect() {
    closing = false; link.info = null; buf = '';
    if ('serial' in navigator) await openSerial();
    else if ('usb' in navigator) await openUsb();
    else throw new Error('Bu tarayıcı karta bağlanamıyor. Chrome kullan.');
    for (let i = 0; i < 3 && !link.info; i++) {
      await sendLine('hello');
      await new Promise(r => setTimeout(r, 700));
    }
    return !!link.info;
  }

  async function sendLine(l) {
    const d = enc.encode(l + '\n');
    if (link.kind === 'serial') await writer.write(d);
    else if (link.kind === 'usb') for (let i = 0; i < d.length; i += 64) await usb.dev.transferOut(usb.epOut, d.slice(i, i + 64));
    else throw new Error('Kart bağlı değil');
  }

  async function disconnect() {
    closing = true;
    try {
      if (link.kind === 'serial') { try { await link.reader.cancel(); } catch (e) {} writer.releaseLock(); await link.port.close(); }
      else if (link.kind === 'usb') await usb.dev.close();
    } catch (e) {}
    link.kind = null; link.info = null;
  }

  function waitEv(name, ms) {
    return new Promise((res, rej) => {
      const prev = link.onLine;
      const t = setTimeout(() => { link.onLine = prev; rej(new Error('Kart cevap vermedi')); }, ms);
      link.onLine = ev => { prev(ev); if (ev.ev === name) { clearTimeout(t); link.onLine = prev; res(ev); } };
    });
  }

  async function writeConfig(S) {
    if (!link.info) {
      await sendLine('hello');
      await new Promise(r => setTimeout(r, 700));
      if (!link.info) throw new Error(OLD);
    }
    const blob = compile(S);
    const w = waitEv('cfg', 8000);
    await sendLine('cb ' + blob.length);
    for (let i = 0; i < blob.length; i += 200) { await sendLine('cd ' + hex(blob.slice(i, i + 200))); await new Promise(r => setTimeout(r, 15)); }
    await sendLine('ce ' + crc32(blob).toString(16).padStart(8, '0'));
    const r = await w;
    if (!r.ok) throw new Error('Kart ayarı kabul etmedi (' + (r.err || '?') + ')');
    await sendLine('cs ' + Math.min(S.cur, S.profiles.length - 1));
    return { len: blob.length, crc: crc32(blob) };
  }

  return { compile, crc32, hex, link, connect, disconnect, sendLine, writeConfig,
    connected: () => !!link.kind, OLD, supported: () => 'serial' in navigator || 'usb' in navigator };
})();
if (typeof module !== 'undefined') module.exports = PM;
