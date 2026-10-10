/*
  Arduino_HouseLighting — управление подсветкой дома по 4 углам.

  Система — 4 независимых устройства LilyGo T-Call (по одному на угол дома),
  на каждом одна и та же прошивка, отличается только номер устройства (DEVICE_ID).

  На каждом устройстве:
  - 8 каналов ШИМ через PCA9685 (I2C) — диммирование 0-100 %, плавное
    включение/выключение, гамма-коррекция;
  - автоматическое включение на закате и выключение на рассвете (расчёт по
    координатам, интернет для этого не нужен), смещения в минутах,
    ночной режим (после заданного часа яркость снижается до N %);
  - веб-интерфейс для телефона и компьютера (http://houselight-N.local или по IP);
    в нём же общая панель всех 4 устройств (адреса соседей задаются в настройках);
  - управление по SMS через SIM800 с разрешённого номера;
  - время: NTP по Wi-Fi, либо время сотовой сети (SIM800), либо с браузера;
  - все настройки хранятся во флеш-памяти (Preferences).

  Подробности и схема подключения — в README.md рядом со скетчем.
*/

// ===== Выберите свою плату =====
// #define SIM800L_IP5306_VERSION_20190610
// #define SIM800L_AXP192_VERSION_20200327
// #define SIM800C_AXP192_VERSION_20200609
#define SIM800L_IP5306_VERSION_20200811

// Номер устройства (угла) 1..4 — прошейте каждую плату со своим номером
// (можно поменять и позже в веб-настройках)
#define DEVICE_ID   1

// 1 — использовать SIM800 (SMS-команды, время сотовой сети); 0 — только Wi-Fi
#define USE_MODEM   1

#include "utilities.h"
#include "sun.h"
#include "webpage.h"

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#define SerialMon Serial
#define SerialAT  Serial1

// ===== Выходы: PCA9685 на шине I2C платы (SDA 21, SCL 22, общая с PMU) =====
#define NCH          8                      // каналов на устройство (до 16 на один PCA9685)
#define PCA9685_ADDR 0x40
#define PWM_FREQ     1000                   // Гц, 24..1526
#define PWM_MAX      4095
#define PWM_INVERT   0                      // 1 — если драйвер включается низким уровнем
#define FADE_MS      2000                   // время плавного перехода 0 -> 100 %
#define GAMMA        2.2f                   // гамма-коррекция для равномерной яркости

#define HOSTNAME_FMT "houselight-%u"        // houselight-1.local ... houselight-4.local
#define AP_SSID_FMT  "HouseLight-%u-Setup"  // точка доступа для первой настройки
#define AP_PASS      "12345678"

// ===== Настройки (сохраняются во флеш) =====
#define CFG_VERSION 2

struct Config {
    uint16_t version;
    uint8_t  deviceId;          // 1..4
    char     deviceName[32];
    char     wifiSsid[33];
    char     wifiPass[65];
    char     webUser[17];
    char     webPass[33];       // пусто — без пароля
    float    lat;
    float    lon;
    char     tz[48];            // POSIX TZ, например "MSK-3"
    int16_t  sunsetOffset;      // мин, включение = закат + смещение
    int16_t  sunriseOffset;     // мин, выключение = рассвет + смещение
    bool     nightEnabled;
    uint16_t nightStart;        // минуты от полуночи
    uint8_t  nightPct;          // яркость ночью, % от вечерней (0 — выключить)
    char     phone[20];         // номер, с которого принимаются SMS-команды
    char     peers[3][40];      // адреса остальных трёх устройств для общей панели
    char     name[NCH][32];
    uint8_t  level[NCH];        // вечерняя яркость каждого канала в авто-режиме
    bool     autoOn[NCH];       // канал участвует в авто-режиме
};

// Состояние, которое переживает перезагрузку (режим и ручные уровни)
struct Runtime {
    uint8_t mode;
    uint8_t manual[NCH];
};

enum Mode : uint8_t { MODE_AUTO = 0, MODE_MANUAL = 1 };

Config     cfg;
Runtime    rt;
Preferences prefs;
WebServer  server(80);

int8_t   overrideLvl[NCH];                   // ручное значение в AUTO до ближайшего заката/рассвета (-1 — нет)
uint8_t  targetLvl[NCH] = {0};
float    curLvl[NCH]    = {0};
bool     pcaOk          = false;
char     hostname[24];
bool     needRecalc     = true;
bool     rtDirty        = false;
uint32_t rtDirtyAt      = 0;

time_t   sunriseT = 0, sunsetT = 0, onT = 0, offT = 0;
int      polar      = 0;
int      lastSunDay = -1;
int      lastDark   = -1;
bool     isDark     = false;
bool     isNight    = false;
const char *timeSource = "нет";

