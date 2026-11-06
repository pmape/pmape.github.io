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
import android.os.Bundle;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
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
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(i.getAction())) js("window.PM_native&&PM_native.plug()");
    }

    @Override
    public void onBackPressed() {
        if (web.canGoBack()) web.goBack(); else super.onBackPressed();
    }

    @Override
    protected void onDestroy() {
        closeLink();
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

        @JavascriptInterface public boolean write(String s) {
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

        @JavascriptInterface public void close() { closeLink(); }
    }
}
