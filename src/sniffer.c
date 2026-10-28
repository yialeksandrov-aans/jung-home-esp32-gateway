/*
 * The mesh stack only passes messages addressed to us up to the access
 * layer. To see traffic between other nodes, the linker wraps
 * bt_mesh_trans_recv() (see CMakeLists.txt): every network PDU the stack
 * has already NetKey-decrypted passes through here first, and we decrypt the
 * upper transport layer ourselves with the AppKey or the node's DevKey.
 *
 * Only unsegmented access messages are decoded; that covers short
 * configuration commands. Segmented ones are reported raw.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mbedtls/ccm.h"
#include "esp_ble_mesh_defs.h"

#include "lights.h"
#include "secrets.h"
#include "sniffer.h"

#define TAG "sniffer"

#define NET_HDR_LEN     9
#define TRANS_MIC_LEN   4

typedef struct {
    uint16_t first;
    uint8_t count;
    const char *hex;
} dev_key_entry_t;

static const dev_key_entry_t DEV_KEYS[] = MESH_DEV_KEYS;

static uint8_t app_key[16];
static uint8_t dev_keys[sizeof(DEV_KEYS) / sizeof(DEV_KEYS[0])][16];
static void (*emit_cb)(cJSON *msg);
static void (*light_cmd_cb)(uint16_t dst);
static volatile bool enabled;

int __real_bt_mesh_trans_recv(struct net_buf_simple *buf, void *rx);

static void parse_key(const char *hex, uint8_t out[16])
{
    for (int i = 0; i < 16; i++) {
        unsigned int b = 0;
        sscanf(hex + 2 * i, "%2x", &b);
        out[i] = b;
    }
}

static const uint8_t *dev_key_for(uint16_t addr)
{
    for (int i = 0; i < sizeof(DEV_KEYS) / sizeof(DEV_KEYS[0]); i++) {
        if (addr >= DEV_KEYS[i].first && addr < DEV_KEYS[i].first + DEV_KEYS[i].count) {
            return dev_keys[i];
        }
    }
    return NULL;
}

static bool ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13],
                        const uint8_t *in, size_t len, uint8_t *out)
{
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    bool ok = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
              mbedtls_ccm_auth_decrypt(&ctx, len - TRANS_MIC_LEN, nonce, 13, NULL, 0,
                                       in, out, in + len - TRANS_MIC_LEN, TRANS_MIC_LEN) == 0;
    mbedtls_ccm_free(&ctx);
    return ok;
}

static void hex_add(cJSON *root, const char *name, const uint8_t *data, size_t len)
{
    char hex[2 * 32 + 1] = "";
    for (size_t i = 0; i < len && i < 32; i++) {
        snprintf(hex + 2 * i, 3, "%02X", data[i]);
    }
    cJSON_AddStringToObject(root, name, hex);
}

static cJSON *sniff_header(uint16_t src, uint16_t dst, uint32_t seq)
{
    char buf[8];
    cJSON *root = cJSON_CreateObject();
    snprintf(buf, sizeof(buf), "%04X", src);
    cJSON_AddStringToObject(root, "src", buf);
    snprintf(buf, sizeof(buf), "%04X", dst);
    cJSON_AddStringToObject(root, "dst", buf);
    cJSON_AddNumberToObject(root, "seq", seq);
    cJSON_AddTrueToObject(root, "sniff");
    return root;
}

/* Generic OnOff / Level / Light Lightness Set, Delta, Move (acked or not) */
static bool is_light_command(uint32_t op)
{
    return (op >= 0x8202 && op <= 0x8203) ||   /* OnOff Set */
           (op >= 0x8206 && op <= 0x820C) ||   /* Level Set, Delta, Move */
           (op >= 0x824C && op <= 0x824D);     /* Lightness Set */
}

