#!/usr/bin/env python3
"""Стенд прошивки и проверки. Подключили плату по USB — скрипт сам:

1. определяет плату по MAC (tools/boards.json) и её номер;
2. собирает прошивку с нужным DEVICE_ID (ESP32 core 2.0.17 из ~/.arduino-tcall);
3. прошивает и читает монитор порта;
4. проверяет самопроверку платы (строка SELFTEST) и то, что данные дошли до Blynk;
5. печатает итог и ждёт следующую плату.

    python3 tools/bench.py              # прошить и проверить, по кругу
    python3 tools/bench.py --no-flash   # только проверить, без прошивки
    python3 tools/bench.py --once       # одна плата и выход
"""

import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import serial

from common import SKETCH_DIR, blynk_online, blynk_values, load_boards, load_tokens

FQBN = "esp32:esp32:esp32wrover"
UPLOAD_SPEED = 460800       # на 921600 прошивка обрывается (Unable to verify flash chip connection)
TOOLCHAIN = os.path.expanduser("~/.arduino-tcall")
CLI_CFG = os.path.join(TOOLCHAIN, "arduino-cli.yaml")
BUILD_ROOT = os.path.join(TOOLCHAIN, "build")
SKETCH_NAME = "Arduino_HouseLighting_Blynk"
PORT_GLOBS = ["/dev/cu.wchusbserial*", "/dev/cu.usbserial*", "/dev/cu.SLAB_USBtoUART*", "/dev/ttyUSB*"]
BOOT_TIMEOUT = 180          # с: старт + сеть + GPRS + Blynk + первая самопроверка
CSQ_WEAK = 10

OK, BAD, WARN, INFO = "✅", "❌", "⚠️ ", "ⓘ"


def log(msg=""):
    print(msg, flush=True)


# --- поиск платы ---------------------------------------------------------------

def find_ports():
    return sorted(p for g in PORT_GLOBS for p in glob.glob(g))


def wait_port(skip=None):
    shown = False
    while True:
        ports = [p for p in find_ports() if p != skip]
        if ports:
            return ports[0]
        if not shown:
            log("Жду плату на USB...")
            shown = True
        time.sleep(1)


def wait_unplug(port):
    log("Отключите плату, чтобы проверить следующую (Ctrl+C — выход).")
    while os.path.exists(port):
        time.sleep(1)


def read_mac(port):
    esptool = shutil.which("esptool") or shutil.which("esptool.py")
    out = subprocess.run([esptool, "--chip", "esp32", "-p", port, "read-mac"],
                         capture_output=True, text=True).stdout
    m = re.search(r"MAC:\s*([0-9a-f:]{17})", out, re.I)
    return m.group(1).lower() if m else None


# --- сборка и прошивка ---------------------------------------------------------

def sources_hash(dev):
    h = hashlib.sha256(f"dev={dev}".encode())
    for name in sorted(os.listdir(SKETCH_DIR)):
        if name.endswith((".ino", ".h")):
            with open(os.path.join(SKETCH_DIR, name), "rb") as f:
                h.update(name.encode() + f.read())
    return h.hexdigest()


def build(dev):
    out = os.path.join(BUILD_ROOT, f"d{dev}")
    stamp = os.path.join(out, ".sources")
    digest = sources_hash(dev)
    if os.path.exists(stamp) and open(stamp).read() == digest:
        return out
    log(f"Сборка прошивки для платы №{dev}...")
    with tempfile.TemporaryDirectory() as tmp:
        work = os.path.join(tmp, SKETCH_NAME)
        os.makedirs(work)
        for name in os.listdir(SKETCH_DIR):
            if name.endswith((".ino", ".h")):
                shutil.copy(os.path.join(SKETCH_DIR, name), work)
        ino = os.path.join(work, SKETCH_NAME + ".ino")
        text = open(ino, encoding="utf-8").read()
        text, n = re.subn(r"^#define DEVICE_ID\s+\d+", f"#define DEVICE_ID   {dev}", text, count=1, flags=re.M)
        if n != 1:
            raise SystemExit("Не нашёл строку #define DEVICE_ID в скетче")
        open(ino, "w", encoding="utf-8").write(text)
        r = subprocess.run(["arduino-cli", "--config-file", CLI_CFG, "compile", "--fqbn", FQBN,
                            "--build-path", out, work], capture_output=True, text=True)
    if r.returncode != 0:
        log(r.stdout[-3000:] + r.stderr[-3000:])
        raise SystemExit("Сборка не удалась")
    open(stamp, "w").write(digest)
    return out


def upload(port, build_dir):
    log("Прошивка...")
    r = subprocess.run(["arduino-cli", "--config-file", CLI_CFG, "upload", "-p", port,
                        "--fqbn", f"{FQBN}:UploadSpeed={UPLOAD_SPEED}", "--input-dir", build_dir], capture_output=True, text=True)
    if r.returncode != 0 or "Hash of data verified" not in r.stdout + r.stderr:
        log((r.stdout + r.stderr)[-2000:])
        return False
    return True


# --- монитор порта -------------------------------------------------------------

