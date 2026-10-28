"""Shared helpers: MQTT settings come from src/secrets.h so they live in one place."""
import re
import socket
from pathlib import Path

import paho.mqtt.client as mqtt

ROOT = Path(__file__).resolve().parent.parent


def mqtt_settings():
    text = (ROOT / "src" / "secrets.h").read_text()

    def get(key):
        return re.search(rf'#define\s+{key}\s+"([^"]*)"', text).group(1)

    host, port = re.match(r"mqtt://([^:/]+):?(\d+)?", get("MQTT_URI")).groups()
    return host, int(port or 1883), get("MQTT_USER"), get("MQTT_PASSWORD")


def connect(on_message):
    host, port, user, password = mqtt_settings()
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.username_pw_set(user, password)
    client.on_message = on_message
    client.connect(host, port)
    return client


def local_ip_towards(host):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((host, 1883))
        return s.getsockname()[0]
