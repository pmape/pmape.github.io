package com.padmapper.ayar;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Collections;
import java.util.UUID;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/*
 * Karta Bluetooth ile bağlanma (kablosuz ayar).
 * Kart, kendi ayar servisini yayınlar. Bağlanınca kart rastgele bir sayı gönderir; uygulama bu
 * telefona ait gizli anahtarla imzalayıp geri yollar. Anahtar karta bir kez USB ile tanıtılır.
 */
@SuppressLint("MissingPermission")
class BleLink {
    interface Cb {
        void state(String s, String msg);   /* scan, conn, auth, ok, fail, closed */
        void feed(String text);
    }

    static final UUID SVC = UUID.fromString("7a1e0001-3c4b-4a8e-9c1d-50ad2840f00d");
    static final UUID RX = UUID.fromString("7a1e0002-3c4b-4a8e-9c1d-50ad2840f00d");
    static final UUID TX = UUID.fromString("7a1e0003-3c4b-4a8e-9c1d-50ad2840f00d");
    static final UUID CCC = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    private final Context ctx;
    private final Cb cb;
    private final byte[] key;
    private final Handler h = new Handler(Looper.getMainLooper());
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic rx;
    private volatile boolean authed, active;
    private int chunk = 20;
    private final ArrayDeque<byte[]> q = new ArrayDeque<>();
    private boolean busy;
    private final ByteArrayOutputStream acc = new ByteArrayOutputStream();

    BleLink(Context c, byte[] key, Cb cb) { this.ctx = c; this.key = key; this.cb = cb; }

    boolean ready() { return authed && gatt != null; }
    boolean active() { return active; }

    /* "ok" başladı, "off" Bluetooth kapalı, "nobt" yok */
    String open() {
        close(false);
        BluetoothManager bm = (BluetoothManager) ctx.getSystemService(Context.BLUETOOTH_SERVICE);
        BluetoothAdapter a = bm == null ? null : bm.getAdapter();
        if (a == null) return "nobt";
        if (!a.isEnabled()) return "off";
        scanner = a.getBluetoothLeScanner();
        if (scanner == null) return "off";
        active = true;
        ScanFilter f = new ScanFilter.Builder().setServiceUuid(new ParcelUuid(SVC)).build();
        ScanSettings s = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        try { scanner.startScan(Collections.singletonList(f), s, scanCb); }
        catch (Exception e) { active = false; return "err:" + e.getMessage(); }
        cb.state("scan", null);
        h.postDelayed(scanTimeout, 15000);
        return "ok";
    }

    private final Runnable scanTimeout = () -> {
        if (gatt == null && active) { stopScan(); active = false; cb.state("fail", "notfound"); }
    };

    private void stopScan() {
        h.removeCallbacks(scanTimeout);
        try { if (scanner != null) scanner.stopScan(scanCb); } catch (Exception e) { }
    }

    private final ScanCallback scanCb = new ScanCallback() {
        @Override public void onScanResult(int type, ScanResult r) {
            if (gatt != null || !active) return;
            stopScan();
            cb.state("conn", null);
            BluetoothDevice d = r.getDevice();
            gatt = Build.VERSION.SDK_INT >= 23 ? d.connectGatt(ctx, false, gcb, BluetoothDevice.TRANSPORT_LE) : d.connectGatt(ctx, false, gcb);
            h.postDelayed(connTimeout, 20000);
        }
        @Override public void onScanFailed(int code) { active = false; cb.state("fail", "scan" + code); }
    };

    private final Runnable connTimeout = () -> { if (!authed && active) { close(false); cb.state("fail", "timeout"); } };

    private final BluetoothGattCallback gcb = new BluetoothGattCallback() {
        @Override public void onConnectionStateChange(BluetoothGatt g, int status, int st) {
            if (st == BluetoothProfile.STATE_CONNECTED) {
                h.post(() -> { if (!g.requestMtu(247)) g.discoverServices(); });
            } else if (st == BluetoothProfile.STATE_DISCONNECTED) {
                boolean was = authed, act = active;
                h.post(() -> {
                    try { g.close(); } catch (Exception e) { }
                    if (gatt == g) { gatt = null; rx = null; authed = false; active = false; synchronized (q) { q.clear(); busy = false; } }
                    h.removeCallbacks(connTimeout);
                    if (was) cb.state("closed", null);
                    else if (act) cb.state("fail", "drop" + status);
                });
            }
        }

        @Override public void onMtuChanged(BluetoothGatt g, int mtu, int status) {
            if (status == BluetoothGatt.GATT_SUCCESS) chunk = Math.max(20, Math.min(240, mtu - 3));
            h.post(g::discoverServices);
        }

        @Override public void onServicesDiscovered(BluetoothGatt g, int status) {
            BluetoothGattService s = g.getService(SVC);
            BluetoothGattCharacteristic tx = s == null ? null : s.getCharacteristic(TX);
            rx = s == null ? null : s.getCharacteristic(RX);
            if (tx == null || rx == null) { h.post(() -> { close(false); cb.state("fail", "nosvc"); }); return; }
            g.setCharacteristicNotification(tx, true);
            BluetoothGattDescriptor d = tx.getDescriptor(CCC);
            if (d == null) { h.post(() -> { close(false); cb.state("fail", "nosvc"); }); return; }
            if (Build.VERSION.SDK_INT >= 33) g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            else { d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE); g.writeDescriptor(d); }
        }