def monitor(port):
    """Сброс платы и чтение лога до первой самопроверки с подключённым Blynk."""
    res = {"lines": [], "selftest": None, "reset": None, "time": None, "sun": None,
           "blynk": False, "panic": False, "boots": 0, "pmu_err": False}
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = port, 115200, 0.2
    p.dtr = p.rts = False
    p.open()
    p.rts = True
    time.sleep(0.15)
    p.rts = False
    t0, buf = time.time(), b""
    try:
        while time.time() - t0 < BOOT_TIMEOUT:
            buf += p.read(4096)
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                line = raw.decode("utf-8", "replace").strip()
                res["lines"].append(line)
                if line.startswith("rst:"):
                    res["boots"] += 1
                if "Guru Meditation" in line or "Backtrace" in line:
                    res["panic"] = True
                if "AXP Power begin failed" in line or "Setting power error" in line:
                    res["pmu_err"] = True
                if line.startswith("Причина перезагрузки:"):
                    res["reset"] = line.split(":", 1)[1].strip()
                if line.startswith("Время с вышки:"):
                    res["time"] = line.split(":", 1)[1].strip()
                if line.startswith("Рассвет"):
                    res["sun"] = line
                if line == "Blynk подключен":
                    res["blynk"] = True
                if line.startswith("SELFTEST "):
                    try:
                        res["selftest"] = json.loads(line[9:])
                    except ValueError:
                        pass
                    if res["selftest"] and res["selftest"].get("blynk"):
                        return res
    except serial.SerialException as e:
        res["lines"].append(f"!! порт пропал через {time.time() - t0:.0f} с: {e}")
        res["lost"] = True
    finally:
        try:
            p.close()
        except Exception:
            pass
    return res


# --- итог ----------------------------------------------------------------------

def report(dev, mac, flashed, mon):
    rows = []                                  # (метка, текст)
    st = mon.get("selftest") or {}

    def chk(cond, good, bad, warn=False):
        rows.append((OK if cond else (WARN if warn else BAD), good if cond else bad))

    if flashed is not None:
        chk(flashed, "прошита", "прошивка не удалась")
    if mon.get("lost"):
        rows.append((BAD, "плата пропала с USB во время работы: питание/кабель/док"))
    if mon["panic"] or mon["boots"] > 1:
        rows.append((BAD, f"падение или перезагрузки по кругу ({mon['boots']} стартов)"))
    if not st:
        rows.append((BAD, f"самопроверка не пришла за {BOOT_TIMEOUT} с"))
    else:
        chk(st.get("pmu") and not mon["pmu_err"], "контроллер питания", "контроллер питания не отвечает")
        chk(st.get("at"), "модем", "модем не отвечает")
        chk(st.get("sim"), "SIM", "SIM не найдена")
        csq = st.get("csq", -1)
        chk(csq >= CSQ_WEAK, f"сигнал {csq}", f"сигнал {csq} — проверьте антенну", warn=csq > 0)
        chk(st.get("net"), "сеть GSM", "нет сети GSM")
        chk(st.get("time"), f"время {mon.get('time') or ''}".strip(), "нет времени с вышки")
        if mon.get("sun"):
            rows.append((OK, mon["sun"].lower()))
        chk(st.get("gprs"), "GPRS", "нет GPRS (APN?)")
        chk(st.get("blynk"), "Blynk", "Blynk не подключился")
        if st.get("ext", -1) >= 0:
            chk(st["ext"] == 1, "внешнее питание 5 В", "нет внешнего питания 5 В", warn=True)
        if st.get("pct", -1) >= 0:
            batt = f"{st['batt']:.2f} В / " if st.get("batt", 0) > 0 else ""
            chk(st["pct"] > 20, f"АКБ {batt}{st['pct']} %", f"АКБ {batt}{st['pct']} %", warn=True)
    if mon.get("reset"):
        rows.append((INFO, f"перезагрузка: {mon['reset']}"))

    # Дошло ли до Blynk (плата отправляет итог в течение секунды после самопроверки)
    time.sleep(3)
    try:
        token = load_tokens()[dev - 1]
        online = blynk_online(token)
        v = blynk_values(token)
        chk(online, "Blynk: устройство онлайн", "Blynk: устройство офлайн")
        if st.get("result"):
            chk(v.get("selftest") == st["result"], "Blynk: итог самопроверки получен",
                "Blynk: итог самопроверки ещё не обновился", warn=True)
    except Exception as e:
        rows.append((WARN, f"Blynk API не ответил: {e}"))

    bad = any(m == BAD for m, _ in rows)
    warn = any(m == WARN for m, _ in rows)
    log()
    log(f"Плата №{dev} ({mac})")
    for mark, text in rows:
        log(f"  {mark} {text}")
    if st.get("result"):
        log(f"  Самопроверка платы: {st['result']}")
    log("ИТОГ: " + ("ОШИБКА — см. ❌ выше" if bad else "ОК с замечаниями" if warn else "ОК"))
    if bad:
        log("Последние строки лога:")
        for line in mon["lines"][-15:]:
            log("  | " + line)
    return not bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-flash", action="store_true", help="не прошивать, только проверить")
    ap.add_argument("--once", action="store_true", help="одна плата и выход")
    ap.add_argument("--port", help="порт вручную")
    args = ap.parse_args()

    if not args.no_flash and not os.path.exists(CLI_CFG):
        raise SystemExit(f"Нет {CLI_CFG}: нужен ESP32 core 2.0.17, см. README, раздел «Проверка»")
    boards = load_boards()
    all_ok = True
    while True:
        port = args.port or wait_port()
        time.sleep(1)
        mac = read_mac(port)
        if not mac:
            log(f"Не удалось прочитать MAC на {port} — переподключите плату")
            if args.once:
                return 1
            wait_unplug(port)
            continue
        dev = boards.get(mac)
        if not dev:
            log(f"Плата {mac} не знакома — добавьте её в tools/boards.json с номером 1..4")
            if args.once:
                return 1
            wait_unplug(port)
            continue
        log(f"\n=== Плата №{dev} ({mac}) на {port} ===")
        flashed = None
        if not args.no_flash:
            flashed = upload(port, build(dev))
        mon = monitor(port) if flashed is not False else {"lines": [], "panic": False, "boots": 0, "pmu_err": False}
        all_ok &= report(dev, mac, flashed, mon)
        if args.once:
            return 0 if all_ok else 1
        wait_unplug(port)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print()
