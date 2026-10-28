"""Update the bridge over Wi-Fi.

Serves a firmware image over HTTP, publishes its URL to jungmesh/bridge/ota
and waits until the bridge reports back after reboot. If the bridge comes
back with a different version, the new image was rolled back.

    ~/.platformio/penv/bin/pio run && ~/.platformio/penv/bin/python tools/ota.py
    ~/.platformio/penv/bin/python tools/ota.py path/to/other.bin --timeout 600
"""
import argparse
import functools
import http.server
import struct
import sys
import threading
import time
from pathlib import Path

from common import ROOT, connect, local_ip_towards, mqtt_settings

APP_DESC_OFFSET = 24 + 8  # image header + first segment header
APP_DESC_MAGIC = 0xABCD5432


def image_version(path):
    """Version string as the bridge publishes it: "<version> (<date> <time>)"."""
    with open(path, "rb") as f:
        f.seek(APP_DESC_OFFSET)
        desc = f.read(4 + 4 + 8 + 32 + 32 + 16 + 16)
    if struct.unpack_from("<I", desc)[0] != APP_DESC_MAGIC:
        sys.exit(f"{path} does not look like an ESP-IDF app image")
    text = lambda off, n: desc[off:off + n].split(b"\0")[0].decode()
    return f"{text(16, 32)} ({text(96, 16)} {text(80, 16)})"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware", nargs="?", default=ROOT / ".pio" / "build" / "esp32dev" / "firmware.bin")
    parser.add_argument("--timeout", type=float, default=420, help="seconds, covers a rollback")
    args = parser.parse_args()

    firmware = Path(args.firmware)
    if not firmware.exists():
        sys.exit(f"{firmware} not found, run `pio run` first")
    expected = image_version(firmware)

    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=firmware.parent)
    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", 0), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    url = f"http://{local_ip_towards(mqtt_settings()[0])}:{httpd.server_port}/{firmware.name}"

    state = {"rebooting": False}
    done = threading.Event()

    def on_message(client, userdata, msg):
        payload = msg.payload.decode(errors="replace")
        print(f"{msg.topic}: {payload}", flush=True)
        if msg.topic.endswith("/ota/result") and payload not in ("started", "ok, rebooting"):
            state["error"] = payload
            done.set()
        elif msg.topic.endswith("/ota/result") and payload == "ok, rebooting":
            state["rebooting"] = True
        elif msg.topic.endswith("/bridge/version") and state["rebooting"]:
            state["version"] = payload
            done.set()

    client = connect(on_message)
    client.subscribe("jungmesh/bridge/version")
    client.subscribe("jungmesh/bridge/ota/result")
    client.loop_start()
    time.sleep(1)
    print(f"serving {expected}, {firmware.stat().st_size} bytes at {url}", flush=True)
    client.publish("jungmesh/bridge/ota", url, qos=1, retain=False)

    finished = done.wait(args.timeout)
    client.loop_stop()
    httpd.shutdown()
    if not finished:
        sys.exit("timed out waiting for the bridge")
    if "error" in state:
        sys.exit(f"OTA failed: {state['error']}")
    if state["version"] != expected:
        sys.exit(f"bridge came back with {state['version']}: the new image was rolled back")
    print("update done")


if __name__ == "__main__":
    main()
