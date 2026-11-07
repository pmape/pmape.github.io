package com.padmapper.ayar;

import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.Context;

/* Ana ekran aracı: kart durumu + profil ve eşleme düğmeleri */
public class PmWidget extends AppWidgetProvider {
    @Override public void onUpdate(Context c, AppWidgetManager wm, int[] ids) {
        Status.widgets(c, Status.get(c));
    }
}
