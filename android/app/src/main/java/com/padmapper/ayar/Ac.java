package com.padmapper.ayar;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.provider.Browser;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/* Ayar ekranını (uygulamanın içindeki padmapper.html) Chrome'da açar. */
public class Ac extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        try {
            File f = new File(getFilesDir(), "padmapper.html");
            try (InputStream in = getAssets().open("padmapper.html");
                 OutputStream out = new FileOutputStream(f)) {
                byte[] buf = new byte[16384];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
            Uri u = Uri.parse("content://com.padmapper.ayar.files/padmapper.html");
            Intent i = new Intent(Intent.ACTION_VIEW);
            i.setDataAndType(u, "text/html");
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
            /* aynı sekmeyi yeniden kullansın */
            i.putExtra(Browser.EXTRA_APPLICATION_ID, getPackageName());
            i.setPackage("com.android.chrome");
            try {
                startActivity(i);
            } catch (ActivityNotFoundException e) {
                i.setPackage(null);
                startActivity(i);
            }
        } catch (Exception e) {
            Toast.makeText(this, "Ayar ekranı açılamadı: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
        finish();
    }
}
