#!/usr/bin/env python3
"""Проверка всех 4 плат через Blynk, без подключения к компьютеру.

    python3 tools/fleet.py

Печатает таблицу и список проблем. Код выхода 1, если есть ошибки или плата офлайн —
удобно для запуска по расписанию.
"""

import sys

from common import blynk_online, blynk_values, load_tokens, num

CSQ_WEAK = 10
BATT_WARN_PCT = 20
BAD_RESETS = ("brownout", "watchdog", "сбой", "разряда")


def check(dev, token):
    problems, warnings = [], []
    try:
        online = blynk_online(token)
        v = blynk_values(token)
    except Exception as e:  # сеть, неверный токен
        return {"dev": dev, "online": False, "v": {}}, [f"Blynk не ответил: {e}"], []

    if not online:
        problems.append("офлайн")
    st = v.get("selftest") or ""
    if "❌" in st:
        problems.append(st)
    elif "⚠" in st:
        warnings.append(st)
    elif not st:
        warnings.append("нет итога самопроверки (старая прошивка?)")

    csq = num(v.get("csq"))
    if csq is not None and 0 < csq < CSQ_WEAK:
        warnings.append(f"слабый сигнал {csq:.0f}")
    pct = num(v.get("pct"))
    usb = num(v.get("usb"))
    if usb is not None and usb < 1:
        warnings.append("нет внешнего питания 5 В")
        if pct is not None and pct <= BATT_WARN_PCT:
            warnings.append(f"АКБ {pct:.0f} %")
    reset = v.get("reset") or ""
    if any(w in reset for w in BAD_RESETS):
        warnings.append(f"последняя перезагрузка: {reset}")
    if "нет времени" in (v.get("status") or ""):
        problems.append("нет времени")
    return {"dev": dev, "online": online, "v": v}, problems, warnings


def main():
    tokens = load_tokens()
    rows, issues = [], []
    for dev, token in enumerate(tokens, start=1):
        row, problems, warnings = check(dev, token)
        rows.append(row)
        for p in problems:
            issues.append(("❌", dev, p))
        for w in warnings:
            issues.append(("⚠️ ", dev, w))

    head = f"{'Плата':<8}{'Связь':<9}{'Сигнал':<8}{'АКБ':<12}{'5 В':<6}{'Статус':<24}Самопроверка"
    print(head)
    print("-" * len(head))
    for r in rows:
        v = r["v"]
        batt = (f"{v.get('batt')} В " if num(v.get("batt"), 0) > 0 else "") + (f"{v.get('pct')}%" if v.get("pct") else "")
        usb = num(v.get("usb"))
        print(f"{'Ugol ' + str(r['dev']):<8}"
              f"{('онлайн' if r['online'] else 'ОФЛАЙН'):<9}"
              f"{str(v.get('csq') or '—'):<8}"
              f"{batt or '—':<12}"
              f"{('есть' if usb and usb >= 1 else 'нет' if usb is not None else '—'):<6}"
              f"{(v.get('status') or '—')[:23]:<24}"
              f"{v.get('selftest') or '—'}")
    print()
    if not issues:
        print("✅ Все платы в порядке")
        return 0
    for mark, dev, text in issues:
        print(f"{mark} Ugol {dev}: {text}")
    return 1 if any(m == "❌" for m, _, _ in issues) else 0


if __name__ == "__main__":
    sys.exit(main())
