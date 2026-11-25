# PadMapper – Proje Durumu

**Sürüm:** kart 0.9.14 · uygulama 0.37 (yayında, 2026-11-25)

## Çalışan özellikler
- BLE pad → USB/BLE klavye + fare + gamepad eşleme (profil, katman, makro, turbo, aç-kapa, birlikte basış)
- Bluetooth klavye/fare girişi (doğrudan veya gamepad'e çevirme)
- Telefon/PC ayar ekranı (web/padmapper.html, Android uygulaması aynı sayfayı kullanır), kablosuz güncelleme

## 0.37'de
- Bluetooth güncelleme düzeltildi: önyükleyici kablosuzda çift bölme kullanır (en çok ~405 KB); USB bellekli yazılım 527 KB olduğu için reddediliyordu. Kablosuzda artık belleksiz sürüm (314 KB) gönderiliyor; uygulama bu dosyayı da içeriyor. Boyut aşılırsa anlaşılır hata.
- "Nasıl çalışsın" altında "Aynı anda kaç tuşa bassın?" (Tekli/İkili/Üçlü/Dörtlü); seçici yuvaları sırayla açar
- Çizimde tuşların yanında atamalar yeniden görünüyor (birlikte basış "B+X" diye); düz gamepad modunda gizli

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
- Kablosuz güncellemede kartın USB belleği (AYAR.HTM) gelmez; sonraki kablolu güncellemede geri gelir. Bellekli sürüm 405 KB sınırına sığmıyor
- Bekçi donanım değil yazılım: nRF donanım bekçisi önyükleyicide de çalıştığından güncelleme sırasında kartı sıfırlayabilirdi
- Ayar biçimi: başlık bayt 14 bit0 = profil başına 16 baytlık çubuk uzantısı. Eğri kullanılmıyorsa ayar eskisiyle birebir aynı (eski kartlarla uyumlu)

## Bilinen eksikler
- (0.9.14) Klavye/fare için de geçersiz anahtarda yeniden eşleşme eklendi
- Firmware yerelde derlenemiyor (NCS yok); CI ile derleniyor, motor masaüstünde test edildi

## Sıradaki adım
- Telefonda Bluetooth güncellemesini ve ikili basmayı dene
