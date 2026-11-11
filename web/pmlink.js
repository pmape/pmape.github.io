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
    ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82, IntlBackslash: 100, PrintScreen: 70, ScrollLock: 71, Pause: 72,
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
        const steps = a.kind === 'layer' ? [] : expand(a.steps, d.layout).slice(0, 255);
        u8(li); u8(btn); u8(trig); u8(other);
        u8(a.kind === 'layer' ? 1 : 0); u8(MODE[a.mode] || 0); u16(a.turbo || 80);
        u8(a.layer || 0); u8(a.how === 'toggle' ? 1 : 0); u8(steps.length);
        for (const s of steps) {
          if (s.t === 'key') { u8(0); u8(HID[s.k] || 0); }
          else if (s.t === 'mouse') { u8(1); u8(MOUSE[s.b] ?? 0); }
          else if (s.t === 'media') { u8(3); u8(MEDIA[s.m] ?? 1); }
          else if (s.t === 'move') { u8(4); u8(s.x & 255); u8(s.y & 255); u16(s.after || 0); continue; }
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
          const t = u8(), c = u8(), acr = u8(), ac = ACTSI[acr] || 'press', after = u16();
          if (t === 4) { a.steps.push({ t: 'move', x: (c << 24) >> 24, y: (acr << 24) >> 24, after }); continue; }
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
  /* Bluetooth daha yavaş: bekleme süresi uzun */
  const TO = () => link.via === 'ble' ? 25000 : 8000;
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
    if (info.jlen) { try { return { state: await unpack(await collect('jr', 'jdata', TO())), full: true }; } catch (e) { } }
    if (info.cfg) return { state: decompile(await collect('cr', 'cfgdata', TO())), full: false };
    return null;
  }
  async function writeJson(state) {
    const b = await pack(state);
    if (b.length > 8192) return false;
    const w = waitEv('json', TO());
    await sendLine('jb ' + b.length);
    for (let i = 0; i < b.length; i += 200) { await sendLine('jd ' + hex(b.slice(i, i + 200))); await new Promise(r => setTimeout(r, 15)); }
    await sendLine('je ' + crc32(b).toString(16).padStart(8, '0'));
    const r = await w;
    if (r.ok && link.info) { link.info.jlen = b.length; link.info.jcrc = crc32(b).toString(16).padStart(8, '0'); }
    return !!r.ok;
  }

  /* Yazı → tuş basışları (Türkçe Q ya da US düzeni). [kod, shift, altgr] */
  const TXT_US = {}, TXT_TR = {};
  (() => {
    const set = (m, ch, k, sh = 0, ag = 0) => { if (!(ch in m)) m[ch] = [k, sh, ag]; };
    for (const m of [TXT_US, TXT_TR]) {
      'abcdefghjklmnopqrstuvwxyz'.split('').forEach(c => { set(m, c, 'Key' + c.toUpperCase()); set(m, c.toUpperCase(), 'Key' + c.toUpperCase(), 1); });
      '1234567890'.split('').forEach(c => set(m, c, 'Digit' + c));
      set(m, ' ', 'Space'); set(m, '\n', 'Enter'); set(m, '\t', 'Tab');
    }
    set(TXT_US, 'i', 'KeyI'); set(TXT_US, 'I', 'KeyI', 1);
    const us = { '-': ['Minus'], '=': ['Equal'], '[': ['BracketLeft'], ']': ['BracketRight'], '\\': ['Backslash'], ';': ['Semicolon'], "'": ['Quote'], '`': ['Backquote'], ',': ['Comma'], '.': ['Period'], '/': ['Slash'],
      '_': ['Minus', 1], '+': ['Equal', 1], '{': ['BracketLeft', 1], '}': ['BracketRight', 1], '|': ['Backslash', 1], ':': ['Semicolon', 1], '"': ['Quote', 1], '~': ['Backquote', 1], '<': ['Comma', 1], '>': ['Period', 1], '?': ['Slash', 1],
      '!': ['Digit1', 1], '@': ['Digit2', 1], '#': ['Digit3', 1], '$': ['Digit4', 1], '%': ['Digit5', 1], '^': ['Digit6', 1], '&': ['Digit7', 1], '*': ['Digit8', 1], '(': ['Digit9', 1], ')': ['Digit0', 1] };
    for (const [c, [k, s]] of Object.entries(us)) set(TXT_US, c, k, s || 0);
    const tr = { 'ı': ['KeyI'], 'I': ['KeyI', 1], 'i': ['Quote'], 'İ': ['Quote', 1], 'ğ': ['BracketLeft'], 'Ğ': ['BracketLeft', 1], 'ü': ['BracketRight'], 'Ü': ['BracketRight', 1],
      'ş': ['Semicolon'], 'Ş': ['Semicolon', 1], 'ö': ['Comma'], 'Ö': ['Comma', 1], 'ç': ['Period'], 'Ç': ['Period', 1], '.': ['Slash'], ':': ['Slash', 1], ',': ['Backslash'], ';': ['Backslash', 1],
      '"': ['Backquote'], 'é': ['Backquote', 1], '*': ['Minus'], '?': ['Minus', 1], '-': ['Equal'], '_': ['Equal', 1], '<': ['IntlBackslash'], '>': ['IntlBackslash', 1],
      '!': ['Digit1', 1], "'": ['Digit2', 1], '^': ['Digit3', 1], '+': ['Digit4', 1], '%': ['Digit5', 1], '&': ['Digit6', 1], '/': ['Digit7', 1], '(': ['Digit8', 1], ')': ['Digit9', 1], '=': ['Digit0', 1],
      '@': ['KeyQ', 0, 1], '#': ['Digit3', 0, 1], '$': ['Digit4', 0, 1], '{': ['Digit7', 0, 1], '[': ['Digit8', 0, 1], ']': ['Digit9', 0, 1], '}': ['Digit0', 0, 1], '\\': ['Minus', 0, 1], '|': ['Equal', 0, 1], '~': ['BracketRight', 0, 1] };
    for (const [c, [k, s, g]] of Object.entries(tr)) { TXT_TR[c] = [k, s || 0, g || 0]; }
  })();
  /* Düzenleyicideki kolaylıkları kartın anladığı adımlara aç: yazı, basılı tutma süresi, tekrar sayısı */
  function expand(steps, layout) {
    const out = [], map = layout === 'us' ? TXT_US : TXT_TR;
    for (const s of steps || []) {
      const n = Math.max(1, Math.min(50, (s.rep | 0) || 1));
      for (let r = 0; r < n; r++) {
        const after = r < n - 1 ? (s.repGap ?? s.after ?? 0) : (s.after || 0);
        if (s.t === 'text') {
          const chars = Array.from(s.s || '');
          chars.forEach((ch, i) => {
            const m = map[ch]; if (!m) return;
            const last = i === chars.length - 1, gap = last ? after : Math.max(30, s.gap || 35);
            if (m[1]) out.push({ t: 'key', k: 'ShiftLeft', a: 'down', after: 0 });
            if (m[2]) out.push({ t: 'key', k: 'AltRight', a: 'down', after: 0 });
            out.push({ t: 'key', k: m[0], a: 'press', after: m[1] || m[2] ? 15 : gap });
            if (m[2]) out.push({ t: 'key', k: 'AltRight', a: 'up', after: m[1] ? 0 : gap });
            if (m[1]) out.push({ t: 'key', k: 'ShiftLeft', a: 'up', after: gap });
          });
        } else if (s.t === 'move') {
          /* büyük kaydırmayı 127'lik parçalara böl */
          let x = Math.round(+s.x || 0), y = Math.round(+s.y || 0);
          do {
            const px = Math.max(-127, Math.min(127, x)), py = Math.max(-127, Math.min(127, y));
            x -= px; y -= py;
            out.push({ t: 'move', x: px, y: py, after: x || y ? 0 : after });
          } while (x || y);
        } else if (s.a === 'hold' && !(s.t === 'mouse' && String(s.b).startsWith('wheel'))) {
          out.push(Object.assign({}, s, { a: 'down', after: Math.max(0, +s.hold || 100) }));
          out.push(Object.assign({}, s, { a: 'up', after }));
        } else {
          out.push(Object.assign({}, s, { a: s.a === 'hold' ? 'press' : s.a, after }));
        }
      }
    }
    return out;
  }
  const CRC = (() => { const t = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
  function crc32(a) { let c = 0xffffffff; for (const x of a) c = CRC[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; }
  const hex = a => Array.from(a, x => x.toString(16).padStart(2, '0')).join('');

  /* ---------- bağlantı ---------- */
  const link = { kind: null, via: '', onLine: () => {}, onClose: () => {}, onStatus: () => {}, info: null };
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
    link.kind = 'serial'; link.via = 'usb'; link.port = port;
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
    link.kind = 'usb'; link.via = 'usb';
    (async () => {
      try { for (;;) { const r = await dev.transferIn(epIn, 64); if (r.data && r.data.byteLength) feed(dec.decode(r.data)); } }
      catch (e) {} finally { if (!closing) link.onClose(); }
    })();
  }

  /* Android uygulamasının içinde: karta uygulama bağlanır */
  const native = typeof PMNative !== 'undefined';
  let permWait = null, plugCb = null, blePerm = null, bleWait = null;
  const BLE_ERR = {
    notfound: 'Kart Bluetooth\'ta bulunamadı. Kart açık mı (güç var mı)? Kart USB ile bir uygulamaya bağlıysa Bluetooth ayarı kapalıdır.',
    key: 'Bu cihaz karta henüz tanıtılmamış. Kartı güçten çıkarıp tekrar tak, 1 dakika içinde yeniden Bağlan de: kart bu cihazı kaydeder.',
    nokey: 'Bu cihaz karta henüz tanıtılmamış. Kartı güçten çıkarıp tekrar tak, 1 dakika içinde yeniden Bağlan de.',
    nosvc: 'Karttaki yazılım eski: kablosuz ayar için kartı USB ile tak ve güncelle.',
    timeout: 'Karta Bluetooth ile bağlanılamadı (süre doldu). Tekrar dene.',
  };
  const STAGE = { conn: 'bağlanırken', mtu: 'bağlantı ayarlanırken', svc: 'servisler okunurken', ccc: 'bildirim açılırken', auth: 'doğrulanırken', scan: 'ararken' };
  function bleErr(msg) {
    if (BLE_ERR[msg]) return BLE_ERR[msg];
    const [k, st, code] = String(msg || '').split(':');
    if (k === 'timeout') return 'Kart bulundu ama ' + (STAGE[st] || st) + ' yanıt vermedi. Kartı powerbank\'tan çıkarıp tak, 10 sn bekle, tekrar dene. (' + msg + ')';
    if (k === 'drop') return 'Kart bulundu ama ' + (STAGE[st] || st) + ' bağlantı koptu. Telefonun Bluetooth\'unu kapatıp aç, tekrar dene. (' + msg + ')';
    return 'Bluetooth bağlantısı kurulamadı (' + msg + ')';
  }
  window.PM_native = {
    feed: t => feed(t),
    closed: () => { const was = link.kind === 'native'; link.kind = null; link.via = ''; link.info = null; if (was && !closing) link.onClose(); },
    perm: ok => { if (permWait) { const w = permWait; permWait = null; ok ? w.res() : w.rej(new Error('Karta izin verilmedi')); } },
    plug: () => { if (plugCb) plugCb({ native: true }); },
    bleperm: ok => { if (blePerm) { const w = blePerm; blePerm = null; ok ? w.res() : w.rej(new Error('Bluetooth izni verilmedi. Telefon Ayarlar → Uygulamalar → PadMapper → İzinler → "Yakındaki cihazlar"a izin ver.')); } },
    ble: (st, msg) => {
      link.onStatus(st, msg);
      if (!bleWait) return;
      const w = bleWait;
      if (st === 'ok') { bleWait = null; w.res(); }
      else if (st === 'fail' || st === 'closed') {
        bleWait = null;
        w.rej(new Error(bleErr(msg)));
      }
    },
  };
  const bleOk = () => native && typeof PMNative.bleOpen === 'function' && PMNative.bleSupported();
  async function openBle() {
    let r = PMNative.bleOpen();
    if (r === 'perm') {
      await new Promise((res, rej) => { blePerm = { res, rej }; setTimeout(() => { if (blePerm) { blePerm = null; rej(new Error('Bluetooth izni beklenirken süre doldu. Telefon Ayarlar → Uygulamalar → PadMapper → İzinler → "Yakındaki cihazlar"a izin ver.')); } }, 60000); });
      r = PMNative.bleOpen();
    }
    if (r === 'off') throw new Error('Telefonun Bluetooth\'u kapalı. Açıp tekrar dene.');
    if (r === 'loc') throw new Error('Bu Android sürümünde Bluetooth araması için Konum açık olmalı. Konumu açıp tekrar dene.');
    if (r !== 'ok') throw new Error('Bluetooth kullanılamıyor (' + r + ')');
    await new Promise((res, rej) => { bleWait = { res, rej }; setTimeout(() => { if (bleWait === null) return; bleWait = null; try { PMNative.close(); } catch (e) {} rej(new Error(BLE_ERR.timeout)); }, 50000); });
    link.kind = 'native'; link.via = 'ble';
  }
  async function openNative(known) {
    /* kart USB'de değilse (ya da Bluetooth istendiyse) Bluetooth ile bağlan */
    if ((known && known.ble) || !PMNative.present()) {
      if (bleOk()) return openBle();
      throw new Error('Kart takılı değil');
    }
    let r = PMNative.open();
    if (r === 'perm') {
      await new Promise((res, rej) => { permWait = { res, rej }; setTimeout(() => { if (permWait) { permWait = null; rej(new Error('İzin beklenirken süre doldu')); } }, 60000); });
      r = PMNative.open();
    }
    if (r === 'ok') { link.kind = 'native'; link.via = 'usb'; return; }
    throw new Error(r === 'none' ? 'Kart takılı değil' : 'Karta bağlanılamadı (' + r + ')');
  }


  /* Tarayıcıda Bluetooth (Chrome/Edge): telefonla aynı güvenli ayar kanalı */
  const CFG_SVC = '7a1e0001-3c4b-4a8e-9c1d-50ad2840f00d', CFG_RX = '7a1e0002-3c4b-4a8e-9c1d-50ad2840f00d', CFG_TX = '7a1e0003-3c4b-4a8e-9c1d-50ad2840f00d';
  const webBleOk = () => !native && typeof navigator !== 'undefined' && 'bluetooth' in navigator && !!(window.crypto && crypto.subtle);
  function webKey() {
    let k = null;
    try { k = localStorage.getItem('pm_key'); } catch (e) {}
    if (!k || !/^[0-9a-f]{64}$/.test(k)) {
      const b = new Uint8Array(32); crypto.getRandomValues(b);
      k = [...b].map(x => x.toString(16).padStart(2, '0')).join('');
      try { localStorage.setItem('pm_key', k); } catch (e) {}
    }
    return k;
  }
  async function hmacHex(keyHex, msg) {
    const kb = new Uint8Array(keyHex.match(/../g).map(h => parseInt(h, 16)));
    const ck = await crypto.subtle.importKey('raw', kb, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
    const sig = new Uint8Array(await crypto.subtle.sign('HMAC', ck, enc.encode(msg)));
    return [...sig].map(x => x.toString(16).padStart(2, '0')).join('');
  }
  let wb = null;
  async function openWebBle() {
    const dev = await navigator.bluetooth.requestDevice({ filters: [{ services: [CFG_SVC] }], optionalServices: [CFG_SVC] });
    const server = await dev.gatt.connect();
    const svc = await server.getPrimaryService(CFG_SVC);
    const rx = await svc.getCharacteristic(CFG_RX), tx = await svc.getCharacteristic(CFG_TX);
    const key = webKey();
    wb = { dev, rx, q: Promise.resolve(), authed: false, acc: '' };
    let done, fail;
    const authP = new Promise((res, rej) => { done = res; fail = rej; });
    let pairTried = false;
    tx.addEventListener('characteristicvaluechanged', async e => {
      const t = dec.decode(e.target.value);
      if (wb.authed) { feed(t); return; }
      wb.acc += t;
      let i;
      while ((i = wb.acc.indexOf('\n')) >= 0) {
        const l = wb.acc.slice(0, i); wb.acc = wb.acc.slice(i + 1);
        if (wb.authed) { feed(l + '\n'); continue; }
        let ev; try { ev = JSON.parse(l); } catch (x) { continue; }
        if (ev.ev !== 'auth') continue;
        if (ev.paired !== undefined) { wbWrite('nonce\n'); continue; }
        if (ev.n) {
          if (ev.open && !pairTried) { pairTried = true; wbWrite('pair ' + key + '\n'); continue; }
          wbWrite('auth ' + await hmacHex(key, ev.n) + '\n');
        } else if (ev.ok === 1) { wb.authed = true; done(); }
        else if (ev.ok === 0) fail(new Error(BLE_ERR.key));
      }
    });
    await tx.startNotifications();
    dev.addEventListener('gattserverdisconnected', () => {
      const was = link.kind === 'wble'; fail(new Error(BLE_ERR.timeout));
      if (was) { link.kind = null; link.via = ''; link.info = null; wb = null; if (!closing) link.onClose(); }
    });
    link.kind = 'wble'; link.via = 'ble';
    await wbWrite('nonce\n');
    await Promise.race([authP, new Promise((_, rej) => setTimeout(() => rej(new Error(BLE_ERR.timeout)), 15000))])
      .catch(async e => { try { dev.gatt.disconnect(); } catch (x) {} link.kind = null; link.via = ''; throw e; });
  }
  function wbWrite(str) {
    const w = wb; if (!w) return Promise.reject(new Error('Kart bağlı değil'));
    const d = enc.encode(str);
    w.q = w.q.then(async () => {
      for (let i = 0; i < d.length; i += 120) {
        const part = d.slice(i, i + 120);
        if (w.rx.writeValueWithResponse) await w.rx.writeValueWithResponse(part); else await w.rx.writeValue(part);
      }
    });
    return w.q;
  }

  /* ---------- Kablodan kart güncelleme: XIAO önyükleyicisinin seri DFU'su (Nordic eski HCI/SLIP) ---------- */
  const DFU = (() => {
    function crc16(d, c = 0xFFFF) {
      for (let i = 0; i < d.length; i++) {
        c = ((c >> 8) & 0xFF) | ((c << 8) & 0xFFFF); c ^= d[i]; c ^= (c & 0xFF) >> 4;
        c ^= (c << 12) & 0xFFFF; c ^= ((c & 0xFF) << 5) & 0xFFFF;
      }
      return c;
    }
    const u32 = v => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255];
    const u16 = v => [v & 255, (v >>> 8) & 255];
    /* UF2 -> düz görüntü (başlangıç adresi ile) */
    function uf2ToBin(u) {
      let lo = Infinity, hi = 0;
      const dv = new DataView(u.buffer, u.byteOffset, u.byteLength);
      const blocks = [];
      for (let o = 0; o + 512 <= u.length; o += 512) {
        if (dv.getUint32(o, true) !== 0x0A324655 || dv.getUint32(o + 4, true) !== 0x9E5D5157) continue;
        const flags = dv.getUint32(o + 8, true);
        if (flags & 1) continue; /* ana flaşa değil */
        const addr = dv.getUint32(o + 12, true), n = dv.getUint32(o + 16, true);
        blocks.push([addr, u.subarray(o + 32, o + 32 + n)]);
        lo = Math.min(lo, addr); hi = Math.max(hi, addr + n);
      }
      if (!blocks.length) throw new Error('Dosya geçerli bir UF2 değil');
      const bin = new Uint8Array(hi - lo).fill(0xFF);
      for (const [a, d] of blocks) bin.set(d, a - lo);
      return { base: lo, bin };
    }
    /* io: write(Uint8Array) -> Promise, frame() -> Promise<Uint8Array|null> (bir SLIP çerçevesi ya da zaman aşımı) */
    async function run(bin, io, prog) {
      let seq = 0;
      const sleep = ms => new Promise(r => setTimeout(r, ms));
      function frame(payload) {
        seq = (seq + 1) % 8;
        const ack = (seq + 1) % 8, len = payload.length;
        const h = [seq | (ack << 3) | (1 << 6) | (1 << 7), 14 | ((len & 15) << 4), (len >> 4) & 255];
        h.push((~(h[0] + h[1] + h[2]) + 1) & 255);
        const body = h.concat(Array.from(payload));
        const c = crc16(body);
        body.push(c & 255, c >> 8);
        const out = [0xC0];
        for (const b of body) {
          if (b === 0xC0) out.push(0xDB, 0xDC); else if (b === 0xDB) out.push(0xDB, 0xDD); else out.push(b);
        }
        out.push(0xC0);
        return new Uint8Array(out);
      }
      async function send(payload, what) {
        const f = frame(payload), want = (seq + 1) % 8;
        for (let t = 0; t < 4; t++) {
          await io.write(f);
          const r = await io.frame(t ? 2000 : 1500);
          if (r && r.length && ((r[0] >> 3) & 7) === want) return;
        }
        throw new Error('Kart cevap vermedi (' + what + ')');
      }
      const len = bin.length;
      prog('start', 0);
      await send([...u32(3), ...u32(4), ...u32(0), ...u32(0), ...u32(len)], 'başlat');
      /* önyükleyici uygulama alanını siliyor */
      await sleep(Math.max(500, (Math.floor(len / 4096) + 1) * 90));
      const init = [...u16(0x0052), ...u16(0xFFFF), ...u32(0xFFFFFFFF), ...u16(1), ...u16(0xFFFE), ...u16(crc16(bin))];
      await send([...u32(1), ...init, 0, 0], 'tanıtım');
      let n = 0;
      for (let o = 0; o < len; o += 512) {
        await send([...u32(4), ...bin.subarray(o, Math.min(len, o + 512))], 'veri');
        if (++n % 8 === 0) await sleep(110); /* bir sayfa (4 KB) yazılsın */
        prog('write', Math.min(99, Math.round(o * 100 / len)));
      }
      await sleep(200);
      await send(u32(5), 'bitir');
      prog('done', 100);
    }
    /* bayt akışından SLIP çerçeveleri */
    function framer() {
      let cur = null, esc = false;
      const q = [], waiters = [];
      return {
        push(bytes) {
          for (const b of bytes) {
            if (b === 0xC0) {
              if (cur && cur.length) { const f = new Uint8Array(cur); if (waiters.length) waiters.shift()(f); else q.push(f); cur = null; }
              else cur = [];
              continue;
            }
            if (!cur) continue;
            if (esc) { cur.push(b === 0xDC ? 0xC0 : b === 0xDD ? 0xDB : b); esc = false; } else if (b === 0xDB) esc = true; else cur.push(b);
          }
        },
        next(ms) {
          if (q.length) return Promise.resolve(q.shift());
          return new Promise(res => {
            const w = f => { clearTimeout(t); res(f); };
            const t = setTimeout(() => { const i = waiters.indexOf(w); if (i >= 0) waiters.splice(i, 1); res(null); }, ms);
            waiters.push(w);
          });
        }
      };
    }
    return { crc16, uf2ToBin, run, framer };
  })();

  const isCard = d => d && (d.vendorId === 0x2fe3 || d.productName === 'PadMapper');
  const android = /Android/i.test(navigator.userAgent);

  /* Daha önce izin verilmiş kart takılıysa sormadan bağlan */
  async function findKnown() {
    if (native) return PMNative.present() ? { native: true } : bleOk() && localStorage.getItem('pm_ble') === '1' ? { ble: true, auto: true } : null;
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
    if (native) await openNative(known);
    else if (known && known.usb) await openUsb(known.usb);
    else if (known && known.serial) await openSerial(known.serial);
    else if (known && known.wble) await openWebBle();
    else
    /* Android Chrome'daki seri bağlantı yalnızca Bluetooth'u görür; orada USB yolunu kullan */
    if (android && 'usb' in navigator) await openUsb();
    else if ('serial' in navigator) await openSerial();
    else if ('usb' in navigator) await openUsb();
    else throw new Error('Bu tarayıcı karta bağlanamıyor. Chrome kullan.');
    for (let i = 0; i < 3 && !link.info; i++) {
      await sendLine('hello');
      await new Promise(r => setTimeout(r, link.via === 'ble' ? 1200 : 700));
    }
    /* USB'den bağlıyken bu cihazı karta tanıt: sonra kablosuz da bağlanabilsin */
    if (link.info && link.via === 'usb') {
      try { await sendLine('secret add ' + (native ? PMNative.secret() : webKey())); } catch (e) {}
    }
    return !!link.info;
  }

  async function sendLine(l) {
    const d = enc.encode(l + '\n');
    if (link.kind === 'native') { if (!PMNative.write(l + '\n')) throw new Error('Karta yazılamadı'); if (link.via === 'ble') await new Promise(r => setTimeout(r, 4)); }
    else if (link.kind === 'serial') await writer.write(d);
    else if (link.kind === 'usb') for (let i = 0; i < d.length; i += 64) await usb.dev.transferOut(usb.epOut, d.slice(i, i + 64));
    else if (link.kind === 'wble') await wbWrite(l + '\n');
    else throw new Error('Kart bağlı değil');
  }

  async function disconnect() {
    closing = true;
    if (bleWait) { const w = bleWait; bleWait = null; w.rej(new Error('Vazgeçildi')); try { PMNative.close(); } catch (e) {} }
    try {
      if (link.kind === 'native') PMNative.close();
      else if (link.kind === 'serial') { try { await link.reader.cancel(); } catch (e) {} writer.releaseLock(); await link.port.close(); }
      else if (link.kind === 'usb') await usb.dev.close();
      else if (link.kind === 'wble' && wb) { const d = wb.dev; wb = null; d.gatt.disconnect(); }
    } catch (e) {}
    link.kind = null; link.via = ''; link.info = null;
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
    const w = waitEv('cfg', TO());
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

  return { HID, expandCount: (st, lay) => expand(st, lay).length, decompile, readCard, writeJson, findKnown, onPlug, compile, crc32, hex, link, connect, disconnect, sendLine, writeConfig,
    connected: () => !!link.kind, OLD, bleOk, webBleOk, DFU, webKey, supported: () => native || 'serial' in navigator || 'usb' in navigator, native };
})();
if (typeof module !== 'undefined') module.exports = PM;
