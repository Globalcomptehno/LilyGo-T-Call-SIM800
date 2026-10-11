#pragma once
// Скопируйте этот файл в secrets.h и заполните своими данными.
// secrets.h внесён в .gitignore — токены не попадут в репозиторий.

// Blynk.Console -> Templates -> ваш шаблон -> Template ID / Name
#define BLYNK_TEMPLATE_ID   "TMPLxxxxxxxx"
#define BLYNK_TEMPLATE_NAME "House Light"

// Blynk.Console -> Devices -> устройство -> Device Info -> Auth Token (для плат 1..4)
static const char *AUTH_TOKENS[4] = {
    "TOKEN_DEVICE_1",
    "TOKEN_DEVICE_2",
    "TOKEN_DEVICE_3",
    "TOKEN_DEVICE_4",
};

// APN мобильного интернета вашего оператора
const char APN[]       = "internet";
const char GPRS_USER[] = "";
const char GPRS_PASS[] = "";

// Координаты дома (одинаковые для всех 4 плат)
#define HOME_LAT    55.7558
#define HOME_LON    37.6173
