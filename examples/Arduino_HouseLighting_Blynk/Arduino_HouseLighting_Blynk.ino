/*
  Arduino_HouseLighting_Blynk — подсветка дома, 4 платы × 8 каналов, управление через Blynk по GSM.

  Алгоритм:
  1. Старт: все каналы 0 %, настройки из флеш, модем включается и регистрируется в сети.
  2. Время и часовой пояс — с GSM-вышки (NITZ, AT+CCLK?), сверка раз в 6 часов.
  3. Раз в сутки по координатам считаются рассвет и закат (sun.h).
  4. Режимы:
       АВТО   — темно (закат+смещение .. рассвет+смещение): каналы горят со своей яркостью,
                светло: 0 %;
       РУЧНОЙ — каналы горят со своей яркостью всегда.
     Выключатель "Всё выкл" гасит всё в любом режиме, пока его не включат обратно.
  5. Яркость каналов общая для обоих режимов и запоминается. Ползунок "Все" ставит всем одно значение.
  6. Изменения яркости плавные (FADE), с гамма-коррекцией.
  7. Связь: GPRS -> Blynk. Свет от связи не зависит: при потере интернета работает по часам платы.

  Экономия сообщений Blynk (главное отличие от "наивной" прошивки):
  - значение отправляется в Blynk ТОЛЬКО если оно изменилось с прошлой отправки;
  - то, что пришло из приложения, обратно не отправляется (приложение его уже показывает);
  - текущая яркость во время плавного перехода НЕ отправляется — ползунки показывают заданную;
  - напряжения и сигнал — раз в час и только при заметном изменении;
  - при переподключении нет syncAll — досылаются только изменившиеся значения.
  Итого порядка 1-3 тыс. сообщений в месяц на плату вместо сотен тысяч.
*/

// ===== Номер платы (угла) 1..4 — для каждой платы свой =====
#define DEVICE_ID   1

// ===== Модель платы: выбирается по номеру =====
// Плата №3 — с контроллером питания IP5306, остальные — с AXP192.
// Другие варианты: SIM800L_IP5306_VERSION_20190610, SIM800C_AXP192_VERSION_20200609
#if DEVICE_ID == 3
#define SIM800L_IP5306_VERSION_20200811
#else
#define SIM800L_AXP192_VERSION_20200327
#endif

// ===== Ключи Blynk, APN и координаты — в secrets.h =====
// Скопируйте secrets.example.h в secrets.h и заполните. secrets.h не попадает в git.
#include "secrets.h"

#define BLYNK_PRINT Serial
#define BLYNK_HEARTBEAT 120                 // реже пинги — меньше трафика (пинги не считаются сообщениями)

#define TINY_GSM_MODEM_SIM800
#define TINY_GSM_RX_BUFFER 1024

#include "utilities.h"
#include "sun.h"

#include <TinyGsmClient.h>
#include <BlynkSimpleTinyGSM.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <sys/time.h>
#include <time.h>

#define SerialMon Serial
#define SerialAT  Serial1

TinyGsm modem(SerialAT);

// ===== Виртуальные пины Blynk (подстройте под свой шаблон) =====
#define VP_MODE        V0   // 0 = РУЧНОЙ, 1 = АВТО                       (switch)
#define VP_CH1         V1   // V1..V8 — яркость каналов 0..100             (slider)
#define VP_ALL         V9   // все каналы сразу 0..100                     (slider)
#define VP_ALL_OFF     V10  // 1 = всё выключено, 0 = работа               (switch)
#define VP_FADE        V11  // время плавного перехода, с                  (step / slider)
#define VP_SUNSET_OFS  V12  // включение = закат + N мин                   (step)
#define VP_SUNRISE_OFS V13  // выключение = рассвет + N мин                (step)
#define VP_STATUS      V14  // текст состояния                             (label)
#define VP_SUN         V15  // "рассвет / закат"                           (label)
#define VP_BATT        V16  // напряжение аккумулятора, В                  (label)
#define VP_VBUS        V17  // напряжение USB, В                           (label)
#define VP_ACIN        V18  // напряжение входа 5V, В                      (label)
#define VP_CSQ         V19  // уровень сигнала GSM 0..31                   (label)
#define VP_RESET       V20  // причина последней перезагрузки             (label)
#define VP_BATT_PCT    V21  // заряд аккумулятора, %                       (label)
#define VP_COUNT       22

