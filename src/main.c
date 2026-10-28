/*
 * JUNG HOME Bluetooth Mesh -> MQTT bridge for Home Assistant.
 *
 * The ESP32 joins the existing JUNG mesh as an ordinary node using the
 * NetKey/AppKey exported from the JUNG app (no provisioner, no gateway) and
 * talks to the switches with standard SIG models:
 *   Generic OnOff Client     -> relays and dimmers (on/off)
 *   Light Lightness Client   -> dimmers (brightness)
 * Lights appear in Home Assistant via MQTT discovery.
 *
 * Mesh console (for reverse engineering JUNG features such as LED colours):
 *   jungmesh/debug/rx  <- every JUNG vendor message / property status we hear
 *   jungmesh/debug/tx  -> {"dst":"000F","op":"C02705","data":"01"}  raw send
 *                         {"dst":"000F","prop":"admin","id":"0001"} property get
 *   jungmesh/debug/sniff  -> "on"/"off": also decode traffic between other nodes
 * Firmware update over Wi-Fi: publish a http:// URL to jungmesh/bridge/ota.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_lighting_model_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"

#include "secrets.h"
#include "lights.h"
#include "sniffer.h"

#define TAG "jung-bridge"

#define TOPIC_PREFIX        "jungmesh"
#define TOPIC_AVAILABILITY  TOPIC_PREFIX "/bridge/status"
#define TOPIC_HA_STATUS     "homeassistant/status"
#define TOPIC_VERSION       TOPIC_PREFIX "/bridge/version"
#define TOPIC_OTA           TOPIC_PREFIX "/bridge/ota"
#define TOPIC_DEBUG_RX      TOPIC_PREFIX "/debug/rx"
#define TOPIC_DEBUG_TX      TOPIC_PREFIX "/debug/tx"
#define TOPIC_DEBUG_SNIFF   TOPIC_PREFIX "/debug/sniff"

#define JUNG_CID            0x0527
#define CONSOLE_VND_MODEL   0x1015  /* same vendor client model id the JUNG app uses */
#define JUNG_GROUP_FIRST    0xC000
#define JUNG_GROUP_LAST     0xC012

/* JUNG vendor parameter access (reverse engineered from the JUNG app):
 *   Get    C2: param id (LE16)
 *   Set    C3: param id (LE16) + value
 *   Status C5: param id (LE16) + value  (param id only = not supported) */
#define JUNG_OP_PARAM_GET       ESP_BLE_MESH_MODEL_OP_3(0x02, JUNG_CID)
#define JUNG_OP_PARAM_SET       ESP_BLE_MESH_MODEL_OP_3(0x03, JUNG_CID)
#define JUNG_OP_PARAM_STATUS    ESP_BLE_MESH_MODEL_OP_3(0x05, JUNG_CID)
#define JUNG_LED_USER_COLOUR    0x03
#define LED_POLL_EVERY          5   /* light poll rounds (~1 min each) */

#define OTA_VERIFY_TIMEOUT_MS   (5 * 60 * 1000)
#define REFRESH_AFTER_CMD_MS    2500
#define POLL_INTERVAL_MS    60000
#define POLL_SPACING_MS     2000
#define MESH_SET_ATTEMPTS   3

/* Not part of the public ESP-BLE-MESH API, but exported by the bt component.
 * It is the same call the stack makes at the end of provisioning; using it
 * directly lets us join a network whose keys we already know. */
extern int bt_mesh_provision(const uint8_t net_key[16], uint16_t net_idx,
                             uint8_t flags, uint32_t iv_index, uint16_t addr,
                             const uint8_t dev_key[16]);

typedef struct {
    bool known;
    bool on;
    uint8_t brightness; /* 1..255, last non-zero level */
} light_state_t;

typedef struct {
    bool active;
    bool lightness;     /* Light Lightness Set, else Generic OnOff Set */
    uint16_t value;
    uint8_t tid;
    uint8_t tries;
} pending_cmd_t;

typedef struct {
    bool known;
    bool on;
    uint8_t rgb[3];     /* full-scale colour, 0..255 */
    uint8_t brightness; /* 1..255 */
} led_state_t;

static light_state_t states[LIGHT_COUNT];
static pending_cmd_t pending[LIGHT_COUNT];
static led_state_t leds[LIGHT_COUNT][2];
static esp_mqtt_client_handle_t mqtt;
static bool mqtt_connected;
static uint8_t mesh_tid;

static SemaphoreHandle_t setup_done;
static int setup_err;

/* ---------- BLE Mesh composition ---------- */

static uint8_t dev_uuid[16];

static esp_ble_mesh_cfg_srv_t config_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_NOT_SUPPORTED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl = MESH_TTL,
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_cli_pub, 2 + 1, ROLE_NODE);
ESP_BLE_MESH_MODEL_PUB_DEFINE(lightness_cli_pub, 2 + 3, ROLE_NODE);

static esp_ble_mesh_client_t onoff_client;
static esp_ble_mesh_client_t lightness_client;
static esp_ble_mesh_client_t property_client;

/* New models are appended so the indices of the ones already stored in NVS
 * (bindings, subscriptions) stay the same. */
static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(&onoff_cli_pub, &onoff_client),
    ESP_BLE_MESH_MODEL_LIGHT_LIGHTNESS_CLI(&lightness_cli_pub, &lightness_client),
    ESP_BLE_MESH_MODEL_GEN_PROPERTY_CLI(NULL, &property_client),
};

/* All 64 possible JUNG vendor opcodes */
#define JUNG_OP(n)      ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_MODEL_OP_3(n, JUNG_CID), 0)
#define JUNG_OPS4(n)    JUNG_OP(n), JUNG_OP(n + 1), JUNG_OP(n + 2), JUNG_OP(n + 3)
#define JUNG_OPS16(n)   JUNG_OPS4(n), JUNG_OPS4(n + 4), JUNG_OPS4(n + 8), JUNG_OPS4(n + 12)

static esp_ble_mesh_model_op_t jung_vnd_ops[] = {
    JUNG_OPS16(0), JUNG_OPS16(16), JUNG_OPS16(32), JUNG_OPS16(48),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(JUNG_CID, CONSOLE_VND_MODEL, jung_vnd_ops, NULL, NULL),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, vnd_models),
};

