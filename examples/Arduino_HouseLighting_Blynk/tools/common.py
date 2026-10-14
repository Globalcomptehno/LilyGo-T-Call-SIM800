"""Общее для tools/bench.py и tools/fleet.py: токены из secrets.h, запросы к Blynk, список плат."""

import json
import os
import re
import urllib.parse
import urllib.request

TOOLS = os.path.dirname(os.path.abspath(__file__))
SKETCH_DIR = os.path.dirname(TOOLS)
SECRETS = os.path.join(SKETCH_DIR, "secrets.h")
BOARDS_FILE = os.path.join(TOOLS, "boards.json")

# Сервер своего региона Blynk (Blynk.Console внизу страницы: Region FRA1 -> fra1.blynk.cloud)
BLYNK_SERVER = os.environ.get("BLYNK_SERVER", "https://fra1.blynk.cloud")

PINS = {
    "status": "V14",
    "sun": "V15",
    "batt": "V16",
    "usb": "V17",
    "acin": "V18",
    "csq": "V19",
    "reset": "V20",
    "pct": "V21",
    "selftest": "V22",
}


def load_tokens():
    """Токены устройств 1..4 из secrets.h (в git не попадает)."""
    with open(SECRETS, encoding="utf-8") as f:
        text = f.read()
    block = re.search(r"AUTH_TOKENS\s*\[\s*\d*\s*\]\s*=\s*\{(.*?)\}", text, re.S)
    if not block:
        raise SystemExit(f"Не нашёл AUTH_TOKENS в {SECRETS}")
    return re.findall(r'"([^"]+)"', block.group(1))


def load_boards():
    """MAC платы -> номер устройства. Файл tools/boards.json можно править."""
    with open(BOARDS_FILE, encoding="utf-8") as f:
        data = json.load(f)
    return {mac.lower(): int(dev) for mac, dev in data["boards"].items()}


def _get(path, params):
    url = f"{BLYNK_SERVER}/external/api/{path}?" + urllib.parse.urlencode(params, doseq=True)
    with urllib.request.urlopen(url, timeout=15) as r:
        return r.read().decode("utf-8")


def blynk_online(token):
    return _get("isHardwareConnected", {"token": token}).strip() == "true"


def blynk_values(token):
    """Текущие значения пинов из PINS: {"status": "...", "csq": "18", ...}."""
    query = [("token", token)] + [(v, "") for v in PINS.values()]
    url = f"{BLYNK_SERVER}/external/api/get?" + "&".join(
        f"{k}={urllib.parse.quote(v)}" if v else k for k, v in query)
    with urllib.request.urlopen(url, timeout=15) as r:
        raw = json.loads(r.read().decode("utf-8"))
    by_pin = {k.upper(): v for k, v in raw.items()}
    return {name: by_pin.get(pin) for name, pin in PINS.items()}


def num(v, default=None):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default
