#pragma once
// Расчёт восхода и заката (алгоритм NOAA, точность ~1-2 минуты).
// Используется только математика, без библиотек и без часовых поясов:
// времена возвращаются в минутах UTC от полуночи (UTC) указанной даты.

#include <math.h>
#include <time.h>

// Число дней от 1970-01-01 до даты (григорианский календарь).
static inline long daysFromCivil(int y, int m, int d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static inline bool isLeapYear(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

// Возвращает:
//   0  — обычный день, riseUtcMin / setUtcMin заполнены;
//  -1  — полярная ночь (солнце не восходит);
//   1  — полярный день (солнце не заходит).
static int sunCalc(int y, int m, int d, double lat, double lon,
                   double &riseUtcMin, double &setUtcMin)
{
    const double rad = M_PI / 180.0;
    const int    doy = (int)(daysFromCivil(y, m, d) - daysFromCivil(y, 1, 1)) + 1;
    const double g   = 2.0 * M_PI / (isLeapYear(y) ? 366.0 : 365.0) * (doy - 1 + 0.5);

    const double eqtime = 229.18 * (0.000075 + 0.001868 * cos(g) - 0.032077 * sin(g)
                                    - 0.014615 * cos(2 * g) - 0.040849 * sin(2 * g));
    const double decl = 0.006918 - 0.399912 * cos(g) + 0.070257 * sin(g)
                        - 0.006758 * cos(2 * g) + 0.000907 * sin(2 * g)
                        - 0.002697 * cos(3 * g) + 0.00148 * sin(3 * g);

    const double latR  = lat * rad;
    const double cosHa = cos(90.833 * rad) / (cos(latR) * cos(decl)) - tan(latR) * tan(decl);
    if (cosHa > 1.0) {
        return -1;
    }
    if (cosHa < -1.0) {
        return 1;
    }
    const double ha = acos(cosHa) / rad;
    riseUtcMin = 720.0 - 4.0 * (lon + ha) - eqtime;
    setUtcMin  = 720.0 - 4.0 * (lon - ha) - eqtime;
    return 0;
}