static esp_ble_mesh_comp_t composition = {
    .cid = 0x02E5, /* Espressif */
    .element_count = ARRAY_SIZE(elements),
    .elements = elements,
};

static esp_ble_mesh_prov_t provision = {
    .uuid = dev_uuid,
};

/* ---------- helpers ---------- */

static bool hex_to_key(const char *hex, uint8_t out[16])
{
    if (strlen(hex) != 32) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        unsigned int b;
        if (sscanf(hex + 2 * i, "%2x", &b) != 1) {
            return false;
        }
        out[i] = (uint8_t)b;
    }
    return true;
}

static void bytes_to_hex(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    size_t n = 0;
    out[0] = '\0';
    for (size_t i = 0; i < len && n + 3 <= out_size; i++) {
        n += snprintf(out + n, out_size - n, "%02X", data[i]);
    }
}

/* Returns number of bytes parsed, or -1 on bad input */
static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_size)
{
    size_t len = strlen(hex);
    if (len % 2 || len / 2 > out_size) {
        return -1;
    }
    for (size_t i = 0; i < len / 2; i++) {
        unsigned int b;
        if (sscanf(hex + 2 * i, "%2x", &b) != 1) {
            return -1;
        }
        out[i] = (uint8_t)b;
    }
    return len / 2;
}

static int light_by_addr(uint16_t addr)
{
    for (int i = 0; i < LIGHT_COUNT; i++) {
        if (LIGHTS[i].addr == addr) {
            return i;
        }
    }
    return -1;
}

static int light_by_id(const char *id, size_t len)
{
    for (int i = 0; i < LIGHT_COUNT; i++) {
        if (strlen(LIGHTS[i].id) == len && strncmp(LIGHTS[i].id, id, len) == 0) {
            return i;
        }
    }
    return -1;
}

static uint8_t lightness_to_brightness(uint16_t lightness)
{
    uint32_t b = ((uint32_t)lightness * 255 + 32767) / 65535;
    return b ? b : 1;
}

static uint16_t brightness_to_lightness(uint8_t brightness)
{
    return ((uint32_t)brightness * 65535 + 127) / 255;
}

/* ---------- MQTT publishing ---------- */

static void publish_state(int i)
{
    if (!mqtt_connected || !states[i].known) {
        return;
    }
    char topic[64];
    char payload[64];
    snprintf(topic, sizeof(topic), TOPIC_PREFIX "/%s/state", LIGHTS[i].id);
    if (LIGHTS[i].dimmable) {
        snprintf(payload, sizeof(payload), "{\"state\":\"%s\",\"brightness\":%u}",
                 states[i].on ? "ON" : "OFF", states[i].brightness);
    } else {
        snprintf(payload, sizeof(payload), "{\"state\":\"%s\"}", states[i].on ? "ON" : "OFF");
    }
    esp_mqtt_client_enqueue(mqtt, topic, payload, 0, 1, 1, true);
}

/* ---------- Status LED colours ---------- */

/* Each switch LED is exposed as an RGB light. The insert stores
 * mode + R,G,B,X in 0..100; mode 0x01 = factory colour, 0x03 = user colour.
 * X is not white: the JUNG app never sets it, and a non-zero X made the
 * living-room LED go dark after ~1 min, so it is always written as 0. */

#define LED_ON      0
#define LED_OFF     1

static const char *const LED_KIND[2] = { "led_on", "led_off" };
static const char *const LED_NAME[2] = { "LED when on", "LED when off" };

static const uint16_t *led_params(int i, int k)
{
    return k == LED_ON ? LIGHTS[i].led_on : LIGHTS[i].led_off;
}

static esp_err_t jung_send(uint16_t node, uint32_t opcode, uint8_t *data, uint16_t len)
{
    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx = MESH_NET_IDX,
        .app_idx = MESH_APP_IDX,
        .addr = node,
        .send_ttl = MESH_TTL,
    };
    return esp_ble_mesh_server_model_send_msg(&vnd_models[0], &ctx, opcode, len, data);
}

static void led_publish_state(int i, int k)
{
    const led_state_t *s = &leds[i][k];
    if (!mqtt_connected || !s->known) {
        return;
    }
    char topic[64];
    char payload[128];
    snprintf(topic, sizeof(topic), TOPIC_PREFIX "/%s/%s/state", LIGHTS[i].id, LED_KIND[k]);
    if (s->on) {
        snprintf(payload, sizeof(payload),
                 "{\"state\":\"ON\",\"brightness\":%u,\"color_mode\":\"rgb\","
                 "\"color\":{\"r\":%u,\"g\":%u,\"b\":%u}}",
                 s->brightness, s->rgb[0], s->rgb[1], s->rgb[2]);
    } else {
        snprintf(payload, sizeof(payload), "{\"state\":\"OFF\"}");
    }
    esp_mqtt_client_enqueue(mqtt, topic, payload, 0, 1, 1, true);
}

static void led_get(int i, int k)
{
    uint16_t p = led_params(i, k)[0];
    uint8_t data[2] = { p & 0xFF, p >> 8 };
    jung_send(LIGHTS[i].led_node, JUNG_OP_PARAM_GET, data, sizeof(data));
}