// ===== Выходы на 8-канальный MOSFET-модуль =====
#define NCH 8
#if defined(SIM800C_AXP192_VERSION_20200609)
static const uint8_t LIGHT_PINS[NCH] = {18, 19, 14, 15, 2, 13, 32, 12};
#elif defined(SIM800L_IP5306_VERSION_20190610)
static const uint8_t LIGHT_PINS[NCH] = {18, 19, 25, 14, 13, 15, 32, 33};
#else
static const uint8_t LIGHT_PINS[NCH] = {18, 19, 25, 14, 13, 15, 2, 32};  // SIM800L AXP192 / IP5306 20200811
#endif
#define PWM_FREQ    1000
#define PWM_BITS    12
#define PWM_MAX     4095
#define PWM_INVERT  0
#define GAMMA       2.2f

#define TELEMETRY_MS   (60UL * 60 * 1000)   // сигнал GSM — раз в час
#define VOLTS_MS       (60UL * 1000)        // напряжения — раз в минуту, если изменились от 0.1 В
#define TIME_SYNC_MS   (6UL * 60 * 60 * 1000)
#define NET_RETRY_MS   30000UL
#define NET_REBOOT_MS  (15UL * 60 * 1000)   // нет связи 15 мин — перезапуск модема
#define AT_FAIL_MAX    3                    // модем 3 раза подряд молчит на AT — выключить ему питание

// ===== Защита от зависаний и разряда аккумулятора =====
#define WDT_TIMEOUT_S  300                  // цикл завис на 5 мин — перезагрузка платы (GPRS бывает долгим)
#define POWER_CHECK_MS 2000
#define BATT_LOW_V     3.50f                // без внешнего питания ниже — модем выключаем
#define BATT_OK_V      3.70f                // выше — модем снова включаем
#define BATT_DEEP_V    3.60f                // был ниже, а потом вернулось 5 В — полная перезагрузка
#define BATT_DEEP_PCT  25                   // то же для IP5306: он сообщает только уровень 100/75/50/25/0 %
#define BATT_LOW_PCT   0
#define BATT_OK_PCT    50
#define BATT_SAMPLES   3                    // столько замеров подряд (6 с), чтобы не ловить просадку от модема
#define BATT_LOG_MS    (5UL * 60 * 1000)    // напряжение АКБ в журнал не чаще раза в 5 мин

#if defined(SIM800L_AXP192_VERSION_20200327) || defined(SIM800C_AXP192_VERSION_20200609)
#define HAS_AXP192
#endif

// ===== Сохраняемые настройки =====
struct Settings {
    uint8_t  version;
    uint8_t  autoMode;          // 1 = АВТО
    uint8_t  allOff;            // 1 = всё выключено
    uint8_t  level[NCH];        // яркость каналов
    uint16_t fadeSec;
    int16_t  sunsetOfs;
    int16_t  sunriseOfs;
};
#define SETTINGS_VERSION 1

Settings    st;
Preferences prefs;
bool        stDirty   = false;
uint32_t    stDirtyAt = 0;

uint8_t  targetLvl[NCH] = {0};
float    curLvl[NCH]    = {0};

bool     timeOk     = false;
time_t   sunriseT = 0, sunsetT = 0, onT = 0, offT = 0;
int      polar      = 0;
int      lastSunDay = -1;
bool     isDark     = false;

float    battV = 0, vbusV = 0, acinV = 0;
int      battPct = -1;                      // заряд АКБ, %; -1 — ещё не измерен
int      csq   = -1;

bool     modemOn    = false;
String   resetText;                         // причина последней перезагрузки (для лога и V20)

// Состояние питания для защит. На AXP192 — напряжение АКБ, на IP5306 — уровень заряда 0..100 %
struct PowerState {
    bool   valid;                           // прочитали контроллер
    bool   ext;                             // есть внешнее питание (USB или вход 5V)
    bool   deep, low, ok;                   // АКБ: глубокий разряд / модем выключить / модем можно включить
    String text;                            // для журнала: "3.58 В" или "25 %"
};