bool     apActive      = false;
uint32_t wifiLostAt    = 0;
uint32_t lastWifiRetry = 0;

bool     modemOk  = false;
int      modemCsq = -1;

// ---------------------------------------------------------------------------
// Настройки
// ---------------------------------------------------------------------------

void setDefaults()
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.version  = CFG_VERSION;
    cfg.deviceId = DEVICE_ID;
    snprintf(cfg.deviceName, sizeof(cfg.deviceName), "Угол %d", DEVICE_ID);
    strlcpy(cfg.webUser, "admin", sizeof(cfg.webUser));
    cfg.lat = 55.7558f;                     // Москва — поменяйте на свои координаты
    cfg.lon = 37.6173f;
    strlcpy(cfg.tz, "MSK-3", sizeof(cfg.tz));
    cfg.nightEnabled = false;
    cfg.nightStart   = 23 * 60;
    cfg.nightPct     = 30;
    for (int i = 0; i < NCH; i++) {
        snprintf(cfg.name[i], sizeof(cfg.name[i]), "Канал %d", i + 1);
        cfg.level[i]  = 100;
        cfg.autoOn[i] = true;
    }
}

void loadConfig()
{
    prefs.begin("light", false);
    if (prefs.getBytesLength("cfg") != sizeof(cfg) ||
            prefs.getBytes("cfg", &cfg, sizeof(cfg)) != sizeof(cfg) ||
            cfg.version != CFG_VERSION) {
        setDefaults();
    }
    memset(&rt, 0, sizeof(rt));
    if (prefs.getBytesLength("rt") == sizeof(rt)) {
        prefs.getBytes("rt", &rt, sizeof(rt));
    }
    if (rt.mode > MODE_MANUAL) {
        rt.mode = MODE_AUTO;
    }
    if (cfg.deviceId < 1 || cfg.deviceId > 9) {
        cfg.deviceId = DEVICE_ID;
    }
    for (int i = 0; i < NCH; i++) {
        overrideLvl[i] = -1;
    }
}

void saveConfig()
{
    prefs.putBytes("cfg", &cfg, sizeof(cfg));
}

// Ручные уровни сохраняем с задержкой, чтобы не писать во флеш на каждое движение ползунка
void markRuntimeDirty()
{
    rtDirty   = true;
    rtDirtyAt = millis();
}

void saveRuntimeIfDirty()
{
    if (rtDirty && millis() - rtDirtyAt > 5000) {
        prefs.putBytes("rt", &rt, sizeof(rt));
        rtDirty = false;
    }
}

// ---------------------------------------------------------------------------
// ШИМ-выходы
// ---------------------------------------------------------------------------