static void led_apply(int i, int k)
{
    const led_state_t *s = &leds[i][k];
    uint8_t data[7] = { 0, 0, JUNG_LED_USER_COLOUR, 0, 0, 0, 0 };

    for (int c = 0; c < 3; c++) {
        /* colour (0..255) x brightness (0..255) -> 0..100 */
        data[3 + c] = s->on ? ((uint32_t)s->rgb[c] * s->brightness * 100 + 255 * 255 / 2) / (255 * 255) : 0;
    }
    /* Sent twice: only the first parameter is read back, so a lost write to
     * the mirrored second one would otherwise go unnoticed. The value is
     * absolute, so repeating it is harmless. */
    for (int round = 0; round < 2; round++) {
        for (int n = 0; n < 2; n++) {
            uint16_t p = led_params(i, k)[n];
            if (!p) {
                continue;
            }
            data[0] = p & 0xFF;
            data[1] = p >> 8;
            esp_err_t err = jung_send(LIGHTS[i].led_node, JUNG_OP_PARAM_SET, data, sizeof(data));
            ESP_LOGI(TAG, "%s %s -> %02X%02X%02X (%s)", LIGHTS[i].name, LED_NAME[k],
                     data[3], data[4], data[5], esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

static void handle_led_status(uint16_t src, const uint8_t *d, uint16_t len)
{
    if (len < 7) {
        return;
    }
    uint16_t p = d[0] | (d[1] << 8);
    for (int i = 0; i < LIGHT_COUNT; i++) {
        for (int k = 0; k < 2; k++) {
            if (LIGHTS[i].led_node != src || led_params(i, k)[0] != p) {
                continue;
            }
            led_state_t *s = &leds[i][k];
            uint8_t max = 0;
            for (int c = 0; c < 3; c++) {
                max = d[3 + c] > max ? d[3 + c] : max;
            }
            s->known = true;
            s->on = max > 0;
            if (s->on) {
                /* split into full-scale colour + brightness, as HA expects */
                for (int c = 0; c < 3; c++) {
                    s->rgb[c] = (d[3 + c] * 255 + max / 2) / max;
                }
                s->brightness = max >= 100 ? 255 : (max * 255 + 50) / 100;
            }
            led_publish_state(i, k);
        }
    }
}

static void handle_led_command(int i, int k, const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        return;
    }
    led_state_t *s = &leds[i][k];
    const cJSON *state = cJSON_GetObjectItem(root, "state");
    const cJSON *color = cJSON_GetObjectItem(root, "color");
    const cJSON *bri = cJSON_GetObjectItem(root, "brightness");

    s->on = !cJSON_IsString(state) || strcmp(state->valuestring, "OFF") != 0;
    if (cJSON_IsObject(color)) {
        const char *ch[3] = { "r", "g", "b" };
        for (int c = 0; c < 3; c++) {
            const cJSON *v = cJSON_GetObjectItem(color, ch[c]);
            s->rgb[c] = cJSON_IsNumber(v) ? v->valueint : 0;
        }
    }
    if (cJSON_IsNumber(bri)) {
        s->brightness = bri->valueint;
    }
    if (s->on && !(s->rgb[0] | s->rgb[1] | s->rgb[2])) {
        s->rgb[0] = s->rgb[1] = s->rgb[2] = 255; /* nothing known yet: white */
    }
    if (s->on && s->brightness == 0) {
        s->brightness = 255;
    }
    s->known = true;
    cJSON_Delete(root);

    led_publish_state(i, k); /* optimistic; the switch's status confirms it */
    led_apply(i, k);
}

static void publish_led_discovery(int i)
{
    const jung_light_t *l = &LIGHTS[i];
    for (int k = 0; k < 2; k++) {
        char topic[96];
        char buf[96];
        cJSON *root = cJSON_CreateObject();

        cJSON_AddStringToObject(root, "name", LED_NAME[k]);
        snprintf(buf, sizeof(buf), "jungmesh_%04x_%s", l->addr, LED_KIND[k]);
        cJSON_AddStringToObject(root, "unique_id", buf);
        cJSON_AddStringToObject(root, "schema", "json");
        snprintf(buf, sizeof(buf), TOPIC_PREFIX "/%s/%s/set", l->id, LED_KIND[k]);
        cJSON_AddStringToObject(root, "command_topic", buf);
        snprintf(buf, sizeof(buf), TOPIC_PREFIX "/%s/%s/state", l->id, LED_KIND[k]);
        cJSON_AddStringToObject(root, "state_topic", buf);
        cJSON_AddStringToObject(root, "availability_topic", TOPIC_AVAILABILITY);
        cJSON *modes = cJSON_AddArrayToObject(root, "supported_color_modes");
        cJSON_AddItemToArray(modes, cJSON_CreateString("rgb"));
        cJSON_AddTrueToObject(root, "brightness");
        cJSON_AddStringToObject(root, "entity_category", "config");
        cJSON_AddStringToObject(root, "icon", "mdi:led-on");
        cJSON *dev = cJSON_AddObjectToObject(root, "device");
        cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
        snprintf(buf, sizeof(buf), "jungmesh_%04x", l->addr);
        cJSON_AddItemToArray(ids, cJSON_CreateString(buf));

        char *json = cJSON_PrintUnformatted(root);
        snprintf(topic, sizeof(topic), "homeassistant/light/jungmesh_%s_%s/config", l->id, LED_KIND[k]);
        esp_mqtt_client_enqueue(mqtt, topic, json, 0, 1, 1, true);
        cJSON_free(json);
        cJSON_Delete(root);
        led_publish_state(i, k);
    }
}

static void publish_discovery(void)
{
    for (int i = 0; i < LIGHT_COUNT; i++) {
        const jung_light_t *l = &LIGHTS[i];
        char topic[96];
        char buf[64];

        publish_led_discovery(i);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddNullToObject(root, "name"); /* entity takes the device name */
        snprintf(buf, sizeof(buf), "jungmesh_%04x", l->addr);
        cJSON_AddStringToObject(root, "unique_id", buf);
        cJSON_AddStringToObject(root, "schema", "json");
        snprintf(buf, sizeof(buf), TOPIC_PREFIX "/%s/set", l->id);
        cJSON_AddStringToObject(root, "command_topic", buf);
        snprintf(buf, sizeof(buf), TOPIC_PREFIX "/%s/state", l->id);
        cJSON_AddStringToObject(root, "state_topic", buf);
        cJSON_AddStringToObject(root, "availability_topic", TOPIC_AVAILABILITY);
        cJSON *modes = cJSON_AddArrayToObject(root, "supported_color_modes");
        cJSON_AddItemToArray(modes, cJSON_CreateString(l->dimmable ? "brightness" : "onoff"));
        if (l->dimmable) {
            cJSON_AddTrueToObject(root, "brightness");
        }

        cJSON *dev = cJSON_AddObjectToObject(root, "device");
        cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
        snprintf(buf, sizeof(buf), "jungmesh_%04x", l->addr);
        cJSON_AddItemToArray(ids, cJSON_CreateString(buf));
        cJSON_AddStringToObject(dev, "name", l->name);
        cJSON_AddStringToObject(dev, "manufacturer", "JUNG");
        cJSON_AddStringToObject(dev, "model", l->dimmable ? "JUNG HOME dimmer" : "JUNG HOME switch");
        cJSON_AddStringToObject(dev, "suggested_area", l->area);

        char *json = cJSON_PrintUnformatted(root);
        snprintf(topic, sizeof(topic), "homeassistant/light/jungmesh_%s/config", l->id);
        esp_mqtt_client_enqueue(mqtt, topic, json, 0, 1, 1, true);
        cJSON_free(json);
        cJSON_Delete(root);
    }
}

/* ---------- BLE Mesh commands ---------- */

static void fill_common(esp_ble_mesh_client_common_param_t *common, esp_ble_mesh_model_t *model,
                        uint32_t opcode, uint16_t addr)
{
    memset(common, 0, sizeof(*common));
    common->opcode = opcode;
    common->model = model;
    common->ctx.net_idx = MESH_NET_IDX;
    common->ctx.app_idx = MESH_APP_IDX;
    common->ctx.addr = addr;
    common->ctx.send_ttl = MESH_TTL;
    common->msg_timeout = 0; /* use the menuconfig default */
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    common->msg_role = ROLE_NODE;
#endif
}

/* Wi-Fi and BLE share one radio on the ESP32, so a Set or its Status can get
 * lost. Unanswered Sets are resent with the same TID, which the switch
 * treats as the same transaction. */
static void send_pending(int i)
{
    pending_cmd_t *p = &pending[i];
    esp_ble_mesh_client_common_param_t common;
    esp_err_t err;

    p->tries++;
    if (p->lightness) {
        esp_ble_mesh_light_client_set_state_t set = {0};
        fill_common(&common, lightness_client.model, ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET, LIGHTS[i].addr);
        set.lightness_set.lightness = p->value;
        set.lightness_set.tid = p->tid;
        err = esp_ble_mesh_light_client_set_state(&common, &set);
    } else {
        esp_ble_mesh_generic_client_set_state_t set = {0};
        fill_common(&common, onoff_client.model, ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET, LIGHTS[i].addr);
        set.onoff_set.onoff = p->value;
        set.onoff_set.tid = p->tid;
        err = esp_ble_mesh_generic_client_set_state(&common, &set);
    }
    ESP_LOGI(TAG, "%s -> %s %u, try %u (%s)", LIGHTS[i].name, p->lightness ? "lightness" : "onoff",
             p->value, p->tries, esp_err_to_name(err));
}

static void mesh_set(int i, bool lightness, uint16_t value)
{
    pending[i] = (pending_cmd_t) {
        .active = true,
        .lightness = lightness,
        .value = value,
        .tid = mesh_tid++,
    };
    send_pending(i);
}

static void set_answered(uint16_t addr)
{
    int i = light_by_addr(addr);
    if (i >= 0) {
        pending[i].active = false;
    }
}

static void set_timed_out(uint16_t addr)
{
    int i = light_by_addr(addr);
    if (i < 0 || !pending[i].active) {
        return;
    }
    if (pending[i].tries >= MESH_SET_ATTEMPTS) {
        ESP_LOGW(TAG, "%s: giving up after %u tries", LIGHTS[i].name, pending[i].tries);
        pending[i].active = false;
        return;
    }
    send_pending(i);
}

static void mesh_get_state(int i)
{
    esp_ble_mesh_client_common_param_t common;
    esp_err_t err;

    if (LIGHTS[i].dimmable) {
        esp_ble_mesh_light_client_get_state_t get = {0};
        fill_common(&common, lightness_client.model, ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_GET, LIGHTS[i].addr);
        err = esp_ble_mesh_light_client_get_state(&common, &get);
    } else {
        esp_ble_mesh_generic_client_get_state_t get = {0};
        fill_common(&common, onoff_client.model, ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET, LIGHTS[i].addr);
        err = esp_ble_mesh_generic_client_get_state(&common, &get);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get %s failed: %s", LIGHTS[i].name, esp_err_to_name(err));
    }
}

/* Not every switch publishes its new state (the living-room insert stays
 * silent when switched off), so when the sniffer sees a rocker or the JUNG
 * app command a light, re-read that light shortly afterwards. */
static TimerHandle_t refresh_timers[LIGHT_COUNT];

static void refresh_timer_cb(TimerHandle_t timer)
{
    mesh_get_state((int)(intptr_t)pvTimerGetTimerID(timer));
}

static void on_foreign_light_cmd(uint16_t dst)
{
    for (int i = 0; i < LIGHT_COUNT; i++) {
        if (LIGHTS[i].addr == dst || LIGHTS[i].group == dst) {
            xTimerReset(refresh_timers[i], 0);
        }
    }
}

static void refresh_timers_init(void)
{
    for (int i = 0; i < LIGHT_COUNT; i++) {
        refresh_timers[i] = xTimerCreate("refresh", pdMS_TO_TICKS(REFRESH_AFTER_CMD_MS), pdFALSE,
                                         (void *)(intptr_t)i, refresh_timer_cb);
    }
}

/* ---------- Mesh console ---------- */

static cJSON *console_msg(const esp_ble_mesh_msg_ctx_t *ctx, uint32_t opcode)
{
    char buf[16];
    cJSON *root = cJSON_CreateObject();
    snprintf(buf, sizeof(buf), "%04X", ctx->addr);
    cJSON_AddStringToObject(root, "src", buf);
    snprintf(buf, sizeof(buf), "%04X", ctx->recv_dst);
    cJSON_AddStringToObject(root, "dst", buf);
    snprintf(buf, sizeof(buf), "%" PRIX32, opcode);
    cJSON_AddStringToObject(root, "op", buf);
    cJSON_AddNumberToObject(root, "rssi", ctx->recv_rssi);
    return root;
}

static void console_add_hex(cJSON *root, const char *key, const uint8_t *data, size_t len)
{
    char *hex = malloc(2 * len + 1);
    if (hex) {
        bytes_to_hex(data, len, hex, 2 * len + 1);
        cJSON_AddStringToObject(root, key, hex);
        free(hex);
    }
}

static void console_publish(cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "console: %s", json);
    if (mqtt_connected) {
        esp_mqtt_client_enqueue(mqtt, TOPIC_DEBUG_RX, json, 0, 0, 0, true);
    }
    cJSON_free(json);
    cJSON_Delete(root);
}

static void custom_model_cb(esp_ble_mesh_model_cb_event_t event, esp_ble_mesh_model_cb_param_t *param)
{
    if (event == ESP_BLE_MESH_MODEL_OPERATION_EVT) {
        if (param->model_operation.opcode == JUNG_OP_PARAM_STATUS) {
            handle_led_status(param->model_operation.ctx->addr, param->model_operation.msg,
                              param->model_operation.length);
        }
        cJSON *root = console_msg(param->model_operation.ctx, param->model_operation.opcode);
        console_add_hex(root, "data", param->model_operation.msg, param->model_operation.length);
        console_publish(root);
    } else if (event == ESP_BLE_MESH_MODEL_SEND_COMP_EVT && param->model_send_comp.err_code) {
        ESP_LOGW(TAG, "raw send 0x%" PRIX32 " failed: %d",
                 param->model_send_comp.opcode, param->model_send_comp.err_code);
    }
}

/* Returns false if this is not a Generic Property message */
static bool handle_property_status(esp_ble_mesh_generic_client_cb_param_t *param)
{
    esp_ble_mesh_gen_client_status_cb_t *st = &param->status_cb;
    struct net_buf_simple *ids = NULL, *value = NULL;
    uint16_t id = 0;
    const char *kind;

    switch (param->params->opcode) {
    case ESP_BLE_MESH_MODEL_OP_GEN_ADMIN_PROPERTIES_GET:
    case ESP_BLE_MESH_MODEL_OP_GEN_ADMIN_PROPERTIES_STATUS:
        kind = "admin";
        ids = st->admin_properties_status.property_ids;
        break;
    case ESP_BLE_MESH_MODEL_OP_GEN_ADMIN_PROPERTY_GET:
    case ESP_BLE_MESH_MODEL_OP_GEN_ADMIN_PROPERTY_STATUS:
        kind = "admin";
        id = st->admin_property_status.property_id;
        value = st->admin_property_status.property_value;
        break;
    case ESP_BLE_MESH_MODEL_OP_GEN_MANUFACTURER_PROPERTIES_GET:
    case ESP_BLE_MESH_MODEL_OP_GEN_MANUFACTURER_PROPERTIES_STATUS:
        kind = "manufacturer";
        ids = st->manufacturer_properties_status.property_ids;
        break;
    case ESP_BLE_MESH_MODEL_OP_GEN_MANUFACTURER_PROPERTY_GET:
    case ESP_BLE_MESH_MODEL_OP_GEN_MANUFACTURER_PROPERTY_STATUS:
        kind = "manufacturer";
        id = st->manufacturer_property_status.property_id;
        value = st->manufacturer_property_status.property_value;
        break;
    case ESP_BLE_MESH_MODEL_OP_GEN_USER_PROPERTIES_GET:
    case ESP_BLE_MESH_MODEL_OP_GEN_USER_PROPERTIES_STATUS:
        kind = "user";
        ids = st->user_properties_status.property_ids;
        break;
    case ESP_BLE_MESH_MODEL_OP_GEN_USER_PROPERTY_GET:
    case ESP_BLE_MESH_MODEL_OP_GEN_USER_PROPERTY_STATUS:
        kind = "user";
        id = st->user_property_status.property_id;
        value = st->user_property_status.property_value;
        break;
    default:
        return false;
    }

    cJSON *root = console_msg(&param->params->ctx, param->params->opcode);
    cJSON_AddStringToObject(root, "prop", kind);
    if (param->error_code) {
        cJSON_AddNumberToObject(root, "error", param->error_code);
    } else if (ids) {
        cJSON *list = cJSON_AddArrayToObject(root, "ids");
        for (int i = 0; i + 1 < ids->len; i += 2) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%04X", ids->data[i] | (ids->data[i + 1] << 8));
            cJSON_AddItemToArray(list, cJSON_CreateString(buf));
        }
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%04X", id);
        cJSON_AddStringToObject(root, "id", buf);
        if (value) {
            console_add_hex(root, "value", value->data, value->len);
        }
    }
    console_publish(root);
    return true;
}

static void console_tx(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "console: bad JSON");
        return;
    }
    const cJSON *dst = cJSON_GetObjectItem(root, "dst");
    const cJSON *op = cJSON_GetObjectItem(root, "op");
    const cJSON *prop = cJSON_GetObjectItem(root, "prop");
    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const cJSON *hex = cJSON_GetObjectItem(root, "data");
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (!cJSON_IsString(dst)) {
        goto out;
    }
    uint16_t addr = strtoul(dst->valuestring, NULL, 16);

    if (cJSON_IsString(prop)) {
        bool one = cJSON_IsString(id);
        uint16_t pid = one ? strtoul(id->valuestring, NULL, 16) : 0;
        esp_ble_mesh_generic_client_get_state_t get = {0};
        uint32_t opcode;

        if (strcmp(prop->valuestring, "admin") == 0) {
            opcode = one ? ESP_BLE_MESH_MODEL_OP_GEN_ADMIN_PROPERTY_GET : ESP_BLE_MESH_MODEL_OP_GEN_ADMIN_PROPERTIES_GET;
            get.admin_property_get.property_id = pid;
        } else if (strcmp(prop->valuestring, "manufacturer") == 0) {
            opcode = one ? ESP_BLE_MESH_MODEL_OP_GEN_MANUFACTURER_PROPERTY_GET : ESP_BLE_MESH_MODEL_OP_GEN_MANUFACTURER_PROPERTIES_GET;
            get.manufacturer_property_get.property_id = pid;
        } else if (strcmp(prop->valuestring, "user") == 0) {
            opcode = one ? ESP_BLE_MESH_MODEL_OP_GEN_USER_PROPERTY_GET : ESP_BLE_MESH_MODEL_OP_GEN_USER_PROPERTIES_GET;
            get.user_property_get.property_id = pid;
        } else {
            goto out;
        }
        esp_ble_mesh_client_common_param_t common;
        fill_common(&common, property_client.model, opcode, addr);
        err = esp_ble_mesh_generic_client_get_state(&common, &get);
    } else if (cJSON_IsString(op)) {
        uint32_t opcode = strtoul(op->valuestring, NULL, 16);
        uint8_t buf[64];
        int n = cJSON_IsString(hex) ? hex_to_bytes(hex->valuestring, buf, sizeof(buf)) : 0;
        if (n < 0) {
            goto out;
        }
        esp_ble_mesh_msg_ctx_t ctx = {
            .net_idx = MESH_NET_IDX,
            .app_idx = MESH_APP_IDX,
            .addr = addr,
            .send_ttl = MESH_TTL,
        };
        err = esp_ble_mesh_server_model_send_msg(&vnd_models[0], &ctx, opcode, n, buf);
    }
