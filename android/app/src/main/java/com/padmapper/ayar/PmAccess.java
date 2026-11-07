package com.padmapper.ayar;

import android.accessibilityservice.AccessibilityService;
import android.os.Build;
import android.provider.Settings;
import android.view.KeyEvent;
import android.view.accessibility.AccessibilityEvent;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Toast;

/*
 * Kart "Pause" tuşu gönderince klavyeyi Google Sesle Yazma'ya çevirir; tekrar gelince önceki klavyeye döner.
 * Sadece bu tek tuşa bakar; diğer tuşlar olduğu gibi geçer.
 */
public class PmAccess extends AccessibilityService {
    private String previous;

    @Override
    protected boolean onKeyEvent(KeyEvent e) {
        if (e.getKeyCode() != KeyEvent.KEYCODE_BREAK) return false;
        if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) toggleVoice();
        return true; /* bu tuş başka yere gitmesin */
    }

    private String voiceIme() {
        InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        String best = null;
        for (InputMethodInfo i : imm.getEnabledInputMethodList()) {
            String id = i.getId();
            String n = id.toLowerCase();
            if (n.contains("voicesearch") || n.contains("voiceinput") || n.contains("voice.ime")) return id;
            if (n.contains("voice") && best == null) best = id;
        }
        return best;
    }

    private void toggleVoice() {
        if (Build.VERSION.SDK_INT < 33) {
            Toast.makeText(this, "Sesli yazma geçişi için Android 13 veya üstü gerekli", Toast.LENGTH_LONG).show();
            return;
        }
        String cur = Settings.Secure.getString(getContentResolver(), Settings.Secure.DEFAULT_INPUT_METHOD);
        String voice = voiceIme();
        if (voice == null) {
            Toast.makeText(this, "Google Sesle Yazma klavyesi açık değil: Ayarlar → Diller ve giriş → Ekran klavyesi'nden etkinleştir", Toast.LENGTH_LONG).show();
            return;
        }
        String target;
        if (voice.equals(cur)) {
            target = previous != null ? previous : null;
            if (target == null) {
                InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
                for (InputMethodInfo i : imm.getEnabledInputMethodList()) if (!i.getId().equals(voice)) { target = i.getId(); break; }
            }
        } else {
            previous = cur;
            target = voice;
        }
        if (target == null) return;
        if (!getSoftKeyboardController().switchToInputMethod(target))
            Toast.makeText(this, "Klavye değiştirilemedi", Toast.LENGTH_SHORT).show();
    }

    @Override public void onAccessibilityEvent(AccessibilityEvent event) { }
    @Override public void onInterrupt() { }
}