// Последнее отправленное в Blynk значение по каждому пину
String   sentVal[VP_COUNT];
bool     sentValid[VP_COUNT] = {false};

// ---------------------------------------------------------------------------
// Настройки
// ---------------------------------------------------------------------------

void loadSettings()
{
    prefs.begin("light", false);
    if (prefs.getBytesLength("st") != sizeof(st) ||
            prefs.getBytes("st", &st, sizeof(st)) != sizeof(st) ||
            st.version != SETTINGS_VERSION) {
        memset(&st, 0, sizeof(st));
        st.version  = SETTINGS_VERSION;
        st.autoMode = 1;
        st.fadeSec  = 3;
        for (int i = 0; i < NCH; i++) {
            st.level[i] = 100;
        }
    }
}

// С задержкой, чтобы не писать во флеш на каждое движение ползунка
void markDirty()
{
    stDirty   = true;
    stDirtyAt = millis();
}

void saveIfDirty()
{
    if (stDirty && millis() - stDirtyAt > 5000) {
        prefs.putBytes("st", &st, sizeof(st));
        stDirty = false;
    }
}

// ---------------------------------------------------------------------------
// Сторожевой таймер и журнал перезагрузок
// ---------------------------------------------------------------------------

void wdtBegin()
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t cfg = {
        .timeout_ms     = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&cfg);
#else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
    esp_task_wdt_add(NULL);
}

// delay(), который не даёт сработать сторожевому таймеру
void wdtDelay(uint32_t ms)
{
    while (ms) {
        uint32_t d = ms > 1000 ? 1000 : ms;
        delay(d);
        ms -= d;
        esp_task_wdt_reset();
    }
}

// Причина перезагрузки, которую плата записала сама перед ESP.restart()
void saveEvent(const char *ev)
{
    prefs.putString("ev", ev);
}

// Перед намеренной перезагрузкой: сохранить настройки и причину
void restartWith(const char *ev)
{
    SerialMon.printf("Перезагрузка: %s\n", ev);
    if (stDirty) {
        prefs.putBytes("st", &st, sizeof(st));
    }
    saveEvent(ev);
    delay(200);
    ESP.restart();
}

const char *resetReasonText(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "включение питания";
    case ESP_RST_BROWNOUT:  return "просадка питания (brownout)";
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:       return "зависание (watchdog)";
    case ESP_RST_PANIC:     return "программный сбой";
    case ESP_RST_SW:        return "программная перезагрузка";
    case ESP_RST_EXT:       return "кнопка RST";
    case ESP_RST_DEEPSLEEP: return "выход из сна";
    default:                return "неизвестно";
    }
}

// Читает причину прошлой перезагрузки и последнее напряжение АКБ, сохранённое до неё
void loadResetLog()
{
    String ev    = prefs.getString("ev", "");
    String lastP = prefs.getString("lastP", "");
    prefs.remove("ev");

    resetText = resetReasonText(esp_reset_reason());
    if (ev.length()) {
        resetText += ": " + ev;
    }
    if (lastP.length()) {
        resetText += ", АКБ до этого " + lastP;
    }
    SerialMon.println("Причина перезагрузки: " + resetText);
}

// ---------------------------------------------------------------------------
// ШИМ
// ---------------------------------------------------------------------------

uint32_t levelToDuty(float pct)
{
    uint32_t duty = 0;
    if (pct > 0) {
        duty = (uint32_t)lroundf(powf(pct / 100.0f, GAMMA) * PWM_MAX);
        if (duty == 0) {
            duty = 1;
        }
    }
    return PWM_INVERT ? PWM_MAX - duty : duty;
}

void pwmWrite(int i, float pct)
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(LIGHT_PINS[i], levelToDuty(pct));
#else
    ledcWrite(i, levelToDuty(pct));
#endif
}

