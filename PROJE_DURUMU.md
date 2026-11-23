# PadMapper – Proje Durumu

**Sürüm:** kart 0.9.13 · uygulama 0.36 (derlenmedi, CI bekliyor)

## Çalışan özellikler
- BLE pad → USB/BLE klavye + fare + gamepad eşleme (profil, katman, makro, turbo, aç-kapa, birlikte basış)
- Bluetooth klavye/fare girişi (doğrudan veya gamepad'e çevirme)
- Telefon/PC ayar ekranı (web/padmapper.html, Android uygulaması aynı sayfayı kullanır), kablosuz güncelleme

## 0.9.13 / 0.36'da eklenenler
- Yazılım bekçisi: çıkış iş parçacığı veya iş kuyruğu 10 sn donarsa kart yeniden başlar
- Pad bağlandı ama 12 sn içinde hazır olmadıysa bağlantı kesilip yeniden aranır
- Pad'in eşleşme anahtarı geçersizse (başka cihaza eşlenmiş) anahtar silinip yeniden eşlenir
- Makro/turbo yarıda kesilince "basılı tut" adımındaki tuşlar artık takılı kalmıyor
- Pad pil seviyesi (BAS): ana ekranda 🔋%, %15 ve altında LED'de kısa kırmızı
- LED profil rengi: 1 yeşil, 2 sarı, 3 mor, 4 beyaz; katman etkinken yanıp söner; mavi = düz gamepad
- Çubuk eğrisi + ters ölü bölge (fare ve gamepad modunda)
- Klavye/fare bölümü Giriş ekranına bağlandı (önceden kodu vardı ama ekranda yoktu); bağlıyken ana ekranda görünür
- Kopya web/index.html silindi

## Kararlar
- Bekçi donanım değil yazılım: nRF donanım bekçisi önyükleyicide de çalıştığından güncelleme sırasında kartı sıfırlayabilirdi
- Ayar biçimi: başlık bayt 14 bit0 = profil başına 16 baytlık çubuk uzantısı. Eğri kullanılmıyorsa ayar eskisiyle birebir aynı (eski kartlarla uyumlu)

## Bilinen eksikler
- Klavye/fare için anahtar kaybı düzeltmesi yok (yalnızca pad için var)
- Firmware yerelde derlenemiyor (NCS yok); motor masaüstünde test edildi, geri kalanı CI ile doğrulanmalı

## Sıradaki adım
- Değişiklikleri ayrı dalda CI'da derle, kartta dene, sonra main'e al
