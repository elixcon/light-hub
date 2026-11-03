/* The lamp commands a light endpoint answers, decoded from a ZCL frame.
 *
 * A leaf module: no ESP-IDF and no Zigbee SDK, so it compiles with cc and every
 * frame can be driven without hardware. The numbers are the ZCL specification's. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_ZCL_CLUSTER_ON_OFF 0x0006u
#define NODE_ZCL_CLUSTER_LEVEL  0x0008u
#define NODE_ZCL_CLUSTER_COLOR  0x0300u

/* "Use the OnOffTransitionTime attribute", as MoveToLevel's own field spells it. */
#define NODE_ZCL_TT_DEFAULT 0xFFFFu

typedef enum {
    NODE_ZCL_NONE = 0,
    NODE_ZCL_ON,
    NODE_ZCL_OFF,
    NODE_ZCL_TOGGLE,
    NODE_ZCL_OFF_EFFECT,    /* OffWithEffect: fades out, keeping the level */
    NODE_ZCL_LEVEL,         /* MoveToLevel: changes nothing while the lamp is off */
    NODE_ZCL_LEVEL_ONOFF,   /* MoveToLevelWithOnOff */
    NODE_ZCL_STOP,
    NODE_ZCL_CT,            /* MoveToColorTemperature */
} node_zcl_kind_t;

typedef struct {
    uint8_t  kind;          /* node_zcl_kind_t */
    uint8_t  level;         /* 0-254 */
    uint16_t mireds;
    uint16_t transition_ds; /* NODE_ZCL_TT_DEFAULT when the frame leaves it to the attribute */
} node_zcl_cmd_t;

/* False for anything else, a frame too short to carry its fields included: it is
 * then left to the stack to answer. Move and Step are not here, since a lamp that
 * runs to a rail on a lost Stop is what the hub never asks for (CLAUDE.md, Move/Stop). */
bool node_zcl_decode(uint16_t cluster_id, uint8_t cmd_id, const uint8_t *payload, uint16_t len,
                     node_zcl_cmd_t *out);

/* What a light's attributes say, from what it is doing. A fade-out reads as the off it
 * is about to be, at the level it will come back to, as a lamp switched off with
 * OffWithEffect keeps its CurrentLevel. Told "on, at 13 %" straight after switching it
 * off, the hub would send the next toggle the wrong way, and take 13 % for its level. */
typedef struct {
    bool    on;
    bool    has_level;
    uint8_t level;
} node_zcl_report_t;

node_zcl_report_t node_zcl_report(int on, int level, int comes_back_to, bool fading_out);

#ifdef __cplusplus
}
#endif