void pwmBegin()
{
    for (int i = 0; i < NCH; i++) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcAttach(LIGHT_PINS[i], PWM_FREQ, PWM_BITS);
#else
        ledcSetup(i, PWM_FREQ, PWM_BITS);
        ledcAttachPin(LIGHT_PINS[i], i);
#endif
        pwmWrite(i, 0);
    }
}

void fadeStep()
{
    static uint32_t last = millis();
    uint32_t now = millis();
    if (now - last < 10) {
        return;
    }
    float step = st.fadeSec ? 100.0f * (now - last) / (st.fadeSec * 1000.0f) : 100.0f;
    last = now;
    for (int i = 0; i < NCH; i++) {
        float diff = targetLvl[i] - curLvl[i];
        if (diff == 0) {
            continue;
        }
        curLvl[i] = fabsf(diff) <= step ? targetLvl[i] : curLvl[i] + (diff > 0 ? step : -step);
        pwmWrite(i, curLvl[i]);
    }
}

// ---------------------------------------------------------------------------
// Время с GSM-вышки
// ---------------------------------------------------------------------------

// Включает приём времени от оператора (NITZ). Нужно один раз, потом настройка хранится в модеме.
void enableNetworkTime()
{
    // Светодиод NETLIGHT мог быть выключен прежней прошивкой — включаем всегда
    modem.sendAT(GF("+CNETLIGHT=1"));
    modem.waitResponse();

    modem.sendAT(GF("+CLTS?"));
    if (modem.waitResponse(2000L, GF("+CLTS: 1")) == 1) {
        modem.waitResponse();
        return;
    }
    modem.waitResponse();
    SerialMon.println("Включаю NITZ (AT+CLTS=1) и перезапускаю модем");
    modem.sendAT(GF("+CLTS=1"));
    modem.waitResponse();
    modem.sendAT(GF("&W"));
    modem.waitResponse();
    modem.restart();
}

bool syncTimeFromGsm()
{
    int   y, mo, d, h, mi, s;
    float tz;
    if (!modem.getNetworkTime(&y, &mo, &d, &h, &mi, &s, &tz) || y < 2024) {
        SerialMon.println("Время от оператора ещё не получено");
        return false;
    }
    int  tzMin = (int)lroundf(tz * 60);
    // POSIX TZ: знак наоборот ("GSM-3:00" = UTC+3)
    char tzs[20];
    snprintf(tzs, sizeof(tzs), "GSM%c%d:%02d", tzMin >= 0 ? '-' : '+', abs(tzMin) / 60, abs(tzMin) % 60);
    setenv("TZ", tzs, 1);
    tzset();

    time_t  local = (time_t)daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    timeval tv    = {local - tzMin * 60, 0};
    settimeofday(&tv, nullptr);
    timeOk     = true;
    lastSunDay = -1;
    SerialMon.printf("Время с вышки: %04d-%02d-%02d %02d:%02d:%02d, пояс %+.2f\n", y, mo, d, h, mi, s, tz);
    return true;
}

// ---------------------------------------------------------------------------
// Питание модема и аккумулятор
// ---------------------------------------------------------------------------

uint32_t lastOnline = 0;

// Включить питание модема, нажать PWRKEY и проинициализировать
void modemStart()
{
    digitalWrite(MODEM_POWER_ON, HIGH);
    digitalWrite(MODEM_PWRKEY, HIGH);
    delay(100);
    digitalWrite(MODEM_PWRKEY, LOW);
    delay(1000);
    digitalWrite(MODEM_PWRKEY, HIGH);
    modemOn = true;
    wdtDelay(3000);
    if (!modem.restart()) {
        SerialMon.println("Модем не ответил после включения");
    }
    esp_task_wdt_reset();
    enableNetworkTime();
    lastOnline = millis();
}

void modemStop()
{
    if (Blynk.connected()) {
        Blynk.disconnect();
    }
    digitalWrite(MODEM_POWER_ON, LOW);
    modemOn = false;
}

// Жёсткий перезапуск: зависший модем не слышит AT-команд, поэтому снимаем с него питание
void modemPowerCycle()
{
    SerialMon.println("Модем не отвечает — выключаю его питание на 3 с");
    digitalWrite(MODEM_POWER_ON, LOW);
    modemOn = false;
    wdtDelay(3000);
    modemStart();
}

