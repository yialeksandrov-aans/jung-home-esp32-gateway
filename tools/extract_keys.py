"""Read a JUNG HOME app project export and print what the bridge needs.

The export's "network" field is a base64 encoded Bluetooth Mesh
Configuration Database. This prints the MESH_* lines for src/secrets.h and
every switch channel (element address, group, dimmable) for src/lights.h.

    python3 tools/extract_keys.py JungHome.json
"""
import argparse
import base64
import json

ONOFF_SERVER = "1000"
LIGHTNESS_SERVER = "1300"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("export")
    args = parser.parse_args()

    with open(args.export) as f:
        export = json.load(f)
    net = json.loads(base64.b64decode(export["network"]))
    provisioners = {p["UUID"] for p in net.get("provisioners", [])}

    iv_index = max((e.get("ivIndex", 0) for e in net.get("networkExclusions", [])), default=0)
    print(f"// IV index in the export: {iv_index} (MESH_IV_INDEX in src/lights.h)\n")
    print(f'#define MESH_NET_KEY_HEX    "{net["netKeys"][0]["key"]}"')
    print(f'#define MESH_APP_KEY_HEX    "{net["appKeys"][0]["key"]}"')
    print("#define MESH_DEV_KEYS { \\")
    for node in net["nodes"]:
        addr = int(node["unicastAddress"], 16)
        print(f'    {{ 0x{addr:04X}, {len(node["elements"])}, "{node["deviceKey"]}" }}, \\')
    print("}\n")

    print("// Switch channels: element, publish group, dimmable")
    for node in net["nodes"]:
        if node["UUID"] in provisioners:
            continue
        base = int(node["unicastAddress"], 16)
        for element in node["elements"]:
            models = {m["modelId"]: m for m in element["models"]}
            if ONOFF_SERVER not in models:
                continue
            group = (models[ONOFF_SERVER].get("publish") or {}).get("address", "----")
            print(f"//   {node.get('name', '?'):<20} element 0x{base + element['index']:04X}"
                  f"  group 0x{group}  dimmable={LIGHTNESS_SERVER in models}"
                  f"  (node 0x{base:04X})")


if __name__ == "__main__":
    main()