        @Override public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
            cb.state("auth", null);
            send("nonce\n");
        }

        @Override public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            synchronized (q) { busy = false; }
            pump();
        }

        @Override public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] v) { rxData(v); }

        @SuppressWarnings("deprecation")
        @Override public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            if (Build.VERSION.SDK_INT < 33) rxData(c.getValue());
        }
    };

    private void rxData(byte[] v) {
        if (v == null) return;
        String out = null;
        synchronized (acc) {
            acc.write(v, 0, v.length);
            byte[] all = acc.toByteArray();
            int last = -1;
            for (int k = all.length - 1; k >= 0; k--) if (all[k] == '\n') { last = k; break; }
            if (last < 0) return;
            out = new String(all, 0, last + 1, StandardCharsets.UTF_8);
            acc.reset();
            acc.write(all, last + 1, all.length - last - 1);
        }
        if (authed) { cb.feed(out); return; }
        /* doğrulama satırları */
        StringBuilder rest = new StringBuilder();
        for (String l : out.split("\n")) {
            if (authed) { rest.append(l).append('\n'); continue; }
            if (!l.contains("\"ev\":\"auth\"")) continue;
            int i = l.indexOf("\"n\":\"");
            if (i >= 0) {
                String n = l.substring(i + 5, l.indexOf('"', i + 5));
                if (key == null) { h.post(() -> { close(false); cb.state("fail", "nokey"); }); return; }
                send("auth " + hmac(n) + "\n");
            } else if (l.contains("\"ok\":1")) {
                authed = true;
                h.removeCallbacks(connTimeout);
                cb.state("ok", null);
            } else if (l.contains("\"ok\":0")) {
                h.post(() -> { close(false); cb.state("fail", "key"); });
                return;
            }
        }
        if (rest.length() > 0) cb.feed(rest.toString());
    }

    private String hmac(String nonce) {
        try {
            Mac m = Mac.getInstance("HmacSHA256");
            m.init(new SecretKeySpec(key, "HmacSHA256"));
            byte[] r = m.doFinal(nonce.getBytes(StandardCharsets.US_ASCII));
            StringBuilder s = new StringBuilder();
            for (byte b : r) s.append(String.format("%02x", b & 255));
            return s.toString();
        } catch (Exception e) { return ""; }
    }

    boolean send(String s) {
        if (gatt == null || rx == null) return false;
        byte[] b = s.getBytes(StandardCharsets.UTF_8);
        synchronized (q) {
            for (int o = 0; o < b.length; o += chunk) {
                int n = Math.min(chunk, b.length - o);
                byte[] p = new byte[n];
                System.arraycopy(b, o, p, 0, n);
                q.add(p);
            }
        }
        pump();
        return true;
    }

    @SuppressWarnings("deprecation")
    private void pump() {
        byte[] p;
        BluetoothGatt g = gatt;
        synchronized (q) {
            if (busy || q.isEmpty() || g == null || rx == null) return;
            p = q.poll();
            busy = true;
        }
        boolean ok;
        if (Build.VERSION.SDK_INT >= 33) {
            ok = g.writeCharacteristic(rx, p, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == 0;
        } else {
            rx.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            rx.setValue(p);
            ok = g.writeCharacteristic(rx);
        }
        if (!ok) {
            /* yığın meşgul: biraz sonra tekrar dene */
            synchronized (q) { q.addFirst(p); busy = false; }
            h.postDelayed(this::pump, 15);
        }
    }

    void close(boolean notify) {
        boolean was = authed;
        active = false;
        authed = false;
        stopScan();
        h.removeCallbacks(connTimeout);
        synchronized (q) { q.clear(); busy = false; }
        synchronized (acc) { acc.reset(); }
        BluetoothGatt g = gatt;
        gatt = null;
        rx = null;
        if (g != null) { try { g.disconnect(); g.close(); } catch (Exception e) { } }
        if (notify && was) cb.state("closed", null);
    }
}
