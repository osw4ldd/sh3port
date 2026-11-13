# PCSX2'siz native çalışma planı

Hedef: SH3 (SLUS-20622) BIOS ve PCSX2 çekirdeği olmadan, tamamen native olarak PC'de çalışsın.
Bugünkü durum: EE kodu native (statik recompile + çalışma anında derlenen overlay DLL'leri).
GS, VIF/VU1, IPU, SPU2 ve IOP ise PCSX2'den geliyor (BIOS ile LLE).

## 1. Analiz (2026-11-13)

Araçlar:
- Ghidra 12.1.4 + ghidra-emotionengine-reloaded v2.1.39 (r5900:LE:32 dili), headless.
- REA. Windows'ta Ghidra yolu yalnızca PE/x86 dosyalarını kabul ediyor, MIPS ELF'i açmıyor. Bu yüzden SH3 analizi Ghidra headless ile yapıldı.
- ran-j/PS2Recomp kaynak kodu. Bu repo GPL-3.0; buradan **yalnızca fikir** alındı, kod kopyalanmadı.

### SH3'te sorun yaşadığımız noktalar (Ghidra decompile)

| Adres | Ne yapıyor | Bizim için anlamı |
|---|---|---|
| `0x1EC420` | `FlushCache(2)`, ardından `0x1512D0(file,0,0x1F6D600)`, `0x151130`, `0x1EC3E0` | Overlay yükleyici. |
| `0x1512D0` | Nesne `0x484980` üzerinden komut kuyruğuna istek atıyor (handler `0x154680`) | Dosya yükleme ayrı bir thread'de, CD okumasıyla yapılıyor. Native IOP'ta CD okuma çalışırsa overlay de yüklenir. Overlay kodu zaten `dyn.c` ile derleniyor. |
| `0x2B5830` | FMV VSync callback'i. `REG_GS_CSR` bit 13'ü (FIELD) okuyup alanı değiştiriyor | FMV kök nedenini doğruluyor. Native GS'te FIELD her VSync'te değişmeli (SMODE1/SYNCV ayarlıyken). |
| `0x2B5FC0` | `0x100520` field değişene kadar dönüyor, sonra gösterim bayrakları | Aynı konu. |
| `0x103918` / `0x103AE8` / `0x103050` | `sceSifBindRpc` / `sceSifCallRpc` / `sceSifSendCmd` | IOP servisleri RPC numarasıyla bağlanıyor. HLE bu katmanda yapılabilir. |

### SH3'ün kullandığı IOP servisleri (ELF'teki `lui 0x8000`/`ori` sabitleri + decompile)

| SID | Modül | Kaynak |
|---|---|---|
| `0x80000001` | fileio | sif.c'de var |
| `0x80000003` | iopheap | sif.c'de var |
| `0x80000006` | loadfile | sif.c'de var |
| `0x80000592` / `593` / `595` / `597` / `59A` | cdvdfsv: init / S-cmd / N-cmd / search / diskready | `0x10B178` "Libcdvd bind err"; PS2SDK `common/include/libcdvd-rpc.h` |
| `0x80000900`–`0x8000091B` | libdbc / dbcman (pad, DualShock 2) | `0x121BD0` "libdbc: bind failed", "dbcman.irx" |
| `0x80000701` | sdrdrv (libsd RPC) | `0x109EF0` |
| `0x80000400` | mcserv (hafıza kartı) | |

Diskten yüklenen IRX'ler: `dbcman`, `ds2u`, `sio2d`, `sio2man`, `libsd`, `sdrdrv`, `mcman`, `mcserv`, `iop_sound/cri_adxi.irx` (CRI ADX ses akışı), `libsmf2` (MIDI), `sd_call`.

### Ölçülen RPC trafiği (gerçek IOP, LLE)

Ölçüm komutu:
`PS2NATIVE_RPCLOG=1 PS2NATIVE_PRESS="2600-5000/150:Return,2675-9000/150:S" SLUS_206_22.exe --frames 7000`

7000 karede, intro'dan oyuna girişe kadar 5715 RPC çağrısı görüldü. HLE IOP'un karşılaması gereken (SID, fno) çiftlerinin tamamı:

| SID | fno (adet) | Not |
|---|---|---|
| `80000001` fileio | 0 open (58), 1 close (57), 2 read (83), 4 lseek (168), 23 (139), 255 (1) | Paket düzeni SCE ROM fileio'su; PS2SDK'dan farklı. open: `+12` mode, `+20` path. |
| `80000003` iopheap | 1 (9), 2 (12), 4 (12) | |
| `80000006` loadfile | 6 (12), 255 (1) | |
| `80000592` cdvd init | 0 (2) | |
| `80000593` cdvd S-cmd | 1 (1), 4 (1439), 34 (1) | 1 = ReadClock. 4 = muhtemelen GetError, her okumadan sonra çağrılıyor; doğrulanacak. |
| `80000595` cdvd N-cmd | 1 read (590), 14 (590) | read paketi: lbn, sectors, ee dst, rmode, ... (24 bayt) |
| `80000597` cdvd search | 0 (106) | `cdvdfsv_rpc4_sz12c` (300 bayt) |
| `80000701` sdrdrv | 32784, 32976, 32992, 33008, 33024 (97) | |
| `80000400` mcserv | 1, 2, 3, 4, 5, 12, 13, 18, 21, 254 | |
| `80000900` / `8000091B` dbcman | `0x80000901`… / `0x8000091B` (982, her karede pad) | |
| `00000057` | 0x01000000 (981), 0x02000000 (1) | SH3 ses sürücüsü (sd_call / smf2). |
| `7D000000` | 2, 1024, 1027, 1032, 1034, 1058 | CRI ADX (cri_adxi.irx). |

### SDK imza taraması (SH3)

ran-j'deki sce-symbol-scanner veritabanıyla (yalnızca analiz için kullanıldı, repoya eklenmedi) SH3'te **627 SDK fonksiyonu** bulundu. Sonuç `build/games/sh3/sdk_symbols.json` dosyasında (build/ git dışında).

| Kütüphane | Adet | Örnekler |
|---|---|---|
| libkernl | 259 | syscall sarmalayıcıları |
| libmpeg | 92 | `sceMpegCreate`, `sceMpegGetPicture`, `sceMpegDemuxPss`, `sceMpegIsEnd`. FMV HLE hedefi. |
| libcdvd | 18 | `sceCdRead` 0x10B650, `sceCdSync` 0x10AE30, `sceCdInit` 0x10B0B0, `sceCdLayerSearchFile` 0x10A8F0 |
| libdbc | 13 | `sceDbcInit` 0x121B88, `sceDbcSRData`, `sceDbcReceiveData` |
| libmc | 27 | `sceMcOpen` / `Read` / `Write` / `Sync`… |
| libipu | 7 | `sceIpuInit`, `sceIpuSync` |
| libgraph | 11 | `sceGsSyncPath` 0x116E08, `sceGsSwapDBuff` |
| libsdr | 3 | `sceSdRemote` 0x10A008 |

Bu sayede HLE iki seviyede yapılabilir: RPC (SID/fno) veya doğrudan SDK fonksiyonu.

### ran-j/PS2Recomp'tan alınan fikirler

1. **IOP servislerini SID'e göre HLE etmek** (`ps2xIOP` içindeki `IopService::sids()` / `handleRpc`). Bizim `sif.c` zaten bu modelde; servis listesini genişleteceğiz.
2. **SDK imza veritabanı** (sce-symbol-scanner). Relocation alanları maskelenmiş baytların SHA-1'i alınıyor ve isimsiz ELF'te `sceCdRead`, `sceMpeg*`, `scePad*` gibi fonksiyonlar bulunuyor. Böylece IPU ve MPEG kütüphane seviyesinde HLE edilebilir. ran-j bunu FFmpeg ile yapıyor.
3. **Oyuna özel fonksiyon değiştirme** (`handler@0xADDR`, `ret0`). Bizde `game.json` içinde benzeri eklenecek.
4. **Delay-slot koruması.** Bir kontrol transferinin hemen arkasındaki adres, fonksiyon başlangıcı olarak tohumlanmamalı. **Yapıldı:** `dyn.c` içinde `is_branch()`. Overlay sıfırdan derlenip 9000 kare çökmeden çalıştı.
5. **Kaçınılacak şey:** ran-j'de `libsd` HLE'si yaklaşık 70 satırlık bir stub; ses, SPU2 emülasyonu olmadan yok. Biz ses için SPU2 çekirdeğini ayrı ele alacağız.

## 2. Aşamalar

PCSX2 tamamen sökülmeden önce her aşama tek başına çalışır durumda olacak. Eski LLE yolu karşılaştırma için `--reference` ile açık kalıyor.

1. **IOP'u native yap (BIOS gerekmez).** Başlandı:
   - `--hle-iop` bayrağı: BIOS olsa bile HLE IOP'u zorlar.
   - `PS2NATIVE_RPCLOG=1`: LLE'de gerçek RPC'leri loglar (referans). Bu ölçümle her servis LLE'ye karşı doğrulanacak.

   Eklenecekler: `sif.c` HLE'sine şunlar eklenecek: cdvdfsv (ISO'dan okuma), dbcman (klavye/gamepad), mcserv (dosyaya kayıt), sdrdrv (ilk etapta sessiz). GS, VU1 ve IPU bu aşamada hâlâ PCSX2'den gelir.
   **Durum (2026-11-13), `--hle-iop` ile, BIOS'un IOP'u olmadan:**
   - **Fonksiyon seviyesinde HLE:** `game.json` `"hle"` haritası. Recompiler fonksiyon başına `ps2_hle()` kancası koyuyor; LLE modunda orijinal kod çalışıyor. 18 SDK fonksiyonu native:
     - fileio: `sceOpen` / `Read` / `Lseek` / `Close` / `Devctl` / `Getstat`
     - libcdvd: `sceCdRead` / `Sync` / `LayerSearchFile` …
     Kaynak `runtime/src/core/hle_iop.c`; ISO okuyucu `runtime/src/core/iso.c`.
   - **`rom0:ROMVER` sanal dosyası.** Açılamazsa oyun sürekli yeniden deniyor.
   - **RPC servisleri** (`runtime/src/core/iop_services.c`):
     - SH3 ses sürücüsü `0x57`: durum 7 = hazır.
     - CRI ADX `0x7D000000`: sıfır olmayan tutamaçlar. 0 dönerse oyun "Can't create ADX handle" deyip sonsuz döngüye giriyor.
     - CRI DTX kanalı: SIF DMA'ya boş cevap, sayaç +1.
   - **Sonuç:** IRX'ler yükleniyor, dosya tablosu ISO'dan okunuyor, 412 CD okuması yapılıyor (LLE'de 413) ve "SILENT HILL 3" yükleme ekranı görünüyor.
   - **SH3 ses sürücüsü komutları:** `0x1404TTSS` banka yükleme durum bitlerini set ediyor (0x207 → 0x777). Bitler gelmezse oyun yükleme ekranında kalıyordu.
   - **IOP heap:** first-fit ayırıcı + `free`. Eski 448 KB'lık "bump" ayırıcı doluyordu; SH3 266 KB + 160 KB'lık tamponlar ayırıyor.
   - **CRI DTX:** bu aşamada LLE'de de boş dönüyor, boş cevap yeterli. Protokol: `0x20DAB0` gönderme, `0x20DA10` alma; 16 baytlık chunk girdileri.
   - **Sonuç:** 421 CD okuması, bütün ses bankaları yüklendi (durum 0x777), yükleme ekranı geçildi.
   - **Takıldığı yer:** hafıza kartı. LLE'de bu noktada mcserv (`0x80000400`) fno 21/1 (GetInfo/Sync), 12, 13, 18 çağrıları geliyor; ardından dbcman (pad). Sıradaki iş libmc'yi fonksiyon seviyesinde HLE etmek (SDK taramasında `sceMcInit` / `GetInfo` / `Sync` / `Open` / `Read` / `Write` … adresleri var), kayıtları `memcards/` klasöründe tutmak, sonra dbcman ile pad.
   - **Kalan servisler:** dbcman (pad), mcserv (hafıza kartı), sdrdrv (şimdilik sıfır).
   - **Ekran görüntüsü:** `--dump-frame` yalnızca Windows tarzı (`\`) yolla çalışıyor.

2. **FMV: IPU yerine native MPEG-2 (FFmpeg).** `PS2NATIVE_MPEG=native` ile çalışıyor.
   - **Çözücü:** `runtime/src/core/mpeg_native.c`. FFmpeg 9.0 LGPL shared (`tools/ffmpeg`, git dışında) kullanılıyor; PCSX2'nin FFmpeg'inde hiç decoder yok. DLL majorleri aynı (avcodec-63), paketleme tam sürümü kopyalıyor.
   - **RAW8 düzeni gerçek IPU çıktısıyla ölçüldü** (`PS2NATIVE_MPEG_DUMP` gözlem modu): makroblok `Y[16][16] Cb[8][8] Cr[8][8]` (PCSX2 `IPU_MultiISA.h:26`), **sütun öncelikli** sıra (`n = mx*mbh + my`). 170 karede ortalama fark 0,11/255, kare sırası birebir.
   - **Veri yolu:** oyunun kanal 4 (toIPU) DMA zinciri IPU'ya gitmiyor. `mpeg_ipu_dma_start` zinciri DMAC gibi yürüyor, ES'yi kuyruğa alıyor, DMA'yı bitmiş gösteriyor (MADR/TADR/QWC, STR, CIS4). Oyunun halka hesabı (`0x2B7EB0`) değişmeden çalışıyor.
   - **`sceMpegGetPictureRAW8` native:** resim yoksa yalnızca "nodata" callback'i çağrılıyor. Gerçek kütüphane stop/restartDMA çiftini yalnızca IPU'ya kendi verisini gönderirken kullanıyor (ölçüldü), native yolda gereksiz.
   - **Başlık:** w/h/frameCount/flags. Ölçülen desen: pts += 3003, flags = 0x1D8 | tip; oyun pts'i okumuyor.
   - **`sceMpegIsEnd` / `sceMpegReset` native.**
   - **Sonuç:** native modda oynatma sırasında IPU'ya hiç BDEC/CSC komutu gitmiyor; yalnızca kütüphane başlatılırken SETIQ/SETVQ/SETTH yazılıyor. FMV doğru görüntüyle oynuyor (3000., 4500., 6000. kareler).
   - **Açık sorun (önceden de var):** FMV gerçek IPU ile de yaklaşık 16 resim/s oynuyor (29,97 olmalı). Görüntüleme callback'i `0x2B5830` GS yolu meşgulken (`sceGsSyncPath(1,0)`) VSync'i atlıyor; zamanlama incelenecek. Native yol yaklaşık 14 resim/s.

3. **VU1:** Mikroprogramları (VIF MPG ile yüklenen) EE gibi statik veya çalışma anında C'ye çevir.
4. **GS:** Kendi GPU renderer'ımız (D3D11 veya Vulkan). En büyük iş.
5. **Ses:** SPU2 (ADX ve MIDI sürücüleri IOP kodu olduğundan IOP interpreter + SPU2 karıştırıcı gerekiyor).
6. PCSX2 bağımlılığını kaldır.
