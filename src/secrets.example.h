#pragma once

/* Copy to secrets.h and fill in. secrets.h is git-ignored. */

#define WIFI_SSID           "your-ssid"             /* 2.4 GHz */
#define WIFI_PASSWORD       "your-password"

/* Use Home Assistant's IP address, the ESP32 does not resolve .local names */
#define MQTT_URI            "mqtt://<home-assistant-ip>:1883"
#define MQTT_USER           "your-mqtt-user"
#define MQTT_PASSWORD       "your-mqtt-password"

/* Mesh keys: run `python3 tools/extract_keys.py JungHome.json` and paste here */
#define MESH_NET_KEY_HEX    "00000000000000000000000000000000"
#define MESH_APP_KEY_HEX    "00000000000000000000000000000000"

/* Device keys, only used by the sniffer: { first element, element count, key } */
#define MESH_DEV_KEYS { \
    { 0x0001, 1, "00000000000000000000000000000000" }, \
}
