package com.padmapper.ayar;

import java.io.ByteArrayOutputStream;

/* Kablodan kart güncelleme: XIAO önyükleyicisinin seri DFU'su (Nordic eski HCI/SLIP).
 * Sayfadaki (pmlink.js) DFU ile aynı protokol. */
final class Dfu {
    interface Io {
        void write(byte[] b) throws Exception;
        byte[] frame(int ms) throws Exception; /* bir SLIP çerçevesi ya da null */
    }
    interface Prog { void on(String state, int p); }

    static int crc16(byte[] d, int n) {
        int c = 0xFFFF;
        for (int i = 0; i < n; i++) {
            c = ((c >> 8) & 0xFF) | ((c << 8) & 0xFFFF);
            c ^= d[i] & 0xFF;
            c ^= (c & 0xFF) >> 4;
            c ^= (c << 12) & 0xFFFF;
            c ^= ((c & 0xFF) << 5) & 0xFFFF;
        }
        return c;
    }

    /* UF2 -> düz görüntü; out[0] = başlangıç adresi */
    static byte[] uf2ToBin(byte[] u, long[] base) throws Exception {
        long lo = Long.MAX_VALUE, hi = 0;
        for (int o = 0; o + 512 <= u.length; o += 512) {
            if (le32(u, o) != 0x0A324655L || le32(u, o + 4) != 0x9E5D5157L || (le32(u, o + 8) & 1) != 0) continue;
            long a = le32(u, o + 12), n = le32(u, o + 16);
            lo = Math.min(lo, a); hi = Math.max(hi, a + n);
        }
        if (hi == 0) throw new Exception("Dosya geçerli bir UF2 değil");
        byte[] bin = new byte[(int) (hi - lo)];
        java.util.Arrays.fill(bin, (byte) 0xFF);
        for (int o = 0; o + 512 <= u.length; o += 512) {
            if (le32(u, o) != 0x0A324655L || le32(u, o + 4) != 0x9E5D5157L || (le32(u, o + 8) & 1) != 0) continue;
            long a = le32(u, o + 12), n = le32(u, o + 16);
            System.arraycopy(u, o + 32, bin, (int) (a - lo), (int) n);
        }
        base[0] = lo;
        return bin;
    }

    static long le32(byte[] b, int o) {
        return (b[o] & 0xFFL) | (b[o + 1] & 0xFFL) << 8 | (b[o + 2] & 0xFFL) << 16 | (b[o + 3] & 0xFFL) << 24;
    }

    private int seq;
    private final Io io;

    Dfu(Io io) { this.io = io; }

    private byte[] frame(byte[] p) {
        seq = (seq + 1) % 8;
        int ack = (seq + 1) % 8, len = p.length;
        byte[] body = new byte[4 + len + 2];
        body[0] = (byte) (seq | ack << 3 | 1 << 6 | 1 << 7);
        body[1] = (byte) (14 | (len & 15) << 4);
        body[2] = (byte) ((len >> 4) & 255);
        body[3] = (byte) ((-((body[0] & 255) + (body[1] & 255) + (body[2] & 255))) & 255);
        System.arraycopy(p, 0, body, 4, len);
        int c = crc16(body, 4 + len);
        body[4 + len] = (byte) c;
        body[5 + len] = (byte) (c >> 8);
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        o.write(0xC0);
        for (byte x : body) {
            int b = x & 255;
            if (b == 0xC0) { o.write(0xDB); o.write(0xDC); }
            else if (b == 0xDB) { o.write(0xDB); o.write(0xDD); }
            else o.write(b);
        }
        o.write(0xC0);
        return o.toByteArray();
    }

    private void send(byte[] p, String what) throws Exception {
        byte[] f = frame(p);
        int want = (seq + 1) % 8;
        for (int t = 0; t < 4; t++) {
            io.write(f);
            byte[] r = io.frame(t > 0 ? 2000 : 1500);
            if (r != null && r.length > 0 && ((r[0] >> 3) & 7) == want) return;
        }
        throw new Exception("Kart cevap vermedi (" + what + ")");
    }

    private static void put32(byte[] b, int o, long v) { for (int i = 0; i < 4; i++) b[o + i] = (byte) (v >> (8 * i)); }
    private static void put16(byte[] b, int o, int v) { b[o] = (byte) v; b[o + 1] = (byte) (v >> 8); }

    void run(byte[] bin, Prog prog) throws Exception {
        int len = bin.length;
        prog.on("start", 0);
        byte[] st = new byte[20];
        put32(st, 0, 3); put32(st, 4, 4); put32(st, 16, len);
        send(st, "başlat");
        Thread.sleep(Math.max(500, (len / 4096 + 1) * 90L));
        byte[] in = new byte[4 + 14 + 2];
        put32(in, 0, 1); put16(in, 4, 0x0052); put16(in, 6, 0xFFFF); put32(in, 8, 0xFFFFFFFFL);
        put16(in, 12, 1); put16(in, 14, 0xFFFE); put16(in, 16, crc16(bin, len));
        send(in, "tanıtım");
        int n = 0;
        for (int o = 0; o < len; o += 512) {
            int k = Math.min(512, len - o);
            byte[] d = new byte[4 + k];
            put32(d, 0, 4);
            System.arraycopy(bin, o, d, 4, k);
            send(d, "veri");
            if (++n % 8 == 0) Thread.sleep(110);
            prog.on("write", (int) Math.min(99, o * 100L / len));
        }
        Thread.sleep(200);
        byte[] sp = new byte[4];
        put32(sp, 0, 5);
        send(sp, "bitir");
        prog.on("done", 100);
    }

    /* bayt akışından SLIP çerçeveleri */
    static final class Framer {
        private final java.util.ArrayDeque<byte[]> q = new java.util.ArrayDeque<>();
        private ByteArrayOutputStream cur;
        private boolean esc;

        synchronized void push(byte[] b, int n) {
            for (int i = 0; i < n; i++) {
                int x = b[i] & 255;
                if (x == 0xC0) {
                    if (cur != null && cur.size() > 0) { q.add(cur.toByteArray()); cur = null; notifyAll(); }
                    else cur = new ByteArrayOutputStream();
                    continue;
                }
                if (cur == null) continue;
                if (esc) { cur.write(x == 0xDC ? 0xC0 : x == 0xDD ? 0xDB : x); esc = false; }
                else if (x == 0xDB) esc = true;
                else cur.write(x);
            }
        }

        synchronized byte[] next(int ms) throws InterruptedException {
            long end = System.currentTimeMillis() + ms;
            while (q.isEmpty()) {
                long w = end - System.currentTimeMillis();
                if (w <= 0) return null;
                wait(w);
            }
            return q.poll();
        }
    }
}