static void sniff(const uint8_t *pdu, size_t len)
{
    if (len < NET_HDR_LEN + 1 + TRANS_MIC_LEN) {
        return;
    }
    bool ctl = pdu[1] & 0x80;
    uint8_t ivi = pdu[0] >> 7;
    uint32_t seq = (pdu[2] << 16) | (pdu[3] << 8) | pdu[4];
    uint16_t src = (pdu[5] << 8) | pdu[6];
    uint16_t dst = (pdu[7] << 8) | pdu[8];
    const uint8_t *lower = pdu + NET_HDR_LEN;
    size_t lower_len = len - NET_HDR_LEN;

    /* Answers to us are shown too (raw status bytes help debugging), but our
     * own commands must not trigger a refresh of what we just set. */
    if (ctl || src == MESH_BRIDGE_ADDR) {
        return;
    }

    bool seg = lower[0] & 0x80;
    bool akf = lower[0] & 0x40;
    if (seg) {
        if (enabled) {
            cJSON *root = sniff_header(src, dst, seq);
            cJSON_AddTrueToObject(root, "seg");
            hex_add(root, "raw", lower, lower_len);
            emit_cb(root);
        }
        return;
    }

    uint32_t iv = MESH_IV_INDEX;
    if ((iv & 1) != ivi) {
        iv--;
    }
    uint8_t nonce[13] = {
        akf ? 0x01 : 0x02, 0x00,
        seq >> 16, seq >> 8, seq,
        src >> 8, src, dst >> 8, dst,
        iv >> 24, iv >> 16, iv >> 8, iv,
    };
    const uint8_t *upper = lower + 1;
    size_t upper_len = lower_len - 1;
    uint8_t plain[16];
    const char *key = NULL;

    if (upper_len <= sizeof(plain) + TRANS_MIC_LEN) {
        if (akf) {
            key = ccm_decrypt(app_key, nonce, upper, upper_len, plain) ? "app" : NULL;
        } else if (enabled) {
            /* requests use the target's DevKey, responses the sender's */
            const uint8_t *keys[] = { dev_key_for(dst), dev_key_for(src) };
            const char *names[] = { "dev(dst)", "dev(src)" };
            for (int k = 0; k < 2 && !key; k++) {
                if (keys[k] && ccm_decrypt(keys[k], nonce, upper, upper_len, plain)) {
                    key = names[k];
                }
            }
        }
    }

    size_t plain_len = upper_len - TRANS_MIC_LEN;
    if (!key || plain_len == 0) {
        if (enabled) {
            cJSON *root = sniff_header(src, dst, seq);
            cJSON_AddFalseToObject(root, "decrypted");
            hex_add(root, "raw", lower, lower_len);
            emit_cb(root);
        }
        return;
    }

    /* Same opcode notation as ESP-BLE-MESH: vendor = (b0 << 16) | CID */
    size_t op_len = (plain[0] & 0x80) ? ((plain[0] & 0x40) ? 3 : 2) : 1;
    uint32_t op = plain[0];
    if (op_len == 2) {
        op = (plain[0] << 8) | plain[1];
    } else if (op_len == 3) {
        op = (plain[0] << 16) | plain[1] | (plain[2] << 8);
    }

    if (light_cmd_cb && is_light_command(op)) {
        light_cmd_cb(dst);
    }

    if (enabled) {
        char buf[16];
        cJSON *root = sniff_header(src, dst, seq);
        cJSON_AddStringToObject(root, "key", key);
        snprintf(buf, sizeof(buf), "%" PRIX32, op);
        cJSON_AddStringToObject(root, "op", buf);
        if (plain_len > op_len) {
            hex_add(root, "data", plain + op_len, plain_len - op_len);
        }
        emit_cb(root);
    }
}

int __wrap_bt_mesh_trans_recv(struct net_buf_simple *buf, void *rx)
{
    if (emit_cb) {
        sniff(buf->data, buf->len);
    }
    return __real_bt_mesh_trans_recv(buf, rx);
}

void sniffer_init(void (*emit)(cJSON *msg), void (*on_light_cmd)(uint16_t dst))
{
    parse_key(MESH_APP_KEY_HEX, app_key);
    for (int i = 0; i < sizeof(DEV_KEYS) / sizeof(DEV_KEYS[0]); i++) {
        parse_key(DEV_KEYS[i].hex, dev_keys[i]);
    }
    light_cmd_cb = on_light_cmd;
    emit_cb = emit;
}

void sniffer_set_enabled(bool on)
{
    enabled = on;
    ESP_LOGI(TAG, "sniffer %s", on ? "on" : "off");
}
