# Silent Hill 3 — Native PC Port

**Silent Hill 3'ün (ABD, SLUS-20622) emülatörsüz, native x86-64 Windows portu.**

Oyunun PlayStation 2 ana işlemcisi (Emotion Engine, MIPS R5900) için yazılmış kodu
**statik olarak** C'ye çevrilir ve MSVC ile normal bir Windows `.exe`'sine derlenir.
Oyun mantığı yorumlanmaz ya da JIT ile çevrilmez; doğrudan işlemcinizde native kod
olarak çalışır. Hedef, modern bilgisayarlara uygun bir Silent Hill 3 portu: yüksek
çözünürlük, 16:9 geniş ekran, 60 FPS seçeneği, modern kumandalar, kendi launcher'ı ve
**zamanla PCSX2'ye hiç bağımlı olmadan** çalışmak.

Bugün grafik (GS/VU1), ses (SPU2) ve PS2 donanımının bir kısmı hâlâ
[PCSX2](https://github.com/PCSX2/pcsx2) çekirdeğinden geliyor. Bu parçalar aşama aşama
native kodla değiştiriliyor; plan ve ilerleme [`NATIVE_PLAN.md`](NATIVE_PLAN.md) dosyasında.
Altyapının adı **ps2native**: recompiler (`recompiler/ps2recomp`) ve runtime
(`runtime/`).

> **Oyun dosyaları ve BIOS bu depoda yoktur.** Kendi diskinizden aldığınız oyun
> imajını ve kendi PS2 konsolunuzdan aldığınız BIOS'u kullanmanız gerekir.

---

## İçindekiler

- [Nasıl çalışır](#nasıl-çalışır)
- [Durum](#durum)
- [Klasör yapısı](#klasör-yapısı)
- [Kurulum](#kurulum)
- [Silent Hill 3'ü derlemek](#silent-hill-3ü-derlemek)
- [Oynamak: launcher](#oynamak-launcher)
- [Ayarlar (`ps2native.ini`)](#ayarlar-ps2nativeini)
- [Komut satırı](#komut-satırı)
- [Yamalar (pnach)](#yamalar-pnach)
- [Release paketi](#release-paketi)
- [Testler](#testler)
- [Hata ayıklama araçları](#hata-ayıklama-araçları)
- [Bilinen sorunlar ve sınırlamalar](#bilinen-sorunlar-ve-sınırlamalar)
- [Lisans ve yasal notlar](#lisans-ve-yasal-notlar)

---

## Nasıl çalışır

```
 Oyun diski (.iso)
   │  SYSTEM.CNF → SLUS_206.22 (EE ELF)
   ▼
 ps2recomp (Python) ──► gen/*.c ──► MSVC ──► SLUS_206_22.exe
   ├─ R5900 çözücü (MMI, FPU, VU0 makro dahil)        ├─ ps2native runtime (C)
   ├─ fonksiyon keşfi, jump table'lar                 │   native EE kodu, EE kernel HLE,
   ├─ pnach yamaları (kod yamaları derlemeye işlenir) │   thread/semafor/kesme/alarm, SIF
   └─ C kodu üretimi                                  └─ PCSX2 çekirdeği (C++)
                                                          gerçek IOP (BIOS), CD/DVD, SPU2 ses,
                                                          DMAC, VIF, VU0/VU1 (microVU), IPU,
                                                          GIF, GS (DX12/DX11/Vulkan/GL), pad,
                                                          hafıza kartı
```

* **Recompiler (`recompiler/ps2recomp`, Python):** ELF okur, tam bir R5900 komut
  çözücüsüyle fonksiyonları bulur (semboller, `jal` hedefleri, `switch` jump
  table'ları, veri pointer'ları, `lui/addiu` sabitleri, ikincil giriş noktaları) ve
  her fonksiyonu bir C fonksiyonuna çevirir. Delay slot'lar, branch-likely, 128-bit
  GPR'ler, HI/LO, MMI, PS2 FPU kuralları (Inf/NaN yok, sıfıra doğru yuvarlama) ve VU0
  makro modu desteklenir. ISO verilirse önyüklenen ELF'i diskten kendisi çıkarır.
* **Runtime (`runtime/`, C):** native kodun çalıştığı ortam. EE kernel'i HLE olarak
  yeniden yazılmıştır: fiber tabanlı öncelikli thread'ler, semaforlar,
  sleep/wakeup/suspend, INTC/DMAC kesme işleyicileri, alarmlar, `SetSyscall`, SIF
  DMA/RPC servisleri. Gerçek zamanlı EE saati ve "güvenli nokta" (geri dallanmalar)
  tabanlı olay işleme burada.
* **PCSX2 arka ucu (`host/`, C++):** PCSX2'nin VMManager'ı "native host" modunda
  açılır (`patches/pcsx2-native-host.patch`). EE CPU sağlayıcısı derlenmiş oyunu
  çalıştırır; donanım erişimleri PCSX2'nin bellek haritasından (vtlb) geçer. BIOS
  verilirse **gerçek IOP** BIOS'tan önyüklenir (LLE): disk, ses, kumanda ve hafıza
  kartı PCSX2'nin kendi uygulamalarıdır.
* **Yazılımsal arka uç (`runtime/src/soft`):** bağımlılıksız, gcc ile derlenir;
  kendi DMA/VIF/GIF ve yazılımsal GS'si vardır (VU1 ve IPU yok). Test ve deney için.
* **Launcher (`launcher/`, Python/Tkinter):** oyun dosyalarını seçme, ayarlar ve
  oynatma ekranı (12 dil).

## Durum

Silent Hill 3 (SLUS-20622), PCSX2 arka ucu + gerçek IOP (LLE) ile:

| Bölüm | Durum |
|---|---|
| Açılış, logolar, hafıza kartı kontrolü, uyarı ekranı | ✅ Çalışıyor, açılış süresi gerçek PS2 / PCSX2 ile aynı |
| Ses, disk okuma, hafıza kartı, kumanda (Xbox/PS4/PS5/Switch Pro + klavye) | ✅ |
| Yüksek çözünürlük (1×–8×), 16:9 geniş ekran, kenarlıksız tam ekran | ✅ |
| Harici GPU seçimi (NVIDIA Optimus / AMD) | ✅ |
| 60 FPS, kamera yamaları (çalışma anında açılıp kapanabilir) | ✅ derleniyor, oyun içinde ayrıca test edilmeli |
| **FMV / video oynatma** | ✅ Tam ekran oynuyor ([`VIDEO_FIX_NOTES.md`](VIDEO_FIX_NOTES.md)). FMV'ler yaklaşık yarı hızda (≈16 resim/s), VBlank zamanlaması inceleniyor |
| Oyun içi kod parçaları (overlay'ler) | ✅ Çalışma anında derlenip DLL olarak yükleniyor (`runtime/src/core/dyn.c`) |
| Oyun içi oynanış | ⏳ Test ediliyor |

PCSX2'siz geçiş ([`NATIVE_PLAN.md`](NATIVE_PLAN.md)):

| Aşama | Durum |
|---|---|
| 1. IOP native (BIOS'suz), `--hle-iop` | ⏳ CD/dosya (ISO'dan), IRX yükleme, SH3 ses sürücüsü ve CRI ADX protokolleri, IOP heap çalışıyor; yükleme ekranı geçiliyor. Hafıza kartı ve pad sırada |
| 2. FMV: IPU yerine native MPEG-2 (FFmpeg), `PS2NATIVE_MPEG=native` | ✅ Gerçek IPU çıktısıyla kare kare doğrulandı (ortalama fark 0,11/255) |
| 3. VU1, 4. GS renderer, 5. ses (SPU2), 6. PCSX2'yi kaldırma | ⏳ |

## Klasör yapısı

```
ps2native/
├── recompiler/ps2recomp/   Python recompiler (elf, r5900, analysis, codegen, pnach, iso, build)
├── runtime/
│   ├── include/            ps2rt.h, ps2_mmi.h, ps2_vu0.h (üretilen kodun kullandığı başlıklar)
│   └── src/core/           kernel.c (EE kernel HLE), dispatch.c, memory.c, sif.c, sif_lle.c, rtmain.c,
│                           dyn.c (overlay derleme), hle_iop.c / iop_services.c / iso.c (native IOP),
│                           hle_mpeg.c / mpeg_native.c (native FMV)
│   └── src/soft/           yazılımsal arka uç
├── host/                   PCSX2 arka ucu: Main.cpp (pencere, ayarlar, kumanda), NativeCpu.cpp (EE sağlayıcı)
├── patches/                PCSX2 commit numarası + native-host yaması
├── games/sh3/              Silent Hill 3 yapılandırması (game.json) ve PCSX2 pnach yamaları
├── launcher/               SH3 launcher'ı (sh3_launcher.py) ve çevirileri (sh3_lang.py)
├── app/                    Genel recompiler arayüzü (ps2native_gui.py) ve CLI girişi
├── scripts/                PCSX2 kurulum/derleme, paketleme betikleri
├── tests/                  Assembler ile üretilen R5900 test programları
├── bios/                   Kendi BIOS'unuzu buraya koyun (depoda yok)
├── NATIVE_PLAN.md          PCSX2'siz native port planı, ölçümler ve bulgular
└── VIDEO_FIX_NOTES.md      FMV sorunu üzerine çalışma notları
```

Depoda **olmayan** (oluşturulan ya da sizin sağladığınız) klasörler: `third_party/`
(PCSX2), `build/`, `release/`, `tools/`, `bios/*.bin`.

## Kurulum

Gereksinimler:

* Windows 10/11 x64
* Visual Studio (MSVC, "Desktop development with C++") + CMake + Ninja.
  Derleme betikleri `scripts/configure_pcsx2.bat` içindeki `vcvars64.bat` yolunu kullanır;
  Visual Studio başka bir yerdeyse bu yolu düzeltin.
* Python 3.11+ (launcher görselleri için `pip install pillow`, paketleme için
  `pip install pyinstaller`)
* git, curl, 7-Zip (PCSX2 kurulumu için)
* Native FMV için FFmpeg (LGPL, shared): `ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip`
  ([BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases)) dosyasını
  `tools/ffmpeg/` altına açın. Paketleme bu DLL'leri oyun klasörüne kopyalar (PCSX2'nin
  kendi FFmpeg'inde decoder yok).

PCSX2'yi test edilen commit'te indirip yamayı uygulamak ve hazır bağımlılıkları almak için:

```bat
scripts\setup_pcsx2.bat
```

İlk oyun derlemesi PCSX2'yi bir kez derler (15–20 dk). Sonraki derlemeler yalnızca
oyunun kodunu ve runtime'ı derler (1–3 dk).

## Silent Hill 3'ü derlemek

```bat
cd recompiler
python -m ps2recomp "D:\Oyunlar\Silent Hill 3 (USA).iso" -o ..\build\games\sh3 --config ..\games\sh3\game.json
```

Bu komut:

1. ISO'dan `SYSTEM.CNF`'i okuyup `SLUS_206.22`'yi çıkarır,
2. `game.json`'daki pnach yamalarını uygular (kod yamaları ELF'e işlenir, açılıp
   kapanabilen yamalar çalışma anı seçeneği olur),
3. ~12.800 fonksiyonu C'ye çevirir (`build/games/sh3/gen/`),
4. MSVC ile `build/games/sh3/bin/SLUS_206_22.exe`'yi derler ve DLL'leri yanına koyar,
5. `bin/ps2native.ini`'ye ISO ve BIOS yollarını yazar.

BIOS'u `bios\` klasörüne koyun ya da `--bios dosya.bin` verin.

## Oynamak: launcher

```bat
python launcher\sh3_launcher.py
```

ya da paketlenmiş hâliyle `release\SilentHill3\Silent Hill 3.exe`.

* **Oyun dosyaları:** Silent Hill 3 disk imajını (.iso/.chd/.cso) ve PS2 BIOS'unu
  seçin. ISO'nun gerçekten SLUS-20622 olduğu ve BIOS'un geçerli bir PS2 BIOS'u olduğu
  doğrulanır; BIOS sürümü ve bölgesi gösterilir. Dosyalar kopyalanmaz, yalnızca
  konumları kaydedilir.
* **Grafik:** iç çözünürlük, görüntü oranı, tam ekran, VSync, anizotropik filtre,
  FXAA, AMD CAS keskinleştirme, grafik API'si, harici/dahili GPU.
* **Oyun ayarları:** 60 FPS, hızlı yükleme, omuz üstü kamera yamaları.
* **Kontroller:** kumanda otomatik/kapalı, titreşim, klavye tuşlarını yeniden atama.
* **Kayıtlar:** hafıza kartı yedekleme/geri yükleme (oyundan önce otomatik yedek).
* **Dil:** Türkçe, English, Español, Português (BR), Français, Deutsch, Italiano,
  Русский, Polski, 日本語, 한국어, 简体中文. İlk açılışta Windows dili seçilir.

Oyun içinde **Alt+Enter** veya **F11** tam ekranı açıp kapatır.

## Ayarlar (`ps2native.ini`)

`bin/ps2native.ini` launcher tarafından yazılır; elle de düzenlenebilir. Komut satırı
seçenekleri bu değerleri ezer.

| Anahtar | Varsayılan | Anlamı |
|---|---|---|
| `iso`, `bios`, `boot` | | Disk imajı, BIOS dosyası/klasörü, önyükleme yolu (`cdrom0:\SLUS_206.22;1`) |
| `renderer` | auto | `dx12`, `dx11`, `vulkan`, `opengl`, `software` |
| `upscale` | 3 | İç çözünürlük çarpanı (1 = PS2, 3 ≈ 1080p, 6 ≈ 4K) |
| `widescreen` | 1 | 16:9 görüntü (geniş ekran yamasıyla birlikte) |
| `fullscreen` | 0 | Kenarlıksız tam ekran |
| `vsync` | 0 | Dikey senkron |
| `anisotropy` | 16 | 0/2/4/8/16 |
| `fxaa`, `sharpen` | 0 | FXAA; CAS keskinleştirme (0–100) |
| `gpu` | high | `high`: harici GPU, `integrated`: dahili GPU |
| `fast_cdvd` | 1 | Disk okuma gecikmelerini atla |
| `fps60`, `camera_behind`, `camera_l3` | 0 | Çalışma anında açılıp kapanan oyun yamaları |
| `controller`, `vibration` | 1 | Kumanda ve titreşim |
| `key_<Tuş>` | | Klavye ataması, ör. `key_Cross=S`, `key_Start=Return` |

## Komut satırı

```
SLUS_206_22.exe [seçenekler]
  --iso DOSYA          disk imajı
  --bios YOL           PS2 BIOS dosyası veya klasörü (gerçek IOP)
  --boot YOL           önyükleme yolu (cdrom0:\SLUS_206.22;1)
  --renderer AD        auto, dx12, dx11, vulkan, opengl, software
  --upscale N          iç çözünürlük çarpanı
  --widescreen / --no-widescreen
  --fullscreen / --windowed
  --frames N           N kare sonra çık (testler için)
  --dump-frame DOSYA   çıkmadan önce son kareyi PNG olarak kaydet
  --reference          aynı diski PCSX2 ile tamamen emüle ederek çalıştır (karşılaştırma)
  --trace              sistem çağrıları, thread durumları, profil raporu
  -v                   ayrıntılı günlük
```

Recompiler (`python -m ps2recomp`): `--config game.json`, `--pnach dosya`,
`--pnach-sections "Widescreen 16:9,60 FPS"`, `--backend pcsx2|soft`, `--bios`,
`--entry 0xADDR`, `--skip 0xADDR`, `--hints funcs.json` (REA/Ghidra/IDA fonksiyon
listesi), `--no-build`.

## Yamalar (pnach)

`games/sh3/SLUS-20622_2498951B.pnach` PCSX2 topluluk yamalarıdır (geniş ekran:
nemesis2000, 60 FPS: asasega, kamera: Snake356). Native kod çalışma anında
değiştirilemeyeceği için:

* **Kalıcı kod yamaları** (`pnach_sections`) derlemeden önce ELF'e işlenir.
* **Açılıp kapanabilen yamalar** (`pnach_options`): veri yamaları her VSync'te
  uygulanır; "koşullu dal → nop" türündeki kod yamaları ek bir koşulla derlenir ve
  `ps2native.ini`'deki anahtarla (`fps60=1`) açılır.

## Release paketi

```bat
python scripts\package_sh3.py          :: release\SilentHill3\
python scripts\package_sh3.py --zip    :: + release\SilentHill3.zip
```

Paket: `Silent Hill 3.exe` (launcher), `bin\` (oyun + PCSX2 DLL'leri), lisans ve
okubeni. Oyun dosyası ve BIOS içermez. Betik, mevcut paketteki hafıza kartını ve
ayarları korur; launcher ya da oyun açıkken çalışmaz.

## Testler

```bat
python tests\run_tests.py --backend=pcsx2
python tests\run_tests.py --backend=soft
```

Testler gerçek R5900 makine kodu üreten küçük bir assembler (`tests/mips_asm.py`) ile
ELF'ler oluşturur, recompile eder, exe'yi çalıştırıp sonucu doğrular: CPU
(sembollü/sembolsüz), FPU yuvarlama, thread zamanlama, kesme/zamanlayıcı/alarm, GS
çizimi (piksel kontrolü), VU1 mikroprogramı (VIF1 MPG/UNPACK → XGKICK), VU0 mikro
modu (VCALLMS) ve hataların yakalandığını gösteren negatif testler.

## Hata ayıklama araçları

* `--trace`: her sistem çağrısı, 2 saniyede bir thread durum dökümü, misafir kodu
  örnekleme profili (hangi adreste ne kadar zaman) ve host profili (PDB sembolleriyle
  en sıcak fonksiyonlar), IPU/DMA erişim günlüğü.
* `PS2NATIVE_WATCH=2B5830,2B5F50`: bu adreslere yapılan dolaylı çağrıları (callback'ler,
  kesme işleyicileri) günlüğe yazar.
* `PS2NATIVE_IO_GS=1`: G/Ç günlüğünü GS `CSR`/`IMR` erişimleriyle sınırlar.
* `--reference`: aynı oyunu tamamen PCSX2 ile çalıştırıp davranış/zamanlama
  karşılaştırması yapmak için.

## Bilinen sorunlar ve sınırlamalar

* **FMV'ler siyah:** Video çözülüyor, ancak kareleri serbest bırakan GS kesme
  işleyicisi çağrılmıyor ve oynatıcı 5 kareden sonra boş tampon bekliyor. Ayrıntılar
  ve yapılanlar: [`VIDEO_FIX_NOTES.md`](VIDEO_FIX_NOTES.md).
* DLSS / FSR / frame generation doğrudan yok (PS2 çizimi hareket vektörü üretmez).
  Sürücü seviyesinde frame generation (NVIDIA Smooth Motion) kullanılabilir.
* Çalışma anında yüklenen kod (overlay) statik olarak derlenemez; `--entry` ile ya
  da ayrıca derlenerek çözülür.
* `VCALLMS` ile başlatılan VU0 programı EE devam etmeden tamamlanır.
* Yazılımsal arka uçta IPU yok, yani video oynatılamaz.

## Lisans ve yasal notlar

* ps2native, PCSX2 kodu (GPL-3.0+) ile bağlandığı için **GPL-3.0-or-later**
  lisanslıdır (`LICENSE`). Üretilen exe'ler de bu lisansa tabidir.
* Bu depo **hiçbir oyun dosyası, BIOS, oyundan üretilmiş kod ya da Konami'ye ait
  görsel içermez.** Kendi yasal kopyalarınızı kullanın.
* Silent Hill, Konami Digital Entertainment'ın ticari markasıdır. Bu proje Konami
  veya Sony ile bağlantılı değildir.
* EE kernel'inin bazı davranışları (ör. `RemoveIntcHandler` dönüş değeri, alarm sistem
  çağrısı numaraları) [ps2-native-recomp](https://github.com/exoteric33/ps2-native-recomp)
  (MIT) ile karşılaştırılarak doğrulandı; o projeden kod kopyalanmadı.