uint8_t i2cRead(uint8_t addr, uint8_t reg, bool &ok)
{
    Wire.beginTransmission(addr);
    Wire.write(reg);
    ok = Wire.endTransmission(false) == 0 && Wire.requestFrom(addr, (uint8_t)1) == 1;
    return ok ? Wire.read() : 0;
}

PowerState readPower()
{
    PowerState p = {false, true, false, false, false, ""};
#if defined(HAS_AXP192)
    // AXP192, регистр 0x00: бит 7 — ACIN (вход 5V), бит 5 — VBUS (USB)
    bool  ok;
    uint8_t st0 = i2cRead((uint8_t)AXP192_SLAVE_ADDRESS, 0x00, ok);
    float b     = axp.getBattVoltage() / 1000.0f;
    if (!ok || !axp.isBatteryConnect() || b < 2.5f) {
        return p;                           // нет связи с AXP или нет аккумулятора — защищать нечего
    }
    p.valid = true;
    p.ext   = st0 & 0xA0;
    p.deep  = b < BATT_DEEP_V;
    p.low   = b < BATT_LOW_V;
    p.ok    = b > BATT_OK_V;
    p.text  = String(b, 2) + " В";
#elif defined(IP5306_ADDR)
    // IP5306: 0x70 бит 3 — подключено внешнее питание; 0x78 биты 7..4 — уровень заряда
    bool ok1, ok2;
    uint8_t r70 = i2cRead(IP5306_ADDR, 0x70, ok1);
    uint8_t r78 = i2cRead(IP5306_ADDR, 0x78, ok2) & 0xF0;
    if (!ok1 || !ok2) {
        return p;
    }
    int lvl = r78 == 0x00 ? 100 : r78 == 0x80 ? 75 : r78 == 0xC0 ? 50 : r78 == 0xE0 ? 25 : 0;
    p.valid = true;
    p.ext   = r70 & 0x08;
    p.deep  = lvl <= BATT_DEEP_PCT;
    p.low   = lvl <= BATT_LOW_PCT;
    p.ok    = lvl >= BATT_OK_PCT;
    p.text  = String(lvl) + " %";
#endif
    return p;
}

// Раз в 2 с: журнал заряда, защита модема при низком АКБ, перезагрузка после возврата 5 В
void powerLoop()
{
    static uint32_t last = 0;
    if (millis() - last < POWER_CHECK_MS) {
        return;
    }
    last = millis();

    readVoltages();
    PowerState p = readPower();
    if (!p.valid) {
        return;
    }

    // Журнал: последнее значение до перезагрузки, не чаще раза в 5 мин
    static uint32_t lastLog = 0;
    static String   logged;
    if (p.text != logged && (logged.length() == 0 || millis() - lastLog >= BATT_LOG_MS)) {
        prefs.putString("lastP", p.text);
        logged  = p.text;
        lastLog = millis();
    }

    // Замеры подряд, чтобы короткая просадка от модема не считалась разрядом
    static uint8_t deepCnt = 0, lowCnt = 0, okCnt = 0;
    static bool    deepSeen = false;
    auto count = [](uint8_t &c, bool cond) {
        c = cond ? (c < BATT_SAMPLES ? c + 1 : c) : 0;
    };
    count(deepCnt, !p.ext && p.deep);
    count(lowCnt,  !p.ext && p.low);
    count(okCnt,   p.ext || p.ok);

    if (deepCnt >= BATT_SAMPLES) {
        deepSeen = true;
    }
    if (deepSeen && p.ext) {
        restartWith("вернулось внешнее питание после разряда АКБ");
    }
    if (modemOn && lowCnt >= BATT_SAMPLES) {
        SerialMon.println("АКБ " + p.text + " без внешнего питания — выключаю модем, свет работает по часам");
        modemStop();
    } else if (!modemOn && okCnt >= BATT_SAMPLES) {
        SerialMon.println("Питание в норме — включаю модем");
        modemStart();
    }
}

// ---------------------------------------------------------------------------
// Расписание
// ---------------------------------------------------------------------------

