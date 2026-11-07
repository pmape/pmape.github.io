package com.padmapper.ayar;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/* Bildirim ve ana ekran aracındaki düğmeler: açık ayar ekranına iletir, kapalıysa uygulamayı açar */
public class ActRx extends BroadcastReceiver {
    @Override public void onReceive(Context c, Intent i) {
        String cmd = i.getStringExtra("cmd");
        if (cmd == null) return;
        Main m = Main.inst;
        if (m != null && m.linked()) { m.action(cmd); return; }
        Intent o = new Intent(c, Main.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP).putExtra("cmd", cmd);
        try { c.startActivity(o); } catch (Exception e) { }
    }
}