void pcaWriteReg(uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(PCA9685_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

void pwmWrite(int i, float pct)
{
    uint32_t duty = 0;
    if (pct > 0) {
        duty = (uint32_t)lroundf(powf(pct / 100.0f, GAMMA) * PWM_MAX);
        if (duty == 0) {
            duty = 1;
        }
    }
    if (PWM_INVERT) {
        duty = PWM_MAX - duty;
    }
    // LEDn_ON = 0, LEDn_OFF = duty; бит 4 старшего байта — "полностью вкл/выкл"
    uint16_t on = 0, off = duty;
    if (duty == 0) {
        off = 0x1000;
    } else if (duty >= PWM_MAX) {
        on = 0x1000;
        off = 0;
    }
    Wire.beginTransmission(PCA9685_ADDR);
    Wire.write(0x06 + 4 * i);
    Wire.write(on & 0xFF);
    Wire.write(on >> 8);
    Wire.write(off & 0xFF);
    Wire.write(off >> 8);
    Wire.endTransmission();
}

void pwmBegin()
{
    // Wire уже запущен в setupPMU() на пинах I2C_SDA / I2C_SCL платы
    Wire.setClock(400000);
    Wire.beginTransmission(PCA9685_ADDR);
    pcaOk = Wire.endTransmission() == 0;
    if (!pcaOk) {
        SerialMon.println("PCA9685 не найден — проверьте подключение SDA/SCL/питания");
        return;
    }
    uint8_t prescale = (uint8_t)lroundf(25000000.0f / (4096.0f * PWM_FREQ)) - 1;
    pcaWriteReg(0x00, 0x10);                // MODE1: сон (частоту можно менять только во сне)
    pcaWriteReg(0xFE, prescale);            // PRE_SCALE
    pcaWriteReg(0x00, 0x20);                // MODE1: автоинкремент, проснуться
    delay(1);
    pcaWriteReg(0x01, 0x04);                // MODE2: двухтактные выходы (для затворов MOSFET)
    for (int i = 0; i < NCH; i++) {
        pwmWrite(i, 0);
    }
}

// Плавно ведём текущую яркость к целевой
void fadeStep()
{
    static uint32_t last = millis();
    uint32_t now = millis();
    if (now - last < 10) {
        return;
    }
    float step = 100.0f * (now - last) / FADE_MS;
    last = now;
    for (int i = 0; i < NCH; i++) {
        float diff = targetLvl[i] - curLvl[i];
        if (diff == 0) {
            continue;
        }
        if (fabsf(diff) <= step) {
            curLvl[i] = targetLvl[i];
        } else {
            curLvl[i] += diff > 0 ? step : -step;
        }
        if (pcaOk) {
            pwmWrite(i, curLvl[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Время и расписание
// ---------------------------------------------------------------------------

bool timeValid()
{
    return time(nullptr) > 1700000000;
}

void applyTimezone()
{
    setenv("TZ", cfg.tz, 1);
    tzset();
    lastSunDay = -1;
}

void setSystemTime(time_t t, const char *source)
{
    timeval tv = {t, 0};
    settimeofday(&tv, nullptr);
    timeSource = source;
    lastSunDay = -1;
    needRecalc = true;
    SerialMon.printf("Время установлено (%s)\n", source);
}

void onNtpSync(timeval *)
{
    timeSource = "NTP";
    lastSunDay = -1;
    needRecalc = true;
}

String fmtTime(time_t t, const char *fmt = "%H:%M")
{
    if (t == 0) {
        return "--:--";
    }
    struct tm lt;
    localtime_r(&t, &lt);
    char buf[24];
    strftime(buf, sizeof(buf), fmt, &lt);
    return buf;
}

// Пересчёт восхода/заката раз в сутки (или после смены настроек)
void updateSun(time_t now)
{
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_yday == lastSunDay) {
        return;
    }
    lastSunDay = lt.tm_yday;
    int y = lt.tm_year + 1900, m = lt.tm_mon + 1, d = lt.tm_mday;
    double rise = 0, set = 0;
    polar = sunCalc(y, m, d, cfg.lat, cfg.lon, rise, set);
    if (polar == 0) {
        time_t base = (time_t)daysFromCivil(y, m, d) * 86400;
        sunriseT = base + (time_t)lround(rise * 60);
        sunsetT  = base + (time_t)lround(set * 60);
    } else {
        sunriseT = sunsetT = 0;
    }
    onT  = sunsetT ? sunsetT + cfg.sunsetOffset * 60 : 0;
    offT = sunriseT ? sunriseT + cfg.sunriseOffset * 60 : 0;
    SerialMon.printf("Рассвет %s, закат %s, включение %s, выключение %s\n",
                     fmtTime(sunriseT).c_str(), fmtTime(sunsetT).c_str(),
                     fmtTime(onT).c_str(), fmtTime(offT).c_str());
}

bool computeDark(time_t now)
{
    if (polar < 0) {
        return true;
    }
    if (polar > 0) {
        return false;
    }
    return now < offT || now >= onT;
}

// Ночной период — от nightStart до полудня (и только когда темно)
bool computeNight(time_t now, bool dark)
{
    if (!cfg.nightEnabled || !dark) {
        return false;
    }
    struct tm lt;
    localtime_r(&now, &lt);
    int m     = lt.tm_hour * 60 + lt.tm_min;
    int since = (m - cfg.nightStart + 1440) % 1440;
    int span  = (720 - cfg.nightStart + 1440) % 1440;
    return since < span;
}

void computeTargets()
{
    bool tv = timeValid();
    if (tv) {
        time_t now = time(nullptr);
        updateSun(now);
        isDark  = computeDark(now);
        isNight = computeNight(now, isDark);
        // На закате и рассвете ручные корректировки сбрасываются — дальше снова автоматика
        if (lastDark != -1 && lastDark != (int)isDark) {
            for (int i = 0; i < NCH; i++) {
                overrideLvl[i] = -1;
            }
            SerialMon.println(isDark ? "Закат: включение" : "Рассвет: выключение");
        }
        lastDark = isDark;
    }
    for (int i = 0; i < NCH; i++) {
        uint8_t t;
        if (rt.mode == MODE_MANUAL) {
            t = rt.manual[i];
        } else if (overrideLvl[i] >= 0) {
            t = overrideLvl[i];
        } else if (!tv || !cfg.autoOn[i] || !isDark) {
            t = 0;
        } else if (isNight) {
            t = cfg.level[i] * cfg.nightPct / 100;
        } else {
            t = cfg.level[i];
        }
        targetLvl[i] = t;
    }
}

// ---------------------------------------------------------------------------
// Общие команды (используются и вебом, и SMS)
// ---------------------------------------------------------------------------

void setCornerLevel(int i, int level)
{
    level = constrain(level, 0, 100);
    if (rt.mode == MODE_MANUAL) {
        rt.manual[i] = level;
        markRuntimeDirty();
    } else {
        overrideLvl[i] = level;
    }
    needRecalc = true;
}

void setCornerAuto(int i)
{
    overrideLvl[i] = -1;
    needRecalc = true;
}

void setMode(Mode m)
{
    if (m == MODE_MANUAL && rt.mode != MODE_MANUAL) {
        // переходим в ручной режим без скачка яркости
        for (int i = 0; i < NCH; i++) {
            rt.manual[i] = targetLvl[i];
        }
    }
    for (int i = 0; i < NCH; i++) {
        overrideLvl[i] = -1;
    }
    rt.mode = m;
    markRuntimeDirty();
    needRecalc = true;
}

String statusText()
{
    String s = "Dev" + String(cfg.deviceId) + " " + (rt.mode == MODE_AUTO ? "AUTO" : "MANUAL");
    if (timeValid()) {
        s += " " + fmtTime(time(nullptr), "%d.%m %H:%M");
        if (polar == 0) {
            s += " On " + fmtTime(onT) + " Off " + fmtTime(offT);
        }
        s += isDark ? (isNight ? " NIGHT" : " DARK") : " DAY";
    } else {
        s += " NO TIME";
    }
    for (int i = 0; i < NCH; i++) {
        s += String(i % 4 ? " " : "\n") + "C" + String(i + 1) + "=" + String(targetLvl[i]);
        if (rt.mode == MODE_AUTO && overrideLvl[i] >= 0) {
            s += "*";
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// SIM800: AT-команды, SMS, время сети
// ---------------------------------------------------------------------------
#if USE_MODEM

String lineBuf;
String smsFrom;
bool   cmtPending = false;
String smsQueueFrom, smsQueueText;
bool   smsQueued = false;

bool modemReadLine(String &out)
{
    while (SerialAT.available()) {
        char c = SerialAT.read();
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (lineBuf.length()) {
                out = lineBuf;
                lineBuf = "";
                return true;
            }
            continue;
        }
        lineBuf += c;
        if (lineBuf.length() > 400) {
            lineBuf = "";
        }
    }
    return false;
}

// Незапрошенные сообщения модема. Входящая SMS (+CMT) — заголовок и следующая строка с текстом.
bool handleUrc(const String &line)
{
    if (cmtPending) {
        cmtPending = false;
        if (!smsQueued) {
            smsQueueFrom = smsFrom;
            smsQueueText = line;
            smsQueued    = true;
        }
        return true;
    }
    if (line.startsWith("+CMT:")) {
        int a = line.indexOf('"');
        int b = line.indexOf('"', a + 1);
        smsFrom    = (a >= 0 && b > a) ? line.substring(a + 1, b) : "";
        cmtPending = true;
        return true;
    }
    return false;
}

bool atWait(String *resp, uint32_t timeout)
{
    uint32_t start = millis();
    String   line;
    while (millis() - start < timeout) {
        if (!modemReadLine(line)) {
            delay(1);
            continue;
        }
        if (handleUrc(line)) {
            continue;
        }
        if (line == "OK") {
            return true;
        }
        if (line.indexOf("ERROR") >= 0) {
            return false;
        }
        if (resp) {
            *resp += line;
            *resp += '\n';
        }
    }
    return false;
}

bool atCmd(const String &cmd, String *resp = nullptr, uint32_t timeout = 2000)
{
    SerialAT.print(cmd);
    SerialAT.print("\r");
    return atWait(resp, timeout);
}

bool sendSms(const String &number, const String &text)
{
    SerialAT.print("AT+CMGS=\"");
    SerialAT.print(number);
    SerialAT.print("\"\r");
    uint32_t start = millis();
    String   line;
    bool     prompt = false;
    while (millis() - start < 5000 && !prompt) {
        if (modemReadLine(line)) {
            handleUrc(line);
        }
        if (lineBuf.indexOf('>') >= 0) {
            prompt = true;
        }
        delay(1);
    }
    lineBuf = "";
    if (!prompt) {
        SerialAT.write(0x1B);   // отмена
        return false;
    }
    SerialAT.print(text.substring(0, 160));
    SerialAT.write(0x1A);
    return atWait(nullptr, 60000);
}

// Время из сети оператора (NITZ). Работает, если оператор передаёт время.
bool syncTimeFromGsm()
{
    String r;
    if (!atCmd("AT+CCLK?", &r)) {
        return false;
    }
    int p = r.indexOf('"');
    if (p < 0) {
        return false;
    }
    int  yy, MM, dd, hh, mi, ss, q;
    char sign;
    if (sscanf(r.c_str() + p + 1, "%d/%d/%d,%d:%d:%d%c%d", &yy, &MM, &dd, &hh, &mi, &ss, &sign, &q) != 8) {
        return false;
    }
    if (yy < 24) {
        return false;           // часы модема ещё не синхронизированы
    }
    if (sign == '-') {
        q = -q;
    }
    time_t local = (time_t)daysFromCivil(2000 + yy, MM, dd) * 86400 + hh * 3600 + mi * 60 + ss;
    setSystemTime(local - q * 15 * 60, "GSM");   // смещение пояса — в четвертях часа
    return true;
}

// Сравниваем номера по последним 9 цифрам (+7 / 8 / +380 и т.п.)
bool phoneMatches(const String &a, const char *b)
{
    String da, db;
    for (char c : a) if (isdigit(c)) da += c;
    for (const char *p = b; *p; p++) if (isdigit(*p)) db += *p;
    if (da.length() < 9 || db.length() < 9) {
        return false;
    }
    return da.substring(da.length() - 9) == db.substring(db.length() - 9);
}

int parseLevel(const String &s)
{
    if (s == "ON") return 100;
    if (s == "OFF") return 0;
    if (s.length() && isdigit(s[0])) return constrain(s.toInt(), 0, 100);
    return -1;
}

String processSmsCommand(String cmd)
{
    cmd.trim();
    cmd.toUpperCase();
    String arg;
    int sp = cmd.indexOf(' ');
    if (sp > 0) {
        arg = cmd.substring(sp + 1);
        arg.trim();
        cmd = cmd.substring(0, sp);
    }
    if (cmd == "AUTO") {
        setMode(MODE_AUTO);
    } else if (cmd == "MANUAL") {
        setMode(MODE_MANUAL);
    } else if (cmd == "ON" || cmd == "OFF") {
        int lvl = cmd == "OFF" ? 0 : (arg.length() ? parseLevel(arg) : 100);
        if (lvl < 0) lvl = 100;
        for (int i = 0; i < NCH; i++) setCornerLevel(i, lvl);
    } else if (cmd.length() >= 2 && cmd[0] == 'C' && cmd.substring(1).toInt() >= 1 && cmd.substring(1).toInt() <= NCH) {
        int i = cmd.substring(1).toInt() - 1;
        if (arg == "AUTO") {
            setCornerAuto(i);
        } else {
            int lvl = parseLevel(arg);
            if (lvl < 0) return "Format: C1 50 | C1 ON | C1 OFF | C1 AUTO";
            setCornerLevel(i, lvl);
        }
    } else if (cmd != "STATUS" && cmd != "?") {
        return "Commands: STATUS, ON [0-100], OFF, AUTO, MANUAL, C1..C8 <0-100|ON|OFF|AUTO>";
    }
    computeTargets();
    return statusText();
}

void modemBegin()
{
    setupModem();                           // питание и PWRKEY (utilities.h)
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    for (int i = 0; i < 24 && !modemOk; i++) {
        modemOk = atCmd("AT", nullptr, 500);
    }
    if (!modemOk) {
        SerialMon.println("SIM800 не отвечает — работаем без модема");
        return;
    }
    atCmd("ATE0");
    String r;
    atCmd("AT+CLTS?", &r);
    if (r.indexOf("+CLTS: 1") < 0) {
        // синхронизация времени с сетью включится после перезапуска модема
        atCmd("AT+CLTS=1");
        atCmd("AT&W");
    }
    atCmd("AT+CMGF=1");                     // текстовый режим SMS
    atCmd("AT+CSCS=\"GSM\"");
    atCmd("AT+CNMI=2,2,0,0,0");             // новые SMS сразу в порт, без сохранения
    atCmd("AT+CMGDA=\"DEL ALL\"", nullptr, 10000);
    SerialMon.println("SIM800 готов");
}

void modemLoop()
{
    if (!modemOk) {
        return;
    }
    String line;
    while (modemReadLine(line)) {
        handleUrc(line);
    }
    if (smsQueued) {
        smsQueued = false;
        SerialMon.printf("SMS от %s: %s\n", smsQueueFrom.c_str(), smsQueueText.c_str());
        if (cfg.phone[0] && phoneMatches(smsQueueFrom, cfg.phone)) {
            String reply = processSmsCommand(smsQueueText);
            sendSms(smsQueueFrom, reply);
        } else {
            SerialMon.println("Номер не разрешён — команда проигнорирована");
        }
    }
    static uint32_t lastCsq = 0;
    if (millis() - lastCsq > 60000 || lastCsq == 0) {
        lastCsq = millis();
        String r;
        if (atCmd("AT+CSQ", &r)) {
            int p = r.indexOf("+CSQ:");
            modemCsq = p >= 0 ? r.substring(p + 5).toInt() : -1;
        }
    }
    // Если NTP недоступен — берём время сети: каждые 30 с пока его нет, потом раз в 6 часов
    static uint32_t lastSync = 0;
    bool     ntp      = strcmp(timeSource, "NTP") == 0;
    uint32_t interval = timeValid() ? 6UL * 3600 * 1000 : 30000;
    if (!ntp && (lastSync == 0 || millis() - lastSync > interval)) {
        lastSync = millis();
        syncTimeFromGsm();
    }
}

#endif // USE_MODEM

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

void startAp()
{
    char ssid[32];
    snprintf(ssid, sizeof(ssid), AP_SSID_FMT, cfg.deviceId);
    WiFi.mode(cfg.wifiSsid[0] ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(ssid, AP_PASS);
    apActive = true;
    SerialMon.printf("Точка доступа %s (пароль %s), http://%s\n", ssid, AP_PASS,
                     WiFi.softAPIP().toString().c_str());
}

void wifiBegin()
{
    snprintf(hostname, sizeof(hostname), HOSTNAME_FMT, cfg.deviceId);
    WiFi.persistent(false);
    WiFi.setHostname(hostname);
    if (cfg.wifiSsid[0]) {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
        SerialMon.printf("Подключение к %s", cfg.wifiSsid);
        for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
            delay(500);
            SerialMon.print('.');
        }
        SerialMon.println();
    }
    if (WiFi.status() == WL_CONNECTED) {
        SerialMon.printf("Wi-Fi подключен, http://%s или http://%s.local\n",
                         WiFi.localIP().toString().c_str(), hostname);
    } else {
        startAp();
    }
    sntp_set_time_sync_notification_cb(onNtpSync);
    configTzTime(cfg.tz, "pool.ntp.org", "time.google.com");
    if (MDNS.begin(hostname)) {
        MDNS.addService("http", "tcp", 80);
    }
}

void wifiLoop()
{
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected) {
        wifiLostAt = 0;
        // домашняя сеть есть — точка доступа больше не нужна
        if (apActive && millis() > 120000) {
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            apActive = false;
        }
        return;
    }
    if (!cfg.wifiSsid[0]) {
        return;
    }
    if (wifiLostAt == 0) {
        wifiLostAt = millis();
    }
    if (millis() - lastWifiRetry > 60000) {
        lastWifiRetry = millis();
        WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
    }
    // сеть пропала надолго — поднимаем точку доступа, чтобы управление оставалось
    if (!apActive && millis() - wifiLostAt > 120000) {
        startAp();
    }
}

// ---------------------------------------------------------------------------
// Веб-интерфейс и HTTP API
// ---------------------------------------------------------------------------

// Разрешаем запросы с панелей соседних устройств (другой адрес = другой origin)
void sendCors()
{
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
}

bool checkAuth()
{
    sendCors();
    if (!cfg.webPass[0] || server.authenticate(cfg.webUser, cfg.webPass)) {
        return true;
    }
    server.requestAuthentication();
    return false;
}

String jsonEscape(const char *s)
{
    String out;
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            out += '\\';
        }
        if ((uint8_t)*s >= 0x20) {
            out += *s;
        }
    }
    return out;
}

String hhmm(uint16_t minutes)
{
    char buf[6];
    snprintf(buf, sizeof(buf), "%02u:%02u", minutes / 60, minutes % 60);
    return buf;
}

void sendOk()
{
    server.send(200, "application/json", "{\"ok\":true}");
}

void handleRoot()
{
    if (!checkAuth()) return;
    server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleState()
{
    if (!checkAuth()) return;
    bool   tv = timeValid();
    String j  = "{";
    j += "\"id\":" + String(cfg.deviceId);
    j += ",\"device\":\"" + jsonEscape(cfg.deviceName) + "\"";
    j += ",\"pca\":" + String(pcaOk ? "true" : "false");
    j += ",\"mode\":\"" + String(rt.mode == MODE_AUTO ? "auto" : "manual") + "\"";
    j += ",\"timeValid\":" + String(tv ? "true" : "false");
    j += ",\"time\":\"" + (tv ? fmtTime(time(nullptr), "%d.%m.%Y %H:%M:%S") : String("")) + "\"";
    j += ",\"timeSrc\":\"" + String(timeSource) + "\"";
    j += ",\"polar\":" + String(polar);
    j += ",\"sunrise\":\"" + fmtTime(sunriseT) + "\"";
    j += ",\"sunset\":\"" + fmtTime(sunsetT) + "\"";
    j += ",\"on\":\"" + fmtTime(onT) + "\"";
    j += ",\"off\":\"" + fmtTime(offT) + "\"";
    j += ",\"dark\":" + String(isDark ? "true" : "false");
    j += ",\"night\":" + String(isNight ? "true" : "false");
    j += ",\"wifi\":\"" + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("нет")) + "\"";
    j += ",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
    j += ",\"gsm\":" + String(modemOk ? "true" : "false");
    j += ",\"csq\":" + String(modemCsq);
    j += ",\"ch\":[";
    for (int i = 0; i < NCH; i++) {
        if (i) j += ",";
        j += "{\"name\":\"" + jsonEscape(cfg.name[i]) + "\"";
        j += ",\"target\":" + String(targetLvl[i]);
        j += ",\"cur\":" + String((int)lroundf(curLvl[i]));
        j += ",\"ovr\":" + String(overrideLvl[i]);
        j += ",\"auto\":" + String(cfg.autoOn[i] ? "true" : "false") + "}";
    }
    j += "]}";
    server.send(200, "application/json", j);
}

void handleCorner()
{
    if (!checkAuth()) return;
    int i = server.arg("i").toInt();
    if (i < 0 || i >= NCH) {
        server.send(400, "text/plain", "bad channel");
        return;
    }
    if (server.hasArg("auto")) {
        setCornerAuto(i);
    } else {
        setCornerLevel(i, server.arg("level").toInt());
    }
    sendOk();
}

void handleAll()
{
    if (!checkAuth()) return;
    if (server.hasArg("auto")) {
        for (int i = 0; i < NCH; i++) setCornerAuto(i);
    } else {
        int lvl = server.arg("level").toInt();
        for (int i = 0; i < NCH; i++) setCornerLevel(i, lvl);
    }
    sendOk();
}

void handleMode()
{
    if (!checkAuth()) return;
    setMode(server.arg("mode") == "manual" ? MODE_MANUAL : MODE_AUTO);
    sendOk();
}

void handleGetConfig()
{
    if (!checkAuth()) return;
    String j = "{";
    j += "\"deviceId\":" + String(cfg.deviceId);
    j += ",\"deviceName\":\"" + jsonEscape(cfg.deviceName) + "\"";
    j += ",\"peers\":[\"" + jsonEscape(cfg.peers[0]) + "\",\"" + jsonEscape(cfg.peers[1]) +
         "\",\"" + jsonEscape(cfg.peers[2]) + "\"]";
    j += ",\"lat\":" + String(cfg.lat, 5);
    j += ",\"lon\":" + String(cfg.lon, 5);
    j += ",\"tz\":\"" + jsonEscape(cfg.tz) + "\"";
    j += ",\"sunsetOffset\":" + String(cfg.sunsetOffset);
    j += ",\"sunriseOffset\":" + String(cfg.sunriseOffset);
    j += ",\"nightEnabled\":" + String(cfg.nightEnabled ? "true" : "false");
    j += ",\"nightStart\":\"" + hhmm(cfg.nightStart) + "\"";
    j += ",\"nightPct\":" + String(cfg.nightPct);
    j += ",\"phone\":\"" + jsonEscape(cfg.phone) + "\"";
    j += ",\"webUser\":\"" + jsonEscape(cfg.webUser) + "\"";
    j += ",\"webPassSet\":" + String(cfg.webPass[0] ? "true" : "false");
    j += ",\"wifiSsid\":\"" + jsonEscape(cfg.wifiSsid) + "\"";
    j += ",\"ch\":[";
    for (int i = 0; i < NCH; i++) {
        if (i) j += ",";
        j += "{\"name\":\"" + jsonEscape(cfg.name[i]) + "\",\"level\":" + String(cfg.level[i]) +
             ",\"auto\":" + String(cfg.autoOn[i] ? "true" : "false") + "}";
    }
    j += "]}";
    server.send(200, "application/json", j);
}

void copyArg(const char *name, char *dst, size_t size)
{
    if (server.hasArg(name)) {
        strlcpy(dst, server.arg(name).c_str(), size);
    }
}

void handleSetConfig()
{
    if (!checkAuth()) return;
    uint8_t oldId = cfg.deviceId;
    if (server.hasArg("deviceId")) cfg.deviceId = constrain(server.arg("deviceId").toInt(), 1, 9);
    copyArg("deviceName", cfg.deviceName, sizeof(cfg.deviceName));
    copyArg("peer0", cfg.peers[0], sizeof(cfg.peers[0]));
    copyArg("peer1", cfg.peers[1], sizeof(cfg.peers[1]));
    copyArg("peer2", cfg.peers[2], sizeof(cfg.peers[2]));
    if (server.hasArg("lat")) cfg.lat = constrain(server.arg("lat").toFloat(), -90.0f, 90.0f);
    if (server.hasArg("lon")) cfg.lon = constrain(server.arg("lon").toFloat(), -180.0f, 180.0f);
    copyArg("tz", cfg.tz, sizeof(cfg.tz));
    if (server.hasArg("sunsetOffset"))  cfg.sunsetOffset  = constrain(server.arg("sunsetOffset").toInt(), -240, 240);
    if (server.hasArg("sunriseOffset")) cfg.sunriseOffset = constrain(server.arg("sunriseOffset").toInt(), -240, 240);
    if (server.hasArg("nightEnabled"))  cfg.nightEnabled  = server.arg("nightEnabled") == "1";
    if (server.hasArg("nightStart")) {
        String s = server.arg("nightStart");
        int h = s.toInt(), m = s.indexOf(':') > 0 ? s.substring(s.indexOf(':') + 1).toInt() : 0;
        cfg.nightStart = constrain(h, 0, 23) * 60 + constrain(m, 0, 59);
    }
    if (server.hasArg("nightPct")) cfg.nightPct = constrain(server.arg("nightPct").toInt(), 0, 100);
    copyArg("phone", cfg.phone, sizeof(cfg.phone));
    copyArg("webUser", cfg.webUser, sizeof(cfg.webUser));
    if (server.hasArg("webPass") && server.arg("webPass").length()) {
        // "-" — убрать пароль
        strlcpy(cfg.webPass, server.arg("webPass") == "-" ? "" : server.arg("webPass").c_str(), sizeof(cfg.webPass));
    }
    for (int i = 0; i < NCH; i++) {
        String n = String(i);
        copyArg(("name" + n).c_str(), cfg.name[i], sizeof(cfg.name[i]));
        if (server.hasArg("level" + n)) cfg.level[i]  = constrain(server.arg("level" + n).toInt(), 0, 100);
        if (server.hasArg("auto" + n))  cfg.autoOn[i] = server.arg("auto" + n) == "1";
    }
    saveConfig();
    applyTimezone();
    needRecalc = true;
    sendOk();
    if (cfg.deviceId != oldId) {
        delay(500);
        ESP.restart();          // новое имя в сети и точки доступа
    }
}

void handleWifi()
{
    if (!checkAuth()) return;
    copyArg("ssid", cfg.wifiSsid, sizeof(cfg.wifiSsid));
    copyArg("pass", cfg.wifiPass, sizeof(cfg.wifiPass));
    saveConfig();
    sendOk();
    delay(500);
    ESP.restart();
}

// Установка времени с телефона/компьютера, если нет ни интернета, ни времени от оператора
void handleTime()
{
    if (!checkAuth()) return;
    long long t = atoll(server.arg("epoch").c_str());
    if (t < 1700000000LL) {
        server.send(400, "text/plain", "bad time");
        return;
    }
    if (strcmp(timeSource, "NTP") != 0) {
        setSystemTime((time_t)t, "браузер");
    }
    sendOk();
}

void webBegin()
{
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/state", HTTP_GET, handleState);
    server.on("/api/corner", HTTP_POST, handleCorner);
    server.on("/api/all", HTTP_POST, handleAll);
    server.on("/api/mode", HTTP_POST, handleMode);
    server.on("/api/config", HTTP_GET, handleGetConfig);
    server.on("/api/config", HTTP_POST, handleSetConfig);
    server.on("/api/wifi", HTTP_POST, handleWifi);
    server.on("/api/time", HTTP_POST, handleTime);
    server.onNotFound([]() {
        sendCors();
        if (server.method() == HTTP_OPTIONS) {
            server.send(204);   // CORS preflight от панели соседнего устройства
        } else {
            server.send(404, "text/plain", "Not found");
        }
    });
    server.begin();
}

// ---------------------------------------------------------------------------

void setup()
{
    SerialMon.begin(115200);
    delay(10);

    loadConfig();
    setupPMU();                             // заодно запускает Wire на пинах I2C платы
    pwmBegin();
    applyTimezone();

#if USE_MODEM
    modemBegin();
#endif

    wifiBegin();
    webBegin();
    computeTargets();
}

void loop()
{
    server.handleClient();
    wifiLoop();
#if USE_MODEM
    modemLoop();
#endif

    static uint32_t lastCalc = 0;
    if (needRecalc || millis() - lastCalc >= 1000) {
        needRecalc = false;
        lastCalc   = millis();
        computeTargets();
    }
    fadeStep();
    saveRuntimeIfDirty();
    delay(2);
}
