# Setup guide

From an existing JUNG HOME installation to switches in Home Assistant in
about 30 minutes.

**Home Assistant ⇄ (Wi-Fi / MQTT) ⇄ ESP32 ⇄ (Bluetooth Mesh) ⇄ JUNG HOME switches**

## 1. MQTT broker in Home Assistant

1. **Settings → Add-ons → Add-on Store** (called **Apps** in recent versions),
   install **Mosquitto broker**.
2. Enable **Start on boot** and **Watchdog**, then **Start**.

If Home Assistant runs as a plain container (no add-ons), run Mosquitto as
its own container instead.

## 2. An MQTT user for the bridge

1. Enable **Advanced mode** in your user profile.
2. **Settings → People → Users → Add user**, e.g. username `mqtt-bridge`,
   not an administrator. Mosquitto accepts Home Assistant users.

## 3. MQTT integration

**Settings → Devices & services**: confirm the discovered **MQTT**
integration, or add it and pick the Mosquitto broker.

## 4. Home Assistant's IP address

**Settings → System → Network → IPv4 addresses**. Use the IP address, not
`homeassistant.local`: the ESP32 does not resolve mDNS names.

## 5. Mesh keys from the JUNG HOME app

1. In the JUNG HOME app, export the project and copy the `.json` file into
   the project folder as `JungHome.json`. It is git-ignored.
2. Run:
   ```bash
   python3 tools/extract_keys.py JungHome.json
   ```
   It prints the `MESH_*` lines for `src/secrets.h` and every switch channel
   with its element address, group and whether it dims.

Treat the export and `src/secrets.h` like a house key: anyone with them can
control every switch.

## 6. Configure the firmware

```bash
cp src/secrets.example.h src/secrets.h
```

Fill in Wi-Fi (2.4 GHz only), the MQTT URI with Home Assistant's IP, the
MQTT user from step 2, and the mesh lines from step 5.

Then edit the `LIGHTS` table in `src/lights.h` with your channels: the
element address and group come from `extract_keys.py`. For LED colours, 1-gang
inserts use parameters `0xA001/0xA004` (on) and `0xA002/0xA005` (off); a
2-gang insert uses `0xA001/0xA002` for the left rocker and `0xA004/0xA005`
for the right one, both on its primary element. See
[PROTOCOL.md](PROTOCOL.md).

Pick `MESH_BRIDGE_ADDR` in `src/lights.h` so it does not collide with any
node; the default `0x7F00` is far above what the app assigns.

## 7. Build and flash

With the ESP32 on USB:

```bash
pio run -t erase                 # first flash only, clears old mesh state
pio run -t upload -t monitor
```

If the upload hangs at `Connecting...`, hold the **BOOT** button until
writing starts.

Expected log:

```
joining JUNG mesh as 0x7f00
joined; rebooting to start as provisioned node
mesh node up, address 0x7f00
got IP ...
MQTT connected
Kitchen window is OFF
```

## 8. Home Assistant

**Settings → Devices & services → MQTT** now lists one device per channel,
each with the light and two configuration lights for its status LED. Areas
are suggested from `src/lights.h`.

To get brightness sliders on a dashboard, use a **Tile** card with the
**Light brightness** feature; Entities cards only show a toggle.

## 9. Placement

Power the ESP32 from any USB charger, roughly central to the switches,
not inside a metal cabinet. It reconnects by itself after a power cut.

## Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| `Wi-Fi disconnected, retrying` repeats | Wrong SSID or password, or a 5 GHz-only network. |
| `got IP` but no `MQTT connected` | Wrong IP, user or password, or the broker is stopped. Check the Mosquitto log for "not authorised". |
| `no answer from 0x....` | Out of range, or the network's IV index changed. Re-export, check `ivIndex`, update `MESH_IV_INDEX`, erase and flash. |
| Lights "Unavailable" | The ESP32 is offline or lost Wi-Fi. |
| No devices although MQTT is connected | Enable discovery in the MQTT integration options. |

To watch the traffic: **Settings → Devices & services → MQTT → Configure →
Listen to a topic** `jungmesh/#`.

Changing Wi-Fi or MQTT settings later only needs an upload (or
`tools/ota.py`). After changing mesh keys or the bridge address, erase first:
the mesh state lives in NVS.
