# PadMapper

Seeed XIAO nRF52840 ile reWASD benzeri gamepad eşleyici. Bluetooth (BLE) gamepad'i okur, ayarlara göre tablete USB üzerinden klavye, fare ya da gamepad olarak gönderir.

- `firmware/` – kart yazılımı (nRF Connect SDK / Zephyr)
- `web/index.html` – ayar ekranı (Chrome'da açılır)

## Kart yazılımını yükleme

1. GitHub'da **Actions** sekmesine gir, en son başarılı "Kart yazılımını derle" çalışmasını aç.
2. Alttaki **padmapper-uf2** dosyasını indir, zip'ten `padmapper.uf2`'yi çıkar.
3. XIAO'yu USB ile bilgisayara tak, **RST** düğmesine hızlıca **iki kez** bas. `XIAO-SENSE` (ya da benzeri) adlı bir sürücü açılır.
4. `padmapper.uf2` dosyasını bu sürücüye sürükle. Kart kendini yeniden başlatır.

HID Remapper'a dönmek istersen aynı yolla onun `.uf2` dosyasını yüklemen yeterli.

## Durum

- [x] Adım 1: kontrolcüyü bul, bağlan, tuş haritasını ve basışları USB seri porttan gönder
- [ ] Adım 2: tablete klavye + fare + gamepad çıkışı
- [ ] Adım 3: eşleme motoru (makro, turbo, katman, birleşim…)
- [ ] Adım 4: ayar ekranıyla bağlantı, tuş tanıma, canlı test
