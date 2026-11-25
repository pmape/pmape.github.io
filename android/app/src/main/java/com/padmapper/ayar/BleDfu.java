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
import android.bluetooth.le.ScanRecord;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.os.Build;
import android.os.ParcelUuid;

import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.UUID;

/* Kablosuz kart güncelleme: XIAO önyükleyicisinin Bluetooth DFU'su (Nordic eski/legacy DFU).
 * Kart "dfu ble" komutuyla önyükleyiciye geçer; önyükleyici DFU servisiyle yayın yapar.
 * Akış: başlat (boyut) → tanıtım paketi → veri (20 baytlık paketler, her 10'da onay) → doğrula → çalıştır. */
@SuppressLint("MissingPermission")
final class BleDfu {
    static final UUID SVC = UUID.fromString("00001530-1212-efde-1523-785feabcd123");
    static final UUID CP = UUID.fromString("00001531-1212-efde-1523-785feabcd123");
    static final UUID PKT = UUID.fromString("00001532-1212-efde-1523-785feabcd123");
    static final UUID CCC = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    private static final int PRN = 10;
    /* Adafruit önyükleyicisi kablosuzda çift bölme kullanır: (0xF4000 - 0x7000 - 0x27000) / 2 */
    static final int MAX_BLE = 0x63000;

    private final Context ctx;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic cp, pkt;
    private final Semaphore opDone = new Semaphore(0);
    private volatile int opStatus;
    private final Semaphore conn = new Semaphore(0);
    private volatile boolean connected, svcOk;
    private final LinkedBlockingQueue<byte[]> notes = new LinkedBlockingQueue<>();
    private volatile BluetoothDevice found;

    BleDfu(Context c) { ctx = c; }

    private static boolean isDfu(ScanResult r) {
        ScanRecord s = r.getScanRecord();
        if (s == null) return false;
        if (s.getServiceUuids() != null && s.getServiceUuids().contains(new ParcelUuid(SVC))) return true;
        String n = s.getDeviceName();
        return n != null && (n.contains("DFU") || n.contains("Dfu"));
    }

    private final java.util.Set<String> seen = java.util.Collections.synchronizedSet(new java.util.HashSet<String>());
    private volatile boolean filtered;
    private final ScanCallback scb = new ScanCallback() {
        @Override public void onScanResult(int t, ScanResult r) {
            seen.add(r.getDevice().getAddress());
            if (found == null && (filtered || isDfu(r))) found = r.getDevice();
        }
    };