String fmtTime(time_t t)
{
    if (t == 0) {
        return "--:--";
    }
    struct tm lt;
    localtime_r(&t, &lt);
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &lt);
    return buf;
}

void updateSun(time_t now)
{
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_yday == lastSunDay) {
        return;
    }
    lastSunDay = lt.tm_yday;
    int    y = lt.tm_year + 1900, m = lt.tm_mon + 1, d = lt.tm_mday;
    double rise = 0, set = 0;
    polar = sunCalc(y, m, d, HOME_LAT, HOME_LON, rise, set);
    if (polar == 0) {
        time_t base = (time_t)daysFromCivil(y, m, d) * 86400;
        sunriseT = base + (time_t)lround(rise * 60);
        sunsetT  = base + (time_t)lround(set * 60);
    } else {
        sunriseT = sunsetT = 0;
    }
    onT  = sunsetT ? sunsetT + st.sunsetOfs * 60 : 0;
    offT = sunriseT ? sunriseT + st.sunriseOfs * 60 : 0;
    SerialMon.printf("Рассвет %s, закат %s\n", fmtTime(sunriseT).c_str(), fmtTime(sunsetT).c_str());
}

void computeTargets()
{
    if (timeOk) {
        time_t now = time(nullptr);
        updateSun(now);
        isDark = polar < 0 || (polar == 0 && (now < offT || now >= onT));
    }
    bool on = !st.allOff && (!st.autoMode || (timeOk && isDark));
    for (int i = 0; i < NCH; i++) {
        targetLvl[i] = on ? st.level[i] : 0;
    }
}

String statusText()
{
    if (st.allOff) {
        return "ВСЁ ВЫКЛ";
    }
    if (!st.autoMode) {
        return "РУЧНОЙ: ВКЛ";
    }
    if (!timeOk) {
        return "АВТО: нет времени";
    }
    return isDark ? "АВТО: ТЕМНО, ВКЛ" : "АВТО: СВЕТЛО, ВЫКЛ";
}

// ---------------------------------------------------------------------------
// Blynk: отправка только изменившихся значений
// ---------------------------------------------------------------------------

// Запомнить значение как уже известное приложению (пришло из приложения)
void markSent(int pin, const String &val)
{
    sentVal[pin]   = val;
    sentValid[pin] = true;
}

void publish(int pin, const String &val)
{
    if (sentValid[pin] && sentVal[pin] == val) {
        return;
    }
    if (!Blynk.connected()) {
        return;                         // отправим, когда появится связь
    }
    Blynk.virtualWrite(pin, val);
    markSent(pin, val);
}

void publishState()
{
    publish(VP_MODE, String(st.autoMode));
    publish(VP_ALL_OFF, String(st.allOff));
    for (int i = 0; i < NCH; i++) {
        publish(VP_CH1 + i, String(st.level[i]));
    }
    publish(VP_FADE, String(st.fadeSec));
    publish(VP_SUNSET_OFS, String(st.sunsetOfs));
    publish(VP_SUNRISE_OFS, String(st.sunriseOfs));
    publish(VP_STATUS, statusText());
    if (timeOk) {
        publish(VP_SUN, polar ? String(polar < 0 ? "полярная ночь" : "полярный день")
                : fmtTime(sunriseT) + " / " + fmtTime(sunsetT));
    }
#ifdef HAS_AXP192
    // 0.00 тоже отправляем: так видно, что внешнее питание пропало
    publish(VP_BATT, String(battV, 1));
    publish(VP_VBUS, String(vbusV, 1));
    publish(VP_ACIN, String(acinV, 1));
#else
    // IP5306 напряжений не измеряет: в USB V только «есть 5 В / нет» (5.0 / 0.0)
    if (battPct >= 0) {
        publish(VP_VBUS, String(vbusV, 1));
    }
#endif
    if (battPct >= 0) {
        publish(VP_BATT_PCT, String(battPct));
    }
    if (csq >= 0)  publish(VP_CSQ, String(csq));
    publish(VP_RESET, resetText);           // меняется только при перезагрузке
}

