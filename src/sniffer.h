#pragma once

#include <stdbool.h>
#include "cJSON.h"

/* Passive mesh sniffer: decrypts access messages between other nodes (e.g.
 * JUNG app -> switch). While enabled it hands each one to `emit`, which
 * takes ownership. Independently of that, `on_light_cmd` is always called
 * with the destination of every OnOff/Level/Lightness command another node
 * sends, so the bridge can re-read lights that do not report changes. */
void sniffer_init(void (*emit)(cJSON *msg), void (*on_light_cmd)(uint16_t dst));
void sniffer_set_enabled(bool enabled);
