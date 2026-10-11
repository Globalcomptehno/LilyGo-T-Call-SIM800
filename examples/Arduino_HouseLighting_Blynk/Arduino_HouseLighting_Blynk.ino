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

// ===== Выберите свою плату =====
// #define SIM800L_IP5306_VERSION_20190610
#define SIM800L_AXP192_VERSION_20200327
// #define SIM800C_AXP192_VERSION_20200609
// #define SIM800L_IP5306_VERSION_20200811

// ===== Номер платы (угла) 1..4 — для каждой платы свой =====
#define DEVICE_ID   1

// ===== Blynk: шаблон и токены 4 устройств (Blynk.Console -> Devices -> Device Info) =====
#define BLYNK_TEMPLATE_ID   "TMPLxxxxxxxx"
#define BLYNK_TEMPLATE_NAME "House Light"
static const char *AUTH_TOKENS[4] = {
    "TOKEN_DEVICE_1",
    "TOKEN_DEVICE_2",
    "TOKEN_DEVICE_3",
    "TOKEN_DEVICE_4",
};

// ===== GPRS: APN вашего оператора =====
const char APN[]       = "internet";
const char GPRS_USER[] = "";
const char GPRS_PASS[] = "";

// ===== Координаты дома (одинаковые для всех 4 плат) =====
#define HOME_LAT    55.7558
#define HOME_LON    37.6173

#define BLYNK_PRINT Serial
#define BLYNK_HEARTBEAT 120                 // реже пинги — меньше трафика (пинги не считаются сообщениями)

#define TINY_GSM_MODEM_SIM800
#define TINY_GSM_RX_BUFFER 1024

#include "utilities.h"
#include "sun.h"

#include <TinyGsmClient.h>
#include <BlynkSimpleTinyGSM.h>
#include <Preferences.h>
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
#define VP_COUNT       20

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

#define TELEMETRY_MS   (60UL * 60 * 1000)   // напряжения и сигнал — раз в час
#define TIME_SYNC_MS   (6UL * 60 * 60 * 1000)
#define NET_RETRY_MS   30000UL
#define NET_REBOOT_MS  (15UL * 60 * 1000)   // нет связи 15 мин — перезапуск модема

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
int      csq   = -1;

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
    if (battV > 0) publish(VP_BATT, String(battV, 2));
    if (vbusV > 0) publish(VP_VBUS, String(vbusV, 2));
    if (acinV > 0) publish(VP_ACIN, String(acinV, 2));
    if (csq >= 0)  publish(VP_CSQ, String(csq));
}

// Напряжения и сигнал: обновляем раз в час, и только при заметном изменении
void readTelemetry()
{
    auto changed = [](float a, float b) {
        return fabsf(a - b) >= 0.1f;
    };
#if defined(SIM800L_AXP192_VERSION_20200327) || defined(SIM800C_AXP192_VERSION_20200609)
    float b = axp.getBattVoltage() / 1000.0f;
    float u = axp.getVbusVoltage() / 1000.0f;
    float a = axp.getAcinVoltage() / 1000.0f;
    if (changed(b, battV)) battV = b;
    if (changed(u, vbusV)) vbusV = u;
    if (changed(a, acinV)) acinV = a;
#endif
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

uint32_t lastOnline = 0;

void netLoop()
{
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

    if (millis() - lastOnline > NET_REBOOT_MS) {
        SerialMon.println("Долго нет связи — перезапуск модема");
        modem.restart();
        lastOnline = millis();
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
    loadSettings();

    setupModem();                           // PMU, питание и PWRKEY модема (utilities.h)
#if defined(SIM800L_AXP192_VERSION_20200327) || defined(SIM800C_AXP192_VERSION_20200609)
    axp.adc1Enable(AXP202_ACIN_VOL_ADC1, true);
#endif
    pwmBegin();                             // после setupModem: он перенастраивает GPIO светодиода (канал 5)

    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    delay(3000);
    SerialMon.println("Запуск модема...");
    modem.restart();
    enableNetworkTime();

    // Время нужно для расписания — ждём сеть до минуты, дальше пробуем в фоне
    if (modem.waitForNetwork(60000L)) {
        for (int i = 0; i < 6 && !syncTimeFromGsm(); i++) {
            delay(5000);
        }
    }

    Blynk.config(modem, AUTH_TOKENS[DEVICE_ID - 1]);
    lastOnline = millis();
    computeTargets();
    readTelemetry();
}

void loop()
{
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
    if (millis() - lastTimeSync >= TIME_SYNC_MS || (!timeOk && millis() - lastTimeSync >= NET_RETRY_MS)) {
        lastTimeSync = millis();
        syncTimeFromGsm();
    }

    fadeStep();
    saveIfDirty();
    delay(2);
}