out:
    ESP_LOGI(TAG, "console tx %.*s -> %s", len, data, esp_err_to_name(err));
    cJSON_Delete(root);
}

/* ---------- OTA ---------- */

static char ota_url[256];

static void ota_task(void *arg)
{
    ESP_LOGI(TAG, "OTA from %s", ota_url);
    esp_mqtt_client_publish(mqtt, TOPIC_OTA "/result", "started", 0, 1, 0);

    esp_http_client_config_t http = {
        .url = ota_url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = {
        .http_config = &http,
    };
    esp_err_t err = esp_https_ota(&cfg);
    if (err == ESP_OK) {
        esp_mqtt_client_publish(mqtt, TOPIC_OTA "/result", "ok, rebooting", 0, 1, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    esp_mqtt_client_publish(mqtt, TOPIC_OTA "/result", esp_err_to_name(err), 0, 1, 0);
    vTaskDelete(NULL);
}

/* After an OTA the bootloader starts the new image in PENDING_VERIFY. It only
 * becomes permanent once it reaches MQTT (so it can take the next OTA);
 * a crash before that, or this timeout, boots the previous image again. */
static volatile bool ota_pending_verify;

static void ota_verify_timeout(TimerHandle_t timer)
{
    if (ota_pending_verify) {
        ESP_LOGE(TAG, "new firmware never reached MQTT, rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

static void ota_check_pending(void)
{
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ota_pending_verify = true;
        ESP_LOGW(TAG, "new firmware on trial, must reach MQTT within %d s", OTA_VERIFY_TIMEOUT_MS / 1000);
        xTimerStart(xTimerCreate("ota_verify", pdMS_TO_TICKS(OTA_VERIFY_TIMEOUT_MS), pdFALSE, NULL,
                                 ota_verify_timeout), 0);
    }
}

static void ota_confirm(void)
{
    if (ota_pending_verify) {
        ota_pending_verify = false;
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed");
    }
}

/* ---------- BLE Mesh callbacks ---------- */

static void prov_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT:
        setup_err = param->node_prov_enable_comp.err_code;
        xSemaphoreGive(setup_done);
        break;
    case ESP_BLE_MESH_NODE_ADD_LOCAL_APP_KEY_COMP_EVT:
        setup_err = param->node_add_app_key_comp.err_code;
        xSemaphoreGive(setup_done);
        break;
    case ESP_BLE_MESH_NODE_BIND_APP_KEY_TO_MODEL_COMP_EVT:
        setup_err = param->node_bind_app_key_to_model_comp.err_code;
        xSemaphoreGive(setup_done);
        break;
    case ESP_BLE_MESH_MODEL_SUBSCRIBE_GROUP_ADDR_COMP_EVT:
        setup_err = param->model_sub_group_addr_comp.err_code;
        xSemaphoreGive(setup_done);
        break;
    default:
        break;
    }
}

/* Publish the off status the insert itself does not send (see mirror_off),
 * so the rocker on that light's group updates its LED. Also sent for the
 * first state after boot, in case the LED was already stale. */
static void mirror_off_status(int i, bool was_known, bool was_on)
{
    if (!LIGHTS[i].mirror_off || states[i].on || (was_known && !was_on)) {
        return;
    }
    uint8_t onoff[1] = { 0 };
    uint8_t lightness[2] = { 0, 0 };
    jung_send(LIGHTS[i].group, ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS, onoff, sizeof(onoff));
    if (LIGHTS[i].dimmable) {
        jung_send(LIGHTS[i].group, ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS, lightness, sizeof(lightness));
    }
    ESP_LOGI(TAG, "%s: published off status to 0x%04x for the rocker LED", LIGHTS[i].name, LIGHTS[i].group);
}

static void handle_onoff_status(uint16_t src, const esp_ble_mesh_gen_onoff_status_cb_t *st)
{
    int i = light_by_addr(src);
    if (i < 0) {
        return;
    }
    bool was_known = states[i].known, was_on = states[i].on;
    /* JUNG fills the optional target with garbage (e.g. present 1, target 0
     * while the light stays on), so trust present. The target only matters
     * while fading up from off. */
    bool fading_on = st->op_en && st->remain_time && st->target_onoff;
    states[i].known = true;
    states[i].on = st->present_onoff || fading_on;
    if (states[i].brightness == 0) {
        states[i].brightness = 255;
    }
    ESP_LOGI(TAG, "%s is %s", LIGHTS[i].name, states[i].on ? "ON" : "OFF");
    publish_state(i);
    mirror_off_status(i, was_known, was_on);
}

static void handle_lightness_status(uint16_t src, const esp_ble_mesh_light_lightness_status_cb_t *st)
{
    int i = light_by_addr(src);
    if (i < 0) {
        return;
    }
    /* Same as OnOff: target is unreliable (stays 0, or at the dimming limit
     * while the user holds the rocker), present is what the lamp does. */
    uint16_t lightness = st->present_lightness;
    if (lightness == 0 && st->op_en && st->remain_time) {
        lightness = st->target_lightness;
    }
    bool was_known = states[i].known, was_on = states[i].on;
    states[i].known = true;
    states[i].on = lightness > 0;
    if (lightness > 0) {
        states[i].brightness = lightness_to_brightness(lightness);
    } else if (states[i].brightness == 0) {
        states[i].brightness = 255;
    }
    ESP_LOGI(TAG, "%s lightness %u", LIGHTS[i].name, lightness);
    publish_state(i);
    mirror_off_status(i, was_known, was_on);
}

static void generic_client_cb(esp_ble_mesh_generic_client_cb_event_t event,
                              esp_ble_mesh_generic_client_cb_param_t *param)
{
    uint32_t op = param->params->opcode;
    uint16_t src = param->params->ctx.addr;

    switch (event) {
    case ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT:
    case ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT:
    case ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT:
        if (handle_property_status(param)) {
            break;
        }
        if (param->error_code) {
            ESP_LOGW(TAG, "generic client error %d from 0x%04x", param->error_code, src);
            break;
        }
        if (op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET) {
            set_answered(src);
        }
        if (op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET || op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET ||
            op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS) {
            handle_onoff_status(src, &param->status_cb.onoff_status);
        }
        break;
    case ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT:
        ESP_LOGW(TAG, "no answer from 0x%04x (op 0x%04" PRIx32 ")", src, op);
        if (op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET) {
            set_timed_out(src);
        }
        break;
    default:
        break;
    }
}

static void light_client_cb(esp_ble_mesh_light_client_cb_event_t event,
                            esp_ble_mesh_light_client_cb_param_t *param)
{
    uint32_t op = param->params->opcode;
    uint16_t src = param->params->ctx.addr;

    switch (event) {
    case ESP_BLE_MESH_LIGHT_CLIENT_GET_STATE_EVT:
    case ESP_BLE_MESH_LIGHT_CLIENT_SET_STATE_EVT:
    case ESP_BLE_MESH_LIGHT_CLIENT_PUBLISH_EVT:
        if (param->error_code) {
            ESP_LOGW(TAG, "light client error %d from 0x%04x", param->error_code, src);
            break;
        }
        if (op == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET) {
            set_answered(src);
        }
        if (op == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_GET || op == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET ||
            op == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_STATUS) {
            handle_lightness_status(src, &param->status_cb.lightness_status);
        }
        break;
    case ESP_BLE_MESH_LIGHT_CLIENT_TIMEOUT_EVT:
        ESP_LOGW(TAG, "no answer from 0x%04x (op 0x%04" PRIx32 ")", src, op);
        if (op == ESP_BLE_MESH_MODEL_OP_LIGHT_LIGHTNESS_SET) {
            set_timed_out(src);
        }
        break;
    default:
        break;
    }
}

/* ---------- BLE Mesh bring-up ---------- */

static esp_err_t bluetooth_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    return esp_bluedroid_enable();
}

static bool wait_setup_step(const char *what)
{
    if (xSemaphoreTake(setup_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "%s: timed out", what);
        return false;
    }
    if (setup_err) {
        ESP_LOGW(TAG, "%s: err %d", what, setup_err);
    }
    return true;
}

/* First boot only: join the JUNG network with the exported keys, store
 * everything in NVS, then reboot so the stack starts as a normal node. */
static void join_jung_network(void)
{
    uint8_t net_key[16], app_key[16], dev_key[16];

    if (!hex_to_key(MESH_NET_KEY_HEX, net_key) || !hex_to_key(MESH_APP_KEY_HEX, app_key)) {
        ESP_LOGE(TAG, "invalid NetKey/AppKey in secrets.h");
        abort();
    }
    esp_fill_random(dev_key, sizeof(dev_key));

    /* Puts the stack in node role (the API calls below check it) and starts scanning */
    esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV);
    if (!wait_setup_step("enable node role") || setup_err) {
        abort();
    }

    ESP_LOGI(TAG, "joining JUNG mesh as 0x%04x", MESH_BRIDGE_ADDR);
    int err = bt_mesh_provision(net_key, MESH_NET_IDX, 0, MESH_IV_INDEX, MESH_BRIDGE_ADDR, dev_key);
    if (err) {
        ESP_LOGE(TAG, "bt_mesh_provision failed: %d", err);
        abort();
    }

    esp_ble_mesh_node_add_local_app_key(app_key, MESH_NET_IDX, MESH_APP_IDX);
    wait_setup_step("add AppKey");

    const uint16_t client_models[] = {
        ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI,
        ESP_BLE_MESH_MODEL_ID_LIGHT_LIGHTNESS_CLI,
    };
    for (int m = 0; m < ARRAY_SIZE(client_models); m++) {
        esp_ble_mesh_node_bind_app_key_to_local_model(MESH_BRIDGE_ADDR, ESP_BLE_MESH_CID_NVAL,
                                                      client_models[m], MESH_APP_IDX);
        wait_setup_step("bind AppKey");
        /* Switches publish their status to these groups when pressed */
        for (int i = 0; i < LIGHT_COUNT; i++) {
            esp_ble_mesh_model_subscribe_group_addr(MESH_BRIDGE_ADDR, ESP_BLE_MESH_CID_NVAL,
                                                    client_models[m], LIGHTS[i].group);
            wait_setup_step("subscribe group");
        }
    }

    ESP_LOGI(TAG, "joined; rebooting to start as provisioned node");
    vTaskDelay(pdMS_TO_TICKS(3000)); /* let the settings layer flush to NVS */
    esp_restart();
}

/* Runs on every boot so models added in later firmware versions get set up
 * on an already-joined node. Repeats just report "already done". */
static void configure_console_models(void)
{
    uint16_t addr = esp_ble_mesh_get_primary_element_address();

    esp_ble_mesh_node_bind_app_key_to_local_model(addr, ESP_BLE_MESH_CID_NVAL,
                                                  ESP_BLE_MESH_MODEL_ID_GEN_PROP_CLI, MESH_APP_IDX);
    wait_setup_step("bind property client");
    esp_ble_mesh_node_bind_app_key_to_local_model(addr, JUNG_CID, CONSOLE_VND_MODEL, MESH_APP_IDX);
    wait_setup_step("bind vendor model");
    for (uint16_t g = JUNG_GROUP_FIRST; g <= JUNG_GROUP_LAST; g++) {
        esp_ble_mesh_model_subscribe_group_addr(addr, JUNG_CID, CONSOLE_VND_MODEL, g);
        wait_setup_step("subscribe vendor model");
    }
}

static void mesh_init(void)
{
    setup_done = xSemaphoreCreateBinary();

    ESP_ERROR_CHECK(bluetooth_init());

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    memcpy(dev_uuid, "JUNGBRIDGE", 10);
    memcpy(dev_uuid + 10, mac, 6);

    esp_ble_mesh_register_prov_callback(prov_cb);
    esp_ble_mesh_register_generic_client_callback(generic_client_cb);
    esp_ble_mesh_register_light_client_callback(light_client_cb);
    esp_ble_mesh_register_custom_model_callback(custom_model_cb);
    refresh_timers_init();
    sniffer_init(console_publish, on_foreign_light_cmd);
    ESP_ERROR_CHECK(esp_ble_mesh_init(&provision, &composition));

    if (!esp_ble_mesh_node_is_provisioned()) {
        join_jung_network(); /* does not return */
    }
    configure_console_models();
    ESP_LOGI(TAG, "mesh node up, address 0x%04x", esp_ble_mesh_get_primary_element_address());
}

/* ---------- MQTT ---------- */

static void handle_command(int i, const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "bad command payload for %s", LIGHTS[i].id);
        return;
    }
    const cJSON *state = cJSON_GetObjectItem(root, "state");
    const cJSON *bri = cJSON_GetObjectItem(root, "brightness");
    bool on = !cJSON_IsString(state) || strcmp(state->valuestring, "OFF") != 0;

    if (!on) {
        mesh_set(i, false, 0);
    } else if (LIGHTS[i].dimmable && cJSON_IsNumber(bri)) {
        int b = bri->valueint;
        b = b < 1 ? 1 : (b > 255 ? 255 : b);
        mesh_set(i, true, brightness_to_lightness(b));
    } else {
        mesh_set(i, false, 1);
    }
    cJSON_Delete(root);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        mqtt_connected = true;
        ota_confirm();
        esp_mqtt_client_publish(mqtt, TOPIC_AVAILABILITY, "online", 0, 1, 1);
        esp_mqtt_client_subscribe(mqtt, TOPIC_PREFIX "/+/set", 1);
        esp_mqtt_client_subscribe(mqtt, TOPIC_PREFIX "/+/+/set", 1);
        esp_mqtt_client_subscribe(mqtt, TOPIC_HA_STATUS, 1);
        esp_mqtt_client_subscribe(mqtt, TOPIC_DEBUG_TX, 0);
        esp_mqtt_client_subscribe(mqtt, TOPIC_DEBUG_SNIFF, 0);
        esp_mqtt_client_subscribe(mqtt, TOPIC_OTA, 1);
        {
            const esp_app_desc_t *app = esp_app_get_description();
            char ver[96];
            snprintf(ver, sizeof(ver), "%s (%s %s)", app->version, app->date, app->time);
            esp_mqtt_client_publish(mqtt, TOPIC_VERSION, ver, 0, 1, 1);
        }
        publish_discovery();
        for (int i = 0; i < LIGHT_COUNT; i++) {
            publish_state(i);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        mqtt_connected = false;
        break;
    case MQTT_EVENT_DATA: {
        /* HA restarted: re-announce */
        if (ev->topic_len == strlen(TOPIC_HA_STATUS) &&
            strncmp(ev->topic, TOPIC_HA_STATUS, ev->topic_len) == 0) {
            if (ev->data_len == 6 && strncmp(ev->data, "online", 6) == 0) {
                publish_discovery();
                for (int i = 0; i < LIGHT_COUNT; i++) {
                    publish_state(i);
                }
            }
            break;
        }
        if (ev->topic_len == strlen(TOPIC_DEBUG_TX) &&
            strncmp(ev->topic, TOPIC_DEBUG_TX, ev->topic_len) == 0) {
            console_tx(ev->data, ev->data_len);
            break;
        }
        if (ev->topic_len == strlen(TOPIC_DEBUG_SNIFF) &&
            strncmp(ev->topic, TOPIC_DEBUG_SNIFF, ev->topic_len) == 0) {
            sniffer_set_enabled(ev->data_len == 2 && strncmp(ev->data, "on", 2) == 0);
            break;
        }
        if (ev->topic_len == strlen(TOPIC_OTA) && strncmp(ev->topic, TOPIC_OTA, ev->topic_len) == 0) {
            /* a retained URL would re-flash on every reconnect */
            if (!ev->retain && ev->data_len > 0 && ev->data_len < sizeof(ota_url)) {
                memcpy(ota_url, ev->data, ev->data_len);
                ota_url[ev->data_len] = '\0';
                xTaskCreate(ota_task, "ota", 8192, NULL, 5, NULL);
            }
            break;
        }
        /* jungmesh/<id>/set or jungmesh/<id>/led_on|led_off/set */
        const char *prefix = TOPIC_PREFIX "/";
        const size_t plen = strlen(prefix);
        if (ev->topic_len > plen + 4 && strncmp(ev->topic, prefix, plen) == 0 &&
            strncmp(ev->topic + ev->topic_len - 4, "/set", 4) == 0) {
            const char *id = ev->topic + plen;
            size_t rest = ev->topic_len - plen - 4;
            const char *slash = memchr(id, '/', rest);
            int i = light_by_id(id, slash ? (size_t)(slash - id) : rest);
            if (i < 0) {
                break;
            }
            if (!slash) {
                handle_command(i, ev->data, ev->data_len);
                break;
            }
            size_t kind_len = rest - (slash - id) - 1;
            for (int k = 0; k < 2; k++) {
                if (strlen(LED_KIND[k]) == kind_len && strncmp(slash + 1, LED_KIND[k], kind_len) == 0) {
                    handle_led_command(i, k, ev->data, ev->data_len);
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

static void mqtt_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_URI,
        .credentials.username = MQTT_USER,
        .credentials.client_id = "jungmesh-bridge",
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.last_will = {
            .topic = TOPIC_AVAILABILITY,
            .msg = "offline",
            .qos = 1,
            .retain = 1,
        },
    };
    mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt);
}

/* ---------- Wi-Fi ---------- */

static EventGroupHandle_t wifi_events;
#define WIFI_GOT_IP BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_GOT_IP);
    }
}

static void wifi_init(void)
{
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---------- main ---------- */

/* Status publications from the switches are the primary update path; this
 * poll only fills the initial state and catches anything that was missed. */
static void poll_task(void *arg)
{
    for (unsigned round = 0;; round++) {
        for (int i = 0; i < LIGHT_COUNT; i++) {
            mesh_get_state(i);
            vTaskDelay(pdMS_TO_TICKS(POLL_SPACING_MS));
        }
        /* LED colours only change from the JUNG app or HA, so poll rarely */
        if (round % LED_POLL_EVERY == 0) {
            for (int i = 0; i < LIGHT_COUNT; i++) {
                for (int k = 0; k < 2; k++) {
                    led_get(i, k);
                    vTaskDelay(pdMS_TO_TICKS(POLL_SPACING_MS));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ota_check_pending();

    mesh_init();

    wifi_init();
    xEventGroupWaitBits(wifi_events, WIFI_GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
    mqtt_start();

    xTaskCreate(poll_task, "mesh_poll", 4096, NULL, 5, NULL);
}
