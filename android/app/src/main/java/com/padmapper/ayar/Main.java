package com.padmapper.ayar;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.net.Uri;
import android.os.Bundle;
import android.Manifest;
import android.content.pm.PackageManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/*
 * PadMapper ayar ekranı. Kart takılınca açılır, karta kendisi bağlanır.
 * Ekran (padmapper.html) uygulamanın içinde; karta USB seri (CDC) ile konuşur.
 */
public class Main extends Activity {
    private static final String PERM = "com.padmapper.ayar.USB_PERMISSION";
    private static final int VID = 0x2FE3, PID = 0x0100;

    private WebView web;
    private UsbManager um;
    private UsbDeviceConnection conn;
    private UsbInterface ctrlIf, dataIf;
    private UsbEndpoint epIn, epOut;
    private volatile boolean reading;
    private final Object wlock = new Object();
    private static final int PICK = 7;
    private static final int DRIVE = 8;
    private volatile boolean flashing;
    private BleLink ble;
    static Main inst;
    private String pendingCmd;

    boolean linked() { return (conn != null && reading) || (ble != null && ble.ready()); }

    /* bildirim / widget düğmesi */
    void action(String cmd) { js("window.PM_native&&PM_native.action&&PM_native.action(" + JSONObject.quote(cmd) + ")"); }
    private static final int BLE_PERM = 9;

    /* Bu telefona ait gizli anahtar: karta USB ile bir kez tanıtılır, sonra Bluetooth'tan bağlanırken kullanılır */
    private String secretHex() {
        android.content.SharedPreferences sp = getSharedPreferences("pm", MODE_PRIVATE);
        String s = sp.getString("secret", null);
        if (s == null || s.length() != 64) {
            byte[] r = new byte[32];
            new java.security.SecureRandom().nextBytes(r);
            StringBuilder b = new StringBuilder();
            for (byte x : r) b.append(String.format("%02x", x & 255));
            s = b.toString();
            sp.edit().putString("secret", s).apply();
        }
        return s;
    }

    private static byte[] unhex(String s) {
        byte[] r = new byte[s.length() / 2];
        for (int i = 0; i < r.length; i++) r[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        return r;
    }

    private String[] blePerms() {
        if (Build.VERSION.SDK_INT >= 31) return new String[] { Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT };
        return new String[] { Manifest.permission.ACCESS_FINE_LOCATION };
    }

    private boolean hasBlePerms() {
        if (Build.VERSION.SDK_INT < 23) return true;
        for (String p : blePerms()) if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) return false;
        return true;
    }

    @Override
    public void onRequestPermissionsResult(int req, String[] perms, int[] res) {
        if (req == 10) { Status.refresh(this); return; }
        if (req != BLE_PERM) return;
        boolean ok = res.length > 0;
        for (int r : res) ok &= r == PackageManager.PERMISSION_GRANTED;
        js("window.PM_native&&PM_native.bleperm&&PM_native.bleperm(" + ok + ")");
    }

    private BleLink bleLink() {
        if (ble == null) ble = new BleLink(this, unhex(secretHex()), new BleLink.Cb() {
            @Override public void state(String s, String msg) {
                js("window.PM_native&&PM_native.ble&&PM_native.ble(" + JSONObject.quote(s) + "," + JSONObject.quote(msg == null ? "" : msg) + ")");
                if ("closed".equals(s)) js("window.PM_native&&PM_native.closed()");
            }
            @Override public void feed(String t) { js("window.PM_native&&PM_native.feed(" + JSONObject.quote(t) + ")"); }
        });
        return ble;
    }

    static String mimeOf(String n) {
        if (n == null) return "application/octet-stream";
        if (n.endsWith(".html")) return "text/html";
        if (n.endsWith(".json")) return "application/json";
        if (n.endsWith(".apk")) return "application/vnd.android.package-archive";
        return "application/octet-stream";
    }

    private static void copy(InputStream in, OutputStream out) throws java.io.IOException {
        byte[] b = new byte[16384];
        int n;
        while ((n = in.read(b)) > 0) out.write(b, 0, n);
    }

