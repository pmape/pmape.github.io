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
  const MEDIA = { voice: 1, search: 2, volUp: 3, volDown: 4, mute: 5, play: 6, next: 7, prev: 8, brUp: 9, brDown: 10, home: 11, back: 12 };
  const OUT = { kbm: 0, kb: 1, pad: 2, all: 3 };
  const SM = { inherit: 0, off: 1, mouse: 2, keys: 3, scroll: 4, pad: 5 };
  const TRIG = { single: 0, double: 1, long: 2, start: 3, release: 4 };
  const MODE = { normal: 0, turbo: 1, toggle: 2, macro: 3 };
  const ACTS = { press: 0, down: 1, up: 2 };

  function compile(S) {
    const b = [];
    const u8 = v => b.push(v & 255);
    const u16 = v => { v = Math.max(0, Math.min(65535, Math.round(+v || 0))); b.push(v & 255, v >> 8); };
    const d = S.device;
    const profs = S.profiles.map(p => p._applied ? JSON.parse(p._applied) : p).slice(0, 8);
    b.push(80, 77, 2);
    u8(d.startMode === 'pad' ? 1 : 0);
    u8(BTN[d.comboA] ?? 12); u8(BTN[d.comboB] ?? 10);
    u16(d.holdMs || 1500);
    u8((d.profSwitch ? 1 : 0) | (d.passUnmapped !== false ? 2 : 0));
    u8(profs.length); u8(0); /* seçili profil ayrıca 'cs' ile gider */
    u8(BTN[d.profMod || 'HOME'] ?? 12); u8(BTN[d.profPrev || 'LB'] ?? 6); u8(BTN[d.profNext || 'RB'] ?? 7); u8(0); u8(0);
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
          else if (s.t === 'media') { u8(3); u8(MEDIA[s.m] ?? 1); }
          else { u8(2); u8(BTN[s.p] ?? 0); }
          u8(ACTS[s.a] ?? 0); u16(s.after || 0);
        }
      }
    }
    return new Uint8Array(b);
  }

  /* Karttaki ayardan profilleri geri çıkar (profil adları kartta yok: Profil 1, 2…) */
  const inv = o => Object.fromEntries(Object.entries(o).map(([k, v]) => [v, k]));
  function decompile(b) {
    const HIDI = inv(HID), BTNI = inv(BTN), MOUSEI = inv(MOUSE), MEDIAI = inv(MEDIA), OUTI = inv(OUT), SMI = inv(SM), TRIGI = inv(TRIG), MODEI = inv(MODE), ACTSI = inv(ACTS);
    let i = 0;
    const u8 = () => b[i++], u16 = () => { const v = b[i] | (b[i + 1] << 8); i += 2; return v; };
    if (b[0] !== 80 || b[1] !== 77 || b[2] !== 2) throw new Error('Karttaki ayar okunamadı');
    i = 3;
    const d = { startMode: u8() ? 'pad' : 'map', comboA: BTNI[u8()] || 'HOME', comboB: BTNI[u8()] || 'SELECT', holdMs: u16() };
    const fl = u8(); d.profSwitch = !!(fl & 1); d.passUnmapped = !!(fl & 2);
    const np = u8(); u8();
    d.profMod = BTNI[u8()] || 'HOME'; d.profPrev = BTNI[u8()] || 'LB'; d.profNext = BTNI[u8()] || 'RB'; u8(); u8();
    const profiles = [];
    for (let n = 0; n < np; n++) {
      const p = { name: 'Profil ' + (n + 1), output: OUTI[u8()] || 'all', trigThr: u8(), longMs: u16(), doubleMs: u16(), gapMs: 50, layers: [] };
      for (let l = 0; l < 4; l++) {
        const sticks = {};
        for (const s of ['LS', 'RS']) {
          const mode = SMI[u8()] || 'off', sens = u8(), dead = u8(), k = [u8(), u8(), u8(), u8()].map(x => HIDI[x] || '');
          sticks[s] = { mode, sens, dead, keys: { up: k[0] || 'KeyW', down: k[1] || 'KeyS', left: k[2] || 'KeyA', right: k[3] || 'KeyD' } };
        }
        p.layers.push({ btn: {}, sticks });
      }
      const nb = u16();
      for (let k = 0; k < nb; k++) {
        const li = u8(), btn = BTNI[u8()], trig = u8(), other = u8(), kind = u8(), mode = u8(), loop = u16(), tl = u8(), how = u8(), ns = u8();
        const a = { kind: kind ? 'layer' : 'combo', mode: MODEI[mode] || 'normal', turbo: loop, layer: tl || 1, how: how ? 'toggle' : 'hold', steps: [] };
        for (let s = 0; s < ns; s++) {
          const t = u8(), c = u8(), ac = ACTSI[u8()] || 'press', after = u16();
          a.steps.push(t === 0 ? { t: 'key', k: HIDI[c] || '', a: ac, after } : t === 1 ? { t: 'mouse', b: MOUSEI[c] || 'left', a: ac, after } : t === 3 ? { t: 'media', m: MEDIAI[c] || 'voice', a: ac, after } : { t: 'pad', p: BTNI[c] || 'A', a: ac, after });
        }
        if (!btn) continue;
        const key = trig === 5 ? 'c_' + (BTNI[other] || '') : TRIGI[trig];
        if (!key || key === 'c_') continue;
        (p.layers[li].btn[btn] = p.layers[li].btn[btn] || {})[key] = a;
      }
      profiles.push(p);
    }
    return { cur: 0, profiles, device: d };
  }

  /* Profillerin düzenlenebilir hali: sıkıştırıp karta da yazılır */
  async function pack(obj) {
    const raw = new TextEncoder().encode(JSON.stringify(obj));
    if (typeof CompressionStream === 'undefined') return Uint8Array.from([74, ...raw]); /* 'J' */
    const z = new Uint8Array(await new Response(new Blob([raw]).stream().pipeThrough(new CompressionStream('gzip'))).arrayBuffer());
    const out = new Uint8Array(z.length + 1); out[0] = 71; out.set(z, 1); return out; /* 'G' */
  }
  async function unpack(b) {
    if (b[0] === 74) return JSON.parse(new TextDecoder().decode(b.slice(1)));
    const r = await new Response(new Blob([b.slice(1)]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer();
    return JSON.parse(new TextDecoder().decode(r));
  }
  const unhex = s => Uint8Array.from((s.match(/../g) || []).map(x => parseInt(x, 16)));

  function collect(cmd, ev, ms) {
    return new Promise((res, rej) => {
      const prev = link.onLine; let acc = '';
      const t = setTimeout(() => { link.onLine = prev; rej(new Error('Kart cevap vermedi')); }, ms);
      link.onLine = e => { prev(e); if (e.ev === ev) acc += e.d; if (e.ev === ev + '_end') { clearTimeout(t); link.onLine = prev; res(unhex(acc)); } };
      sendLine(cmd).catch(rej);
    });
  }
  async function readCard() {
    const info = link.info || {};
    if (info.jlen) { try { return { state: await unpack(await collect('jr', 'jdata', 8000)), full: true }; } catch (e) { } }
    if (info.cfg) return { state: decompile(await collect('cr', 'cfgdata', 8000)), full: false };
    return null;
  }
  async function writeJson(state) {
    const b = await pack(state);
    if (b.length > 8192) return false;
    const w = waitEv('json', 8000);
    await sendLine('jb ' + b.length);
    for (let i = 0; i < b.length; i += 200) { await sendLine('jd ' + hex(b.slice(i, i + 200))); await new Promise(r => setTimeout(r, 15)); }
    await sendLine('je ' + crc32(b).toString(16).padStart(8, '0'));
    const r = await w;
    if (r.ok && link.info) { link.info.jlen = b.length; link.info.jcrc = crc32(b).toString(16).padStart(8, '0'); }
    return !!r.ok;
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

  async function openSerial(port) {
    if (!port) port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x2fe3 }] }).catch(() => navigator.serial.requestPort());
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

  async function openUsb(dev) {
    if (!dev) dev = await navigator.usb.requestDevice({ filters: [] }); /* listede "PadMapper"ı seç */
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

  /* Android uygulamasının içinde: karta uygulama bağlanır */
  const native = typeof PMNative !== 'undefined';
  let permWait = null, plugCb = null;
  window.PM_native = {
    feed: t => feed(t),
    closed: () => { const was = link.kind === 'native'; link.kind = null; link.info = null; if (was && !closing) link.onClose(); },
    perm: ok => { if (permWait) { const w = permWait; permWait = null; ok ? w.res() : w.rej(new Error('Karta izin verilmedi')); } },
    plug: () => { if (plugCb) plugCb({ native: true }); },
  };
  async function openNative() {
    let r = PMNative.open();
    if (r === 'perm') {
      await new Promise((res, rej) => { permWait = { res, rej }; setTimeout(() => { if (permWait) { permWait = null; rej(new Error('İzin beklenirken süre doldu')); } }, 60000); });
      r = PMNative.open();
    }
    if (r === 'ok') { link.kind = 'native'; return; }
    throw new Error(r === 'none' ? 'Kart takılı değil' : 'Karta bağlanılamadı (' + r + ')');
  }

  const isCard = d => d && (d.vendorId === 0x2fe3 || d.productName === 'PadMapper');
  const android = /Android/i.test(navigator.userAgent);

  /* Daha önce izin verilmiş kart takılıysa sormadan bağlan */
  async function findKnown() {
    if (native) return PMNative.present() ? { native: true } : null;
    if (android && 'usb' in navigator) {
      const d = (await navigator.usb.getDevices()).find(isCard);
      return d ? { usb: d } : null;
    }
    if ('serial' in navigator) {
      const p = (await navigator.serial.getPorts()).find(x => (x.getInfo().usbVendorId) === 0x2fe3);
      return p ? { serial: p } : null;
    }
    return null;
  }

  async function connect(known) {
    closing = false; link.info = null; buf = '';
    if (native) await openNative();
    else if (known && known.usb) await openUsb(known.usb);
    else if (known && known.serial) await openSerial(known.serial);
    else
    /* Android Chrome'daki seri bağlantı yalnızca Bluetooth'u görür; orada USB yolunu kullan */
    if (android && 'usb' in navigator) await openUsb();
    else if ('serial' in navigator) await openSerial();
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
    if (link.kind === 'native') { if (!PMNative.write(l + '\n')) throw new Error('Karta yazılamadı'); }
    else if (link.kind === 'serial') await writer.write(d);
    else if (link.kind === 'usb') for (let i = 0; i < d.length; i += 64) await usb.dev.transferOut(usb.epOut, d.slice(i, i + 64));
    else throw new Error('Kart bağlı değil');
  }

  async function disconnect() {
    closing = true;
    try {
      if (link.kind === 'native') PMNative.close();
      else if (link.kind === 'serial') { try { await link.reader.cancel(); } catch (e) {} writer.releaseLock(); await link.port.close(); }
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

  /* takılınca haber ver */
  function onPlug(cb) {
    if (native) { plugCb = cb; return; }
    if (android && 'usb' in navigator) navigator.usb.addEventListener('connect', e => { if (isCard(e.device)) cb({ usb: e.device }); });
    else if ('serial' in navigator) navigator.serial.addEventListener('connect', e => { const p = e.target; if (p.getInfo && p.getInfo().usbVendorId === 0x2fe3) cb({ serial: p }); });
  }

  return { decompile, readCard, writeJson, findKnown, onPlug, compile, crc32, hex, link, connect, disconnect, sendLine, writeConfig,
    connected: () => !!link.kind, OLD, supported: () => native || 'serial' in navigator || 'usb' in navigator, native };
})();
if (typeof module !== 'undefined') module.exports = PM;
