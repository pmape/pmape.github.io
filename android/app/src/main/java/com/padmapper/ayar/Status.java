package com.padmapper.ayar;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.widget.RemoteViews;

import org.json.JSONObject;

/*
 * Kartın durumu: bildirim ve ana ekran aracı (widget).
 * Ayar ekranı her değişiklikte durumu buraya yazar (Bridge.status). Düğmeler ActRx'e gider.
 */
final class Status {
    static final String CH = "pm_durum";
    static final int NID = 7;

    static JSONObject get(Context c) {
        try { return new JSONObject(c.getSharedPreferences("pm", Context.MODE_PRIVATE).getString("status", "{}")); }
        catch (Exception e) { return new JSONObject(); }
    }

    static void set(Context c, String json) {
        c.getSharedPreferences("pm", Context.MODE_PRIVATE).edit().putString("status", json).apply();
        refresh(c);
    }

    /* uygulama kapanınca: bağlantı yok */
    static void offline(Context c) {
        try {
            JSONObject o = get(c);
            o.put("conn", false);
            set(c, o.toString());
        } catch (Exception e) { }
    }

    static PendingIntent act(Context c, String cmd, int req) {
        Intent i = new Intent(c, ActRx.class).setAction("com.padmapper.ayar.ACT." + cmd).putExtra("cmd", cmd);
        int fl = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0);
        return PendingIntent.getBroadcast(c, req, i, fl);
    }

    static PendingIntent open(Context c) {
        Intent i = new Intent(c, Main.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        int fl = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0);
        return PendingIntent.getActivity(c, 1, i, fl);
    }

    static String line1(JSONObject o) {
        if (!o.optBoolean("conn")) return "Kart bağlı değil";
        String via = "ble".equals(o.optString("via")) ? "📶 Bluetooth" : "🔌 USB";
        String out = o.optInt("route") == 1 ? " → Bluetooth hedef" : "";
        return "Kart bağlı · " + via + out;
    }

    static String line2(JSONObject o) {
        String p = o.optString("prof", "");
        int pi = o.optInt("pi", 0), np = o.optInt("np", 1);
        String m = o.optInt("mapping", 1) == 1 ? "Eşleme açık" : "🎮 Gamepad";
        return (p.isEmpty() ? "" : "Profil: " + p + (np > 1 ? " (" + (pi + 1) + "/" + np + ")" : "") + " · ") + m;
    }

    static void refresh(Context c) {
        JSONObject o = get(c);
        try { widgets(c, o); } catch (Exception e) { }
        try { notif(c, o); } catch (Exception e) { }
    }

    static void widgets(Context c, JSONObject o) {
        AppWidgetManager wm = AppWidgetManager.getInstance(c);
        int[] ids = wm.getAppWidgetIds(new ComponentName(c, PmWidget.class));
        if (ids == null || ids.length == 0) return;
        boolean on = o.optBoolean("conn");
        RemoteViews v = new RemoteViews(c.getPackageName(), R.layout.pm_widget);
        v.setTextViewText(R.id.w_l1, line1(o));
        v.setTextViewText(R.id.w_l2, on ? line2(o) : "Dokun: uygulamayı aç");
        v.setTextViewText(R.id.w_map, o.optInt("mapping", 1) == 1 ? "Eşleme ✓" : "Gamepad");
        v.setOnClickPendingIntent(R.id.w_head, open(c));
        v.setOnClickPendingIntent(R.id.w_prev, on ? act(c, "prev", 11) : open(c));
        v.setOnClickPendingIntent(R.id.w_next, on ? act(c, "next", 12) : open(c));
        v.setOnClickPendingIntent(R.id.w_map, on ? act(c, "map", 13) : open(c));
        float a = on ? 1f : 0.45f;
        if (Build.VERSION.SDK_INT >= 16) {
            v.setFloat(R.id.w_prev, "setAlpha", a);
            v.setFloat(R.id.w_next, "setAlpha", a);
            v.setFloat(R.id.w_map, "setAlpha", a);
        }
        wm.updateAppWidget(ids, v);
    }

    static void notif(Context c, JSONObject o) {
        NotificationManager nm = (NotificationManager) c.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return;
        if (!o.optBoolean("conn") || !c.getSharedPreferences("pm", Context.MODE_PRIVATE).getBoolean("notif", true)) {
            nm.cancel(NID);
            return;
        }
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= 26) {
            if (nm.getNotificationChannel(CH) == null) {
                NotificationChannel ch = new NotificationChannel(CH, "Kart durumu", NotificationManager.IMPORTANCE_LOW);
                ch.setShowBadge(false);
                nm.createNotificationChannel(ch);
            }
            b = new Notification.Builder(c, CH);
        } else {
            b = new Notification.Builder(c);
        }
        b.setSmallIcon(R.drawable.ic_stat)
         .setContentTitle(line2(o))
         .setContentText(line1(o))
         .setOngoing(true)
         .setOnlyAlertOnce(true)
         .setShowWhen(false)
         .setContentIntent(open(c));
        if (Build.VERSION.SDK_INT >= 23) {
            b.addAction(new Notification.Action.Builder(null, "◀ Profil", act(c, "prev", 21)).build());
            b.addAction(new Notification.Action.Builder(null, o.optInt("mapping", 1) == 1 ? "Gamepad'e geç" : "Eşlemeyi aç", act(c, "map", 23)).build());
            b.addAction(new Notification.Action.Builder(null, "Profil ▶", act(c, "next", 22)).build());
        }
        nm.notify(NID, b.build());
    }
}