    private static byte[] readAll(InputStream in) throws java.io.IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        copy(in, o);
        in.close();
        return o.toByteArray();
    }

    /* Paylaşım klasörüne yazılan dosyayı Android paylaşım menüsüyle gönderir */
    private void shareFile(File f, String title) {
        Uri u = Uri.parse("content://" + Share.AUTH + "/" + f.getName());
        Intent i = new Intent(Intent.ACTION_SEND);
        i.setType(mimeOf(f.getName()));
        i.putExtra(Intent.EXTRA_STREAM, u);
        i.putExtra(Intent.EXTRA_SUBJECT, f.getName());
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivity(Intent.createChooser(i, title));
    }

    private File shareDir() {
        File d = new File(getCacheDir(), "share");
        d.mkdirs();
        return d;
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == DRIVE) {
            boolean ok = res == RESULT_OK && data != null && data.getData() != null;
            if (ok) {
                Uri t = data.getData();
                try { getContentResolver().takePersistableUriPermission(t, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION); } catch (Exception e) { }
                getSharedPreferences("pm", MODE_PRIVATE).edit().putString("drive", t.toString()).apply();
            }
            js("window.PM_native&&PM_native.drive&&PM_native.drive(" + ok + ")");
            return;
        }
        if (req != PICK) return;
        if (res != RESULT_OK || data == null || data.getData() == null) { js("window.PM_native&&PM_native.picked(null)"); return; }
        try {
            String s = new String(readAll(getContentResolver().openInputStream(data.getData())), StandardCharsets.UTF_8);
            js("window.PM_native&&PM_native.picked(" + JSONObject.quote(s) + ")");
        } catch (Exception e) {
            js("window.PM_native&&PM_native.picked(null)");
        }
    }

    private final BroadcastReceiver rx = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent i) {
            String a = i.getAction();
            if (PERM.equals(a)) {
                js("window.PM_native&&PM_native.perm(" + i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false) + ")");
            } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(a)) {
                UsbDevice d = i.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                if (d != null && d.getVendorId() == VID) closeLink();
            }
        }
    };

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        inst = this;
        pendingCmd = getIntent().getStringExtra("cmd");
        um = (UsbManager) getSystemService(Context.USB_SERVICE);
        web = new WebView(this);
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(true);
        web.setWebChromeClient(new WebChromeClient());
        web.addJavascriptInterface(new Bridge(), "PMNative");
        setContentView(web);
        web.loadUrl("file:///android_asset/padmapper.html");

        IntentFilter f = new IntentFilter(PERM);
        f.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(rx, f, Context.RECEIVER_EXPORTED);
        else registerReceiver(rx, f);
    }

    @Override
    protected void onNewIntent(Intent i) {
        super.onNewIntent(i);
        String cmd = i.getStringExtra("cmd");
        if (cmd != null) { if (linked()) action(cmd); else pendingCmd = cmd; }
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(i.getAction())) js("window.PM_native&&PM_native.plug()");
    }

    @Override
    public void onBackPressed() {
        if (web.canGoBack()) web.goBack(); else super.onBackPressed();
    }

    @Override
    protected void onDestroy() {
        if (inst == this) inst = null;
        Status.offline(this);
        closeLink();
        if (ble != null) ble.close(false);
        try { unregisterReceiver(rx); } catch (Exception e) { }
        super.onDestroy();
    }

    private void js(final String code) {
        runOnUiThread(() -> web.evaluateJavascript(code, null));
    }

    private UsbDevice find() {
        for (UsbDevice d : um.getDeviceList().values())
            if (d.getVendorId() == VID && d.getProductId() == PID) return d;
        for (UsbDevice d : um.getDeviceList().values())
            if (d.getVendorId() == VID) return d;
        return null;
    }

    private String openDev(UsbDevice d) {
        closeLink();
        UsbDeviceConnection c = um.openDevice(d);
        if (c == null) return "err:open";
        ctrlIf = null; dataIf = null; epIn = null; epOut = null;
        for (int k = 0; k < d.getInterfaceCount(); k++) {
            UsbInterface it = d.getInterface(k);
            if (it.getInterfaceClass() == UsbConstants.USB_CLASS_COMM && ctrlIf == null) ctrlIf = it;
            if (it.getInterfaceClass() == UsbConstants.USB_CLASS_CDC_DATA && dataIf == null) {
                dataIf = it;
                for (int e = 0; e < it.getEndpointCount(); e++) {
                    UsbEndpoint ep = it.getEndpoint(e);
                    if (ep.getType() != UsbConstants.USB_ENDPOINT_XFER_BULK) continue;
                    if (ep.getDirection() == UsbConstants.USB_DIR_IN) epIn = ep; else epOut = ep;
                }
            }
        }
        if (dataIf == null || epIn == null || epOut == null) { c.close(); return "err:nocdc"; }
        if (ctrlIf != null) c.claimInterface(ctrlIf, true);
        if (!c.claimInterface(dataIf, true)) { c.close(); return "err:claim"; }
        int idx = ctrlIf != null ? ctrlIf.getId() : 0;
        byte[] lc = { 0x00, (byte) 0xC2, 0x01, 0x00, 0, 0, 8 }; /* 115200 8N1 */
        c.controlTransfer(0x21, 0x20, 0, idx, lc, lc.length, 500);
        c.controlTransfer(0x21, 0x22, 1, idx, null, 0, 500); /* DTR: kart konuşmaya başlasın */
        conn = c;
        reading = true;
        new Thread(this::readLoop, "pm-usb").start();
        return "ok";
    }

    private void readLoop() {
        byte[] buf = new byte[64];
        ByteArrayOutputStream acc = new ByteArrayOutputStream();
        UsbDeviceConnection c = conn;
        UsbEndpoint in = epIn;
        int fails = 0;
        while (reading && c != null) {
            int n = c.bulkTransfer(in, buf, buf.length, 250);
            if (n > 0) {
                fails = 0;
                acc.write(buf, 0, n);
                byte[] all = acc.toByteArray();
                int last = -1;
                for (int k = all.length - 1; k >= 0; k--) if (all[k] == '\n') { last = k; break; }
                if (last >= 0) {
                    String s = new String(all, 0, last + 1, StandardCharsets.UTF_8);
                    acc.reset();
                    acc.write(all, last + 1, all.length - last - 1);
                    js("window.PM_native&&PM_native.feed(" + JSONObject.quote(s) + ")");
                }
            } else if (n < 0 && find() == null && ++fails > 3) {
                break;
            }
        }
        if (reading) { reading = false; js("window.PM_native&&PM_native.closed()"); }
    }

    private void flashEv(String state, int p, String msg) {
        js("window.PM_native&&PM_native.flash&&PM_native.flash({state:" + JSONObject.quote(state) + ",p:" + p + ",msg:" + JSONObject.quote(msg == null ? "" : msg) + "})");
    }

    private void doFlash() {
        String ds = getSharedPreferences("pm", MODE_PRIVATE).getString("drive", null);
        if (ds == null) { flashEv("nodrive", 0, null); return; }
        byte[] fw;
        try { fw = readAll(getAssets().open("padmapper.uf2")); } catch (Exception e) { flashEv("err", 0, "Uygulamada kart yazılımı yok"); return; }
        Uri tree = Uri.parse(ds);
        Uri dir = android.provider.DocumentsContract.buildDocumentUriUsingTree(tree, android.provider.DocumentsContract.getTreeDocumentId(tree));
        /* sürücü takılıp açılana kadar bekle (en fazla 25 sn) */
        Uri file = null;
        String last = null;
        for (int t = 0; t < 50 && file == null; t++) {
            flashEv("wait", t * 2, null);
            try {
                file = android.provider.DocumentsContract.createDocument(getContentResolver(), dir, "application/octet-stream", "padmapper.uf2");
            } catch (Exception e) { last = e.getMessage(); }
            if (file == null) { try { Thread.sleep(500); } catch (InterruptedException e) { } }
        }
        if (file == null) { flashEv("nodrive", 0, last); return; }
        int done = 0;
        try (OutputStream o = getContentResolver().openOutputStream(file, "w")) {
            for (int off = 0; off < fw.length; off += 4096) {
                int n = Math.min(4096, fw.length - off);
                o.write(fw, off, n);
                done = off + n;
                flashEv("write", (int) (done * 100L / fw.length), null);
            }
            o.flush();
        } catch (Exception e) {
            /* son bloktan sonra kart hemen yeniden başlar; yazma sonunda kopma normal */
            if (done < fw.length * 97 / 100) { flashEv("err", done * 100 / fw.length, e.getMessage()); return; }
        }
        flashEv("done", 100, null);
    }

    private void closeLink() {
        boolean was = reading;
        reading = false;
        synchronized (wlock) {
            if (conn != null) {
                try { if (dataIf != null) conn.releaseInterface(dataIf); } catch (Exception e) { }
                try { if (ctrlIf != null) conn.releaseInterface(ctrlIf); } catch (Exception e) { }
                conn.close();
                conn = null;
            }
        }
        if (was) js("window.PM_native&&PM_native.closed()");
    }

    private class Bridge {
        @JavascriptInterface public boolean present() { return find() != null; }

        /* "ok" bağlandı, "perm" izin soruldu, "none" kart yok, "err:.." hata */
        @JavascriptInterface public String open() {
            UsbDevice d = find();
            if (d == null) return "none";
            if (!um.hasPermission(d)) {
                int fl = Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
                Intent pi = new Intent(PERM).setPackage(getPackageName());
                um.requestPermission(d, PendingIntent.getBroadcast(Main.this, 0, pi, fl));
                return "perm";
            }
            return openDev(d);
        }

        /* Bluetooth ile bağlan: "ok" aranıyor, "perm" izin soruldu, "off" Bluetooth kapalı, "nobt" yok */
        @JavascriptInterface public String bleOpen() {
            if (!hasBlePerms()) {
                if (Build.VERSION.SDK_INT >= 23) runOnUiThread(() -> requestPermissions(blePerms(), BLE_PERM));
                return "perm";
            }
            final String[] r = { "err" };
            final java.util.concurrent.CountDownLatch l = new java.util.concurrent.CountDownLatch(1);
            runOnUiThread(() -> { try { r[0] = bleLink().open(); } finally { l.countDown(); } });
            try { l.await(3, java.util.concurrent.TimeUnit.SECONDS); } catch (InterruptedException e) { }
            return r[0];
        }

        @JavascriptInterface public boolean bleSupported() {
            return getPackageManager().hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE);
        }

        /* şu anki bağlantı: "usb", "ble" ya da "" */
        @JavascriptInterface public String via() {
            if (conn != null && reading) return "usb";
            if (ble != null && ble.ready()) return "ble";
            return "";
        }

        /* kart durumu: bildirim ve ana ekran aracı */
        @JavascriptInterface public void status(String json) { Status.set(Main.this, json); }

        /* bildirim izni (Android 13+), bir kez sorulur */
        @JavascriptInterface public void notifPerm() {
            if (Build.VERSION.SDK_INT < 33) return;
            android.content.SharedPreferences sp = getSharedPreferences("pm", MODE_PRIVATE);
            if (checkSelfPermission("android.permission.POST_NOTIFICATIONS") == PackageManager.PERMISSION_GRANTED || sp.getBoolean("notifAsked", false)) return;
            sp.edit().putBoolean("notifAsked", true).apply();
            runOnUiThread(() -> requestPermissions(new String[] { "android.permission.POST_NOTIFICATIONS" }, 10));
        }

        @JavascriptInterface public void setNotif(boolean on) {
            getSharedPreferences("pm", MODE_PRIVATE).edit().putBoolean("notif", on).apply();
            Status.refresh(Main.this);
        }

        @JavascriptInterface public boolean notifOn() { return getSharedPreferences("pm", MODE_PRIVATE).getBoolean("notif", true); }

        /* uygulama widget/bildirim düğmesiyle açıldıysa bekleyen komut */
        @JavascriptInterface public String takeCmd() { String c = pendingCmd; pendingCmd = null; return c == null ? "" : c; }

        /* karta USB'den tanıtılacak anahtar */
        @JavascriptInterface public String secret() { return secretHex(); }

        @JavascriptInterface public boolean write(String s) {
            if (conn == null && ble != null && ble.ready()) return ble.send(s);
            byte[] b = s.getBytes(StandardCharsets.UTF_8);
            synchronized (wlock) {
                if (conn == null) return false;
                for (int o = 0; o < b.length; o += 64) {
                    int n = Math.min(64, b.length - o);
                    byte[] part = new byte[n];
                    System.arraycopy(b, o, part, 0, n);
                    if (conn.bulkTransfer(epOut, part, n, 1000) < 0) return false;
                }
            }
            return true;
        }

        @JavascriptInterface public void close() {
            closeLink();
            if (ble != null) { final BleLink b = ble; runOnUiThread(() -> b.close(true)); }
        }

        /* Metin dosyası paylaş (profil yedeği vb.) */
        @JavascriptInterface public boolean shareText(String name, String text) {
            try {
                File f = new File(shareDir(), name);
                try (OutputStream o = new FileOutputStream(f)) { o.write(text.getBytes(StandardCharsets.UTF_8)); }
                runOnUiThread(() -> shareFile(f, "Paylaş"));
                return true;
            } catch (Exception e) { return false; }
        }

        /* Ayar ekranını, içine verilen profilleri gömerek paylaş */
        @JavascriptInterface public boolean shareHtml(String name, String presetJson) {
            try {
                String h = new String(readAll(getAssets().open("padmapper.html")), StandardCharsets.UTF_8);
                String inj = "<script>window.PM_PRESET=" + presetJson.replace("</", "<\\/") + ";</script>";
                int at = h.indexOf("<head>");
                h = at >= 0 ? h.substring(0, at + 6) + inj + h.substring(at + 6) : inj + h;
                File f = new File(shareDir(), name);
                try (OutputStream o = new FileOutputStream(f)) { o.write(h.getBytes(StandardCharsets.UTF_8)); }
                runOnUiThread(() -> shareFile(f, "Ayar ekranını paylaş"));
                return true;
            } catch (Exception e) { return false; }
        }

        /* Uygulamanın içindeki dosyayı paylaş: kart yazılımı */
        @JavascriptInterface public boolean shareAsset(String asset, String name) {
            try {
                File f = new File(shareDir(), name);
                try (InputStream in = getAssets().open(asset); OutputStream o = new FileOutputStream(f)) { copy(in, o); }
                runOnUiThread(() -> shareFile(f, "Paylaş"));
                return true;
            } catch (Exception e) { return false; }
        }

        /* Uygulamanın kendisini paylaş (başka telefona kurmak için) */
        @JavascriptInterface public boolean shareApk() {
            try {
                File f = new File(shareDir(), "PadMapper.apk");
                try (InputStream in = new FileInputStream(getApplicationInfo().sourceDir); OutputStream o = new FileOutputStream(f)) { copy(in, o); }
                runOnUiThread(() -> shareFile(f, "Uygulamayı paylaş"));
                return true;
            } catch (Exception e) { return false; }
        }

        @JavascriptInterface public boolean hasAsset(String asset) {
            try { getAssets().open(asset).close(); return true; } catch (Exception e) { return false; }
        }

        @JavascriptInterface public void pickFile() {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            runOnUiThread(() -> startActivityForResult(i, PICK));
        }

        @JavascriptInterface public void openUrl(String url) {
            if (!url.startsWith("https://")) return;
            Intent i = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            runOnUiThread(() -> startActivity(i));
        }

        /* Üst ve alt çubuğu temaya uydur */
        @JavascriptInterface public void setBars(String top, String bottom, boolean light) {
            runOnUiThread(() -> {
                try {
                    android.view.Window w = getWindow();
                    w.setStatusBarColor(android.graphics.Color.parseColor(top));
                    w.setNavigationBarColor(android.graphics.Color.parseColor(bottom));
                    android.view.View d = w.getDecorView();
                    int f = d.getSystemUiVisibility();
                    int bits = android.view.View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR
                            | (Build.VERSION.SDK_INT >= 26 ? android.view.View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR : 0);
                    d.setSystemUiVisibility(light ? (f | bits) : (f & ~bits));
                } catch (Exception e) { }
            });
        }

        @JavascriptInterface public String assetText(String name) {
            try { return new String(readAll(getAssets().open(name)), StandardCharsets.UTF_8).trim(); } catch (Exception e) { return ""; }
        }

        /* Kartın yükleme sürücüsünü seç (bir kere; izin saklanır) */
        @JavascriptInterface public void pickDrive() {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            runOnUiThread(() -> startActivityForResult(i, DRIVE));
        }

        @JavascriptInterface public boolean hasDrive() {
            return getSharedPreferences("pm", MODE_PRIVATE).getString("drive", null) != null;
        }

        /* Gömülü kart yazılımını kartın yükleme sürücüsüne yaz; ilerleme PM_native.flash({...}) ile */
        @JavascriptInterface public void flashUf2() {
            if (flashing) return;
            flashing = true;
            new Thread(() -> {
                try { doFlash(); } finally { flashing = false; }
            }, "pm-flash").start();
        }

        /* Sesli yazma için erişilebilirlik izni açık mı */
        @JavascriptInterface public boolean accessOn() {
            String s = android.provider.Settings.Secure.getString(getContentResolver(), android.provider.Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES);
            return s != null && s.contains(getPackageName() + "/");
        }

        @JavascriptInterface public void openAccess() {
            Intent i = new Intent(android.provider.Settings.ACTION_ACCESSIBILITY_SETTINGS);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            runOnUiThread(() -> startActivity(i));
        }

        @JavascriptInterface public int sdk() { return Build.VERSION.SDK_INT; }

        @JavascriptInterface public String version() {
            try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception e) { return "?"; }
        }
    }
}