// Заряд Li-ion по напряжению, % (примерно; во время зарядки показывает больше)
int voltsToPct(float v)
{
    static const float tbl[][2] = {
        {3.30f, 0}, {3.50f, 10}, {3.60f, 20}, {3.70f, 40}, {3.80f, 60},
        {3.90f, 75}, {4.00f, 85}, {4.10f, 95}, {4.20f, 100},
    };
    const int n = sizeof(tbl) / sizeof(tbl[0]);
    if (v <= tbl[0][0]) {
        return 0;
    }
    for (int i = 1; i < n; i++) {
        if (v <= tbl[i][0]) {
            float k = (v - tbl[i - 1][0]) / (tbl[i][0] - tbl[i - 1][0]);
            int   p = (int)lroundf(tbl[i - 1][1] + k * (tbl[i][1] - tbl[i - 1][1]));
            return (p + 2) / 5 * 5;         // шаг 5 %, чтобы не дёргалось
        }
    }
    return 100;
}

// Питание: замер каждые 2 с из powerLoop(). Сглаживаем, чтобы просадки от модема
// не уходили в Blynk; показание обновляем раз в минуту и только при заметном изменении.
void readVoltages()
{
    static uint32_t lastShow = 0;
#ifdef HAS_AXP192
    static float b = -1, u = 0, a = 0;
    float nb = axp.getBattVoltage() / 1000.0f;
    float nu = axp.getVbusVoltage() / 1000.0f;
    float na = axp.getAcinVoltage() / 1000.0f;
    if (b < 0) {
        b = nb, u = nu, a = na;             // первый замер — без сглаживания
    }
    b += (nb - b) * 0.3f;
    u  = nu < 1.0f ? 0 : u + (nu - u) * 0.5f;   // питание пропало — сразу 0, без «хвоста»
    a  = na < 1.0f ? 0 : a + (na - a) * 0.5f;
    if (lastShow && millis() - lastShow < VOLTS_MS) {
        return;                             // показания обновляем раз в минуту
    }
    lastShow = millis();
    auto upd = [](float &shown, float v) {
        if (fabsf(v - shown) >= 0.1f) {
            shown = v;
        }
    };
    upd(battV, b);
    upd(vbusV, u);
    upd(acinV, a);
    int pct = (axp.isBatteryConnect() && b > 2.5f) ? voltsToPct(b) : 0;
    if (battPct < 0 || abs(pct - battPct) >= 5) {
        battPct = pct;
    }
#elif defined(IP5306_ADDR)
    if (lastShow && millis() - lastShow < VOLTS_MS) {
        return;
    }
    PowerState p = readPower();
    if (!p.valid) {
        return;                             // IP5306 без I2C — показать нечего
    }
    lastShow = millis();
    battPct  = p.text.toInt();              // readPower() для IP5306 отдаёт "75 %"
    vbusV    = p.ext ? 5.0f : 0.0f;
#endif
}

// Сигнал GSM: раз в час, и только при заметном изменении
void readTelemetry()
{
    if (!modemOn) {
        return;
    }
    int q = modem.getSignalQuality();
    if (q != 99 && abs(q - csq) >= 3) {
        csq = q;
    }
}

BLYNK_CONNECTED()
{
    // Без syncAll: плата — источник правды. Досылаем только то, что изменилось
    // (после перезагрузки платы — один раз всё, ~20 сообщений).
    SerialMon.println("Blynk подключен");
    publishState();
}

// Все записи из приложения
BLYNK_WRITE_DEFAULT()
{
    int    pin = request.pin;
    int    v   = param.asInt();
    String s   = param.asStr();

    if (pin >= VP_CH1 && pin < VP_CH1 + NCH) {
        st.level[pin - VP_CH1] = constrain(v, 0, 100);
    } else if (pin == VP_ALL) {
        for (int i = 0; i < NCH; i++) {
            st.level[i] = constrain(v, 0, 100);   // V1..V8 обновятся в приложении через publishState()
        }
    } else if (pin == VP_MODE) {
        st.autoMode = v ? 1 : 0;
    } else if (pin == VP_ALL_OFF) {
        st.allOff = v ? 1 : 0;
    } else if (pin == VP_FADE) {
        st.fadeSec = constrain(v, 0, 600);
    } else if (pin == VP_SUNSET_OFS) {
        st.sunsetOfs = constrain(v, -240, 240);
        lastSunDay   = -1;
    } else if (pin == VP_SUNRISE_OFS) {
        st.sunriseOfs = constrain(v, -240, 240);
        lastSunDay    = -1;
    } else {
        return;
    }
    if (pin < VP_COUNT) {
        markSent(pin, s);                   // приложение это значение уже показывает
    }
    markDirty();
    computeTargets();
    publishState();
}

