# JUNG HOME mesh notes

Findings from running the bridge against JUNG HOME push-button inserts
(1-gang dimmer and 2-gang relay, firmware `02020002`). Everything here was
observed on a real installation; none of it comes from JUNG documentation.

## Network

- Standard Bluetooth SIG Mesh. Company ID `0x0527`.
- The JUNG app's project export is JSON. Its `network` field is a base64
  encoded Mesh Configuration Database (CDB 1.0.1) containing NetKey, AppKey,
  every node's DevKey, element addresses and model bindings.
- Each insert's primary element has the actuator (Generic OnOff Server
  `0x1000`, plus Level `0x1002` and Light Lightness `0x1300` on dimmers). The
  rocker is a separate element with client models.
- The app puts every actuator in its own group (`0xC005`, `0xC006`, ...).
  The actuator publishes its state there and the rocker element subscribes
  to it to colour its LED.

## Vendor parameters (status LED colours)

All messages use the AppKey and go to the node's primary element.

| Opcode | Direction | Payload |
| --- | --- | --- |
| `C2 27 05` Get | client → node | `param` (LE16) |
| `C3 27 05` Set | client → node | `param` (LE16) + value |
| `C5 27 05` Status | node → client | `param` (LE16) + value, or just `param` if unsupported |

LED colour value: `mode R G B X`, each colour 0..100.

- `mode` `0x01` = factory colour, `0x03` = user colour
- `X` is **not** a white channel. The app always writes 0. A non-zero value
  made one LED go dark about a minute after switching off, so the bridge
  always writes 0.

| Parameter | 1-gang | 2-gang |
| --- | --- | --- |
| `0xA001` | LED colour when on | left rocker, when on |
| `0xA002` | LED colour when off | left rocker, when off |
| `0xA004` | mirror of `0xA001` | right rocker, when on |
| `0xA005` | mirror of `0xA002` | right rocker, when off |

The app writes both mirrors on 1-gang inserts, and so does the bridge.

App palette as stored (R G B): red `64 00 00`, blue `00 04 64`,
yellow `60 4D 00`, turquoise `00 64 30`, orange `64 1B 00`,
green `04 64 00`, off `00 00 00`.

Parameter `0x0001` also exists (value `01 00 00` on all inserts) and is
probably one of the "lock operation" / "night mode" options. Not decoded.

## Quirks

**Target fields are unreliable.** Generic OnOff and Light Lightness status
messages include the optional target and remaining time with misleading
values: `present=1, target=0, remaining=1` while the light stays on, and a
target stuck at the minimum level while a rocker is held to dim. Use the
present value. Only trust the target while present is 0 and a transition is
running (fading up from off).

**Some inserts do not publish "off".** After a reconfiguration in the app,
one insert kept publishing its state when switched on but stayed silent when
switched off, so Home Assistant and the rocker LED kept showing "on". The
rocker accepts a Generic OnOff Status published to the group by another
node, so the bridge publishes it on the insert's behalf (`mirror_off` in
`src/lights.h`).

**App configuration bypasses the mesh.** The app talks to the insert it is
connected to directly over GATT, so its commands to that insert never appear
on the advertising bearer. To capture them, connect the phone to a different
insert (stand next to another switch and restart the app).

## Sniffing technique

ESP-BLE-MESH only passes messages addressed to the node itself up to the
access layer. `src/sniffer.c` wraps `bt_mesh_trans_recv()` with the linker's
`--wrap` option. Every network PDU the stack has already decrypted with the
NetKey passes through it, and the upper transport layer is decrypted again
with the AppKey or the relevant DevKey (AES-CCM via mbedTLS). Only
unsegmented access messages are decoded.
