# xBloom Panel

شاشة لمس لمكينة **xBloom Studio**، مبنية على شاشة Elecrow CrowPanel Advance 4.3″.
A touchscreen controller for the **xBloom Studio**, built on the Elecrow CrowPanel Advance 4.3″.

> مشروع شخصي غير رسمي، وما له علاقة بشركة xBloom. يستخدم واجهة xBloom غير الرسمية (من مشروع xbloom-py)، وممكن تتغير في أي وقت.
> Unofficial personal project, not affiliated with xBloom. It uses xBloom's unofficial API, which may change at any time.

---

## العربي

### وش تحتاج
| | |
|---|---|
| الشاشة | **Elecrow CrowPanel Advance 4.3″ HMI** (ESP32-S3، 800×480 IPS). لازم «Advance»، مو الإصدار العادي 480×272. ما تحتاج أي موديول إضافي. |
| كيبل | USB-C للتثبيت والتشغيل، ويفضّل يكون بزاوية ٩٠° عشان يختفي خلف الشاشة |
| الكيس (اختياري) | مشروع Bambu Studio جاهز في مجلد `case/` (`Xbloom_screen_case.3mf`، مضبوط على P2S). يفتح في Bambu Studio و OrcaSlicer و PrusaSlicer. اطبعه ABS أو ASA أو PETG |

**رابط الشراء:** [AliExpress](https://a.aliexpress.com/_c3srIHtH) · أو [متجر Elecrow الرسمي](https://www.elecrow.com/crowpanel-advance-4-3-hmi-esp32-800x480-ai-display-ips-touch-artificial-intelligent-screen.html)

### التثبيت (بضغطة زر)
1. وصّل الشاشة بالكمبيوتر بكيبل USB-C.
2. افتح صفحة التثبيت في Chrome أو Edge: **https://mesh-one.github.io/Xbloom-panel/**
3. اضغط **تثبيت**، واختر منفذ الشاشة، ووافق على المسح، وانتظر حتى ١٠٠٪.

### الإعداد
1. **الإعدادات ← Wi-Fi**: اختر شبكتك واكتب الرمز.
2. **الإعدادات ← حساب xBloom**: سجّل بإيميل وباسورد تطبيق xBloom. وصفاتك تنزل على الشاشة.
3. شغّل المكينة، والشاشة تتصل فيها بالبلوتوث لحالها.

### المميزات
- وصفاتك (اللي سويتها واللي حفظتها) من حساب xBloom، والتعديل من الشاشة يرجع للتطبيق
- شاشة صب متحركة تعرض كل صبّة: الحرارة، نوع الصب، الموية، الوزن، والوقت
- شاشة «Enjoy» بالنتيجة لما تخلص القهوة
- تحكم بالمطحنة، والصب اليدوي، والميزان مع ساعة توقيت
- تعديل الوصفات وإنشاء وصفات جديدة من الشاشة
- ساعة نقطية لما تكون خاملة (تختار لونها: أبيض، أزرق، بني)، وتختار متى تنطفي الشاشة (١، ٣، ٥، ١٠ دقايق أو أبداً)، وترجع للرئيسية بعد ٣٠ ثانية
- زر «مزامنة الوصفات» في الإعدادات
- تحديثات عن طريق الواي فاي (الإعدادات ← تحديث البرنامج)

### التحديث
الإعدادات ← تحديث البرنامج، افتح الرابط اللي يطلع لك من جوالك، وارفع ملف `xbloom-panel-…-update.bin` من مجلد `firmware/`.

---

## English

**You need:** an Elecrow CrowPanel Advance 4.3″ HMI ([AliExpress](https://a.aliexpress.com/_c3srIHtH) or [Elecrow store](https://www.elecrow.com/crowpanel-advance-4-3-hmi-esp32-800x480-ai-display-ips-touch-artificial-intelligent-screen.html)) (ESP32-S3, 800×480 IPS; the "Advance" model, not the 480×272 one) and a USB-C cable.

**Install:** plug the panel into a computer, open **https://mesh-one.github.io/Xbloom-panel/** in Chrome or Edge, press **Install**, pick the panel's port, allow the erase, wait for 100%.

**Set up:** Settings → Wi-Fi, then Settings → xBloom account (your xBloom app email and password). Turn on the machine; the panel connects over Bluetooth by itself.

**Features:** recipe sync with your xBloom account (both ways, plus a Sync button in Settings), live animated brewing screen, finish screen, grinder / manual pour / scale controls, recipe editor, dot-matrix idle clock (white / blue / brown), adjustable sleep time, auto-dim, over-the-air updates.

**Case:** a ready Bambu Studio project in `case/Xbloom_screen_case.3mf` (set up for the P2S; also opens in OrcaSlicer and PrusaSlicer). The screen stands in the groove behind the brewer tower.

**Build from source:** PlatformIO (`pio run`). The first build compiles ESP-IDF with the settings in `platformio.ini` (`custom_sdkconfig`), which takes a while.

## Files
- `docs/` – the one-click installer page (GitHub Pages) and the full firmware image
- `firmware/` – full image (USB, address 0x0) and the update image (Wi-Fi update page)
- `case/` – Bambu Studio project (3MF) for the case
- `src/`, `include/` – firmware source; `sim/` – desktop preview of the UI

Licence: MIT (see `LICENSE`). Third-party code: see `THIRD_PARTY_NOTICES.md`.