// ---------------------------------------------------------------------------
// Связь: сеть -> GPRS -> Blynk, без блокировки света
// ---------------------------------------------------------------------------

void netLoop()
{
    if (!modemOn) {
        return;                             // модем выключен из-за низкого АКБ
    }
    if (Blynk.connected()) {
        Blynk.run();
        lastOnline = millis();
        return;
    }
    static uint32_t lastTry = 0;
    if (lastTry && millis() - lastTry < NET_RETRY_MS) {
        return;
    }
    lastTry = millis();

    // Модем молчит несколько попыток подряд — завис, перезапускаем по питанию
    static uint8_t atFails = 0;
    if (!modem.testAT(2000)) {
        if (++atFails >= AT_FAIL_MAX) {
            atFails = 0;
            modemPowerCycle();
        }
        return;
    }
    atFails = 0;

    if (millis() - lastOnline > NET_REBOOT_MS) {
        SerialMon.println("Долго нет связи — перезапуск модема по питанию");
        modemPowerCycle();
        return;
    }
    if (!modem.isNetworkConnected()) {
        SerialMon.println("Ожидание сети GSM...");
        modem.waitForNetwork(10000L);
        return;
    }
    if (!timeOk) {
        syncTimeFromGsm();
    }
    if (!modem.isGprsConnected()) {
        SerialMon.println("Подключение GPRS...");
        modem.gprsConnect(APN, GPRS_USER, GPRS_PASS);
        return;
    }
    SerialMon.println("Подключение к Blynk...");
    Blynk.connect(10000);
}

// ---------------------------------------------------------------------------

void setup()
{
    SerialMon.begin(115200);
    delay(10);
    wdtBegin();
    loadSettings();
    loadResetLog();

    setupModem();                           // PMU, питание и PWRKEY модема (utilities.h)
    modemOn = true;
#ifdef HAS_AXP192
    axp.adc1Enable(AXP202_ACIN_VOL_ADC1, true);
#endif
    pwmBegin();                             // после setupModem: он перенастраивает GPIO светодиода (канал 5)

    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    wdtDelay(3000);
    SerialMon.println("Запуск модема...");
    if (modem.restart()) {
        enableNetworkTime();
    } else {
        modemPowerCycle();
    }
    esp_task_wdt_reset();

    // Время нужно для расписания — ждём сеть до минуты, дальше пробуем в фоне
    if (modem.waitForNetwork(60000L)) {
        esp_task_wdt_reset();
        for (int i = 0; i < 6 && !syncTimeFromGsm(); i++) {
            wdtDelay(5000);
        }
    }
    esp_task_wdt_reset();

    Blynk.config(modem, AUTH_TOKENS[DEVICE_ID - 1]);
    lastOnline = millis();
    computeTargets();
    readVoltages();
    readTelemetry();
}

void loop()
{
    esp_task_wdt_reset();
    powerLoop();
    netLoop();

    static uint32_t lastCalc = 0;
    if (millis() - lastCalc >= 1000) {
        lastCalc = millis();
        computeTargets();
        publishState();                     // уходит только то, что изменилось
    }

    static uint32_t lastTelemetry = 0;
    if (millis() - lastTelemetry >= TELEMETRY_MS) {
        lastTelemetry = millis();
        readTelemetry();
    }

    static uint32_t lastTimeSync = 0;
    if (modemOn && (millis() - lastTimeSync >= TIME_SYNC_MS || (!timeOk && millis() - lastTimeSync >= NET_RETRY_MS))) {
        lastTimeSync = millis();
        syncTimeFromGsm();
    }

    fadeStep();
    saveIfDirty();
    delay(2);
}
