#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Unicast address the ESP32 uses inside the JUNG mesh. Must not collide with
 * any existing node; the JUNG app hands out low addresses, so stay far away. */
#define MESH_BRIDGE_ADDR    0x7F00
#define MESH_NET_IDX        0x0000
#define MESH_APP_IDX        0x0000
#define MESH_IV_INDEX       0x00000000
#define MESH_TTL            7

typedef struct {
    const char *id;       /* used in MQTT topics / unique_id */
    const char *name;
    const char *area;     /* suggested_area in Home Assistant */
    uint16_t addr;        /* element unicast address of the switch channel */
    uint16_t group;       /* group the switch publishes its status to */
    bool dimmable;        /* has Light Lightness Server (0x1300) */
    /* Status LED colours live in JUNG vendor parameters on the node's primary
     * element. The 2-gang insert has one LED per rocker (left 0xA001/0xA002,
     * right 0xA004/0xA005); 1-gang inserts mirror both pairs, so both are
     * written, as the JUNG app does. 0 = unused. */
    uint16_t led_node;
    uint16_t led_on[2];   /* LED colour while the load is on */
    uint16_t led_off[2];  /* LED colour while the load is off */
    /* The living-room insert publishes its state when switched on but not
     * when switched off, so its rocker LED keeps the "on" colour. For such
     * lights the bridge publishes the off status to the group itself. */
    bool mirror_off;
} jung_light_t;

static const jung_light_t LIGHTS[] = {
    { "kitchen_window",    "Kitchen window",    "Kitchen",     0x0007, 0xC005, false,
      0x0007, { 0xA001, 0 },      { 0xA002, 0 },      false },
    { "kitchen_island",    "Kitchen island",    "Kitchen",     0x0008, 0xC006, false,
      0x0007, { 0xA004, 0 },      { 0xA005, 0 },      false },
    { "kitchen_spots",     "Kitchen spots",     "Kitchen",     0x000F, 0xC00A, true,
      0x000F, { 0xA001, 0xA004 }, { 0xA002, 0xA005 }, false },
    { "dining_lights",     "Dining lights",     "Dining room", 0x0015, 0xC00D, true,
      0x0015, { 0xA001, 0xA004 }, { 0xA002, 0xA005 }, false },
    { "living_room_spots", "Living room spots", "Living room", 0x001B, 0xC010, true,
      0x001B, { 0xA001, 0xA004 }, { 0xA002, 0xA005 }, true },
};

#define LIGHT_COUNT (sizeof(LIGHTS) / sizeof(LIGHTS[0]))
