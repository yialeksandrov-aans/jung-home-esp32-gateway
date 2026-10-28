"""Talk to the JUNG mesh through the bridge's MQTT console.

Sends each JSON argument to jungmesh/debug/tx, then prints everything that
arrives on jungmesh/debug/rx for --seconds (default: until Ctrl+C).

    python tools/mesh_console.py '{"dst":"000F","prop":"admin"}'
    python tools/mesh_console.py --seconds 120          # just listen
"""
import argparse
import json
import time

from common import connect


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("messages", nargs="*")
    parser.add_argument("--seconds", type=float, default=None)
    parser.add_argument("--gap", type=float, default=1.5, help="pause between sends")
    args = parser.parse_args()

    start = time.time()

    def on_message(client, userdata, msg):
        print(f"{time.time() - start:7.1f}s {msg.payload.decode(errors='replace')}", flush=True)

    client = connect(on_message)
    client.subscribe("jungmesh/debug/rx")
    client.loop_start()
    time.sleep(0.5)
    for message in args.messages:
        json.loads(message)  # fail early on typos
        print(f"{time.time() - start:7.1f}s >> {message}", flush=True)
        client.publish("jungmesh/debug/tx", message)
        time.sleep(args.gap)
    try:
        while args.seconds is None or time.time() - start < args.seconds:
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    client.loop_stop()


if __name__ == "__main__":
    main()