    private final BluetoothGattCallback gcb = new BluetoothGattCallback() {
        @Override public void onConnectionStateChange(BluetoothGatt g, int status, int st) {
            connected = st == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS;
            if (connected) {
                try { g.getClass().getMethod("refresh").invoke(g); } catch (Exception e) { }
                g.discoverServices();
            } else { conn.release(); opDone.release(); notes.offer(new byte[0]); }
        }
        @Override public void onServicesDiscovered(BluetoothGatt g, int status) {
            BluetoothGattService s = g.getService(SVC);
            if (s != null) { cp = s.getCharacteristic(CP); pkt = s.getCharacteristic(PKT); }
            svcOk = cp != null && pkt != null;
            conn.release();
        }
        @Override public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) { opStatus = status; opDone.release(); }
        @Override public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) { opStatus = status; opDone.release(); }
        @Override public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            if (CP.equals(c.getUuid())) notes.offer(c.getValue());
        }
    };

    private void op(String what) throws Exception {
        if (!opDone.tryAcquire(10, TimeUnit.SECONDS) || !connected) throw new Exception("Kart cevap vermedi (" + what + ")");
        if (opStatus != BluetoothGatt.GATT_SUCCESS) throw new Exception("Yazılamadı (" + what + ", " + opStatus + ")");
    }

    private void write(BluetoothGattCharacteristic c, byte[] v, boolean resp, String what) throws Exception {
        c.setWriteType(resp ? BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT : BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
        c.setValue(v);
        boolean ok = false;
        for (int t = 0; t < 50 && !(ok = gatt.writeCharacteristic(c)); t++) Thread.sleep(20);
        if (!ok) throw new Exception("Bluetooth meşgul (" + what + ")");
        op(what);
    }

    /* [0x10, istek, sonuç] bekle; sonuç 1 = başarılı */
    private void expect(int req, int ms, String what) throws Exception {
        long end = System.currentTimeMillis() + ms;
        while (true) {
            long w = end - System.currentTimeMillis();
            byte[] n = w > 0 ? notes.poll(w, TimeUnit.MILLISECONDS) : null;
            if (n == null) throw new Exception("Kart cevap vermedi (" + what + ")");
            if (n.length == 0) throw new Exception("Bağlantı koptu (" + what + ")");
            if (n.length >= 3 && n[0] == 0x10 && n[1] == req) {
                if (n[2] == 4) throw new Exception("Yazılım kablosuz yükleme için çok büyük (" + what + "). Bir kere USB kabloyla güncelle.");
                if (n[2] != 1) throw new Exception("Kart reddetti (" + what + ", kod " + n[2] + ")");
                return;
            }
        }
    }

    private static byte[] le32(long v) { return new byte[] { (byte) v, (byte) (v >> 8), (byte) (v >> 16), (byte) (v >> 24) }; }

    void run(byte[] bin, Dfu.Prog prog) throws Exception {
        if (bin.length > MAX_BLE) throw new Exception("Yazılım kablosuz yükleme için çok büyük (" + bin.length / 1024 + " KB). USB kabloyla güncelle.");
        BluetoothManager bm = (BluetoothManager) ctx.getSystemService(Context.BLUETOOTH_SERVICE);
        BluetoothAdapter a = bm == null ? null : bm.getAdapter();
        if (a == null || !a.isEnabled()) throw new Exception("Bluetooth kapalı");
        BluetoothLeScanner sc = a.getBluetoothLeScanner();
        if (sc == null) throw new Exception("Bluetooth kapalı");

        /* önyükleyiciyi bul (en çok 20 sn) */
        prog.on("wait", 0);
        ScanSettings ss = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        /* önce süzgeçli tarama (bazı telefonlar süzgeçsiz taramada sonuç vermez), bulunmazsa süzgeçsiz */
        java.util.List<android.bluetooth.le.ScanFilter> fl = new java.util.ArrayList<>();
        fl.add(new android.bluetooth.le.ScanFilter.Builder().setServiceUuid(new ParcelUuid(SVC)).build());
        fl.add(new android.bluetooth.le.ScanFilter.Builder().setDeviceName("AdaDFU").build());
        for (int pass = 0; pass < 2 && found == null; pass++) {
            filtered = pass == 0;
            sc.startScan(filtered ? fl : null, ss, scb);
            try {
                for (int t = 0; t < 60 && found == null; t++) Thread.sleep(250);
            } finally { try { sc.stopScan(scb); } catch (Exception e) { } }
            Thread.sleep(300);
        }
        if (found == null) { prog.on("noboot", seen.size()); return; }

        try {
            for (int tr = 0; tr < 3 && !svcOk; tr++) {
                if (gatt != null) { try { gatt.close(); } catch (Exception e) { } gatt = null; Thread.sleep(600); }
                gatt = Build.VERSION.SDK_INT >= 23 ? found.connectGatt(ctx, false, gcb, BluetoothDevice.TRANSPORT_LE) : found.connectGatt(ctx, false, gcb);
                conn.tryAcquire(15, TimeUnit.SECONDS);
            }
            if (!svcOk) throw new Exception("Önyükleyiciye bağlanılamadı");
            if (Build.VERSION.SDK_INT >= 21) gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
            Thread.sleep(300);

            /* bildirimleri aç */
            gatt.setCharacteristicNotification(cp, true);
            BluetoothGattDescriptor d = cp.getDescriptor(CCC);
            if (d == null) throw new Exception("Önyükleyicide bildirim yok");
            d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            if (!gatt.writeDescriptor(d)) throw new Exception("Bildirim açılamadı");
            op("bildirim");

            int len = bin.length;
            prog.on("write", 0);
            /* 1) başlat: uygulama görüntüsü, boyutlar (sd, bl, app) */
            write(cp, new byte[] { 0x01, 0x04 }, true, "başlat");
            byte[] sz = new byte[12];
            System.arraycopy(le32(len), 0, sz, 8, 4);
            write(pkt, sz, false, "boyut");
            expect(0x01, 30000, "başlat"); /* önyükleyici bu sırada eski yazılımı siler */

            /* 2) tanıtım paketi: cihaz türü, sürüm, SoftDevice (herhangi), CRC16 */
            write(cp, new byte[] { 0x02, 0x00 }, true, "tanıtım");
            int crc = Dfu.crc16(bin, len);
            byte[] init = { 0x52, 0x00, (byte) 0xFF, (byte) 0xFF, (byte) 0xFF, (byte) 0xFF, (byte) 0xFF, (byte) 0xFF,
                    0x01, 0x00, (byte) 0xFE, (byte) 0xFF, (byte) crc, (byte) (crc >> 8) };
            write(pkt, init, false, "tanıtım");
            write(cp, new byte[] { 0x02, 0x01 }, true, "tanıtım");
            expect(0x02, 10000, "tanıtım");

            /* 3) her 10 pakette bir onay iste, veriyi yolla */
            write(cp, new byte[] { 0x08, (byte) PRN, 0x00 }, true, "onay");
            write(cp, new byte[] { 0x03 }, true, "veri");
            int n = 0, last = -1;
            for (int o = 0; o < len; o += 20) {
                int k = Math.min(20, len - o);
                byte[] p = new byte[k];
                System.arraycopy(bin, o, p, 0, k);
                write(pkt, p, false, "veri");
                if (++n % PRN == 0 && o + k < len) {
                    byte[] r = notes.poll(10, TimeUnit.SECONDS);
                    if (r == null) throw new Exception("Kart cevap vermedi (veri)");
                    if (r.length == 0) throw new Exception("Bağlantı koptu (veri)");
                }
                int pc = (int) Math.min(99, (o + k) * 100L / len);
                if (pc != last) { last = pc; prog.on("write", pc); }
            }
            expect(0x03, 20000, "veri");

            /* 4) doğrula, 5) çalıştır */
            write(cp, new byte[] { 0x04 }, true, "doğrula");
            expect(0x04, 10000, "doğrula");
            try { write(cp, new byte[] { 0x05 }, true, "çalıştır"); } catch (Exception e) { /* kart hemen kapanır */ }
            prog.on("done", 100);
        } finally {
            if (gatt != null) { try { gatt.disconnect(); gatt.close(); } catch (Exception e) { } gatt = null; }
        }
    }
}
