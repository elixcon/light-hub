/* The lamp commands a light endpoint answers, decoded from a ZCL frame.
 *
 * A leaf module: no ESP-IDF and no Zigbee SDK, so it compiles with cc and every
 * frame can be driven without hardware. The numbers are the ZCL specification's. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gradient.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_ZCL_CLUSTER_ON_OFF 0x0006u
#define NODE_ZCL_CLUSTER_LEVEL  0x0008u
#define NODE_ZCL_CLUSTER_COLOR  0x0300u
/* Philips' state cluster, which an LED strip's endpoint lists so the hub sends it
 * a gradient as it sends a Hue lamp one (gradient.h). Manufacturer-specific: the
 * caller checks the frame carries ZBC_HUE_MANUF. */
#define NODE_ZCL_CLUSTER_HUE    ZBC_HUE_CLUSTER

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
    NODE_ZCL_HS,            /* MoveToHueAndSaturation: an LED strip's colour */
    NODE_ZCL_XY,            /* MoveToColor, or Philips' command with one colour */
    NODE_ZCL_GRADIENT,      /* Philips' command with two colours or more */
} node_zcl_kind_t;

typedef struct {
    uint8_t  kind;          /* node_zcl_kind_t */
    uint8_t  level;         /* 0-254 */
    uint16_t mireds;
    uint16_t transition_ds; /* NODE_ZCL_TT_DEFAULT when the frame leaves it to the attribute */
    uint8_t  hue, sat;      /* 0-254 */
    uint16_t x, y;          /* CIE xy in 65536ths */
    zbc_gradient_t gradient;
} node_zcl_cmd_t;

/* False for anything else, a frame too short to carry its fields included: it is
 * then left to the stack to answer. Move and Step are not here, since a lamp that
 * runs to a rail on a lost Stop is what the hub never asks for (CLAUDE.md, Move/Stop). */
bool node_zcl_decode(uint16_t cluster_id, uint8_t cmd_id, const uint8_t *payload, uint16_t len,
                     node_zcl_cmd_t *out);

/* Whether a Read Attributes request's payload - a list of attribute ids - asks
 * for Philips' state attribute. */
bool node_zcl_reads_state(const uint8_t *payload, uint16_t len);

/* The answer to that read, as the whole frame the hub takes in at APS: the ZCL
 * header of a manufacturer-specific Read Attributes Response carrying `tsn`,
 * then the attribute, success, an octet string and the state. Returns its
 * length, 0 when `cap` is too small. */
#define NODE_ZCL_STATE_RSP_MAX (5 + 5 + ZBC_HUE_STATE_LEN_MAX)
size_t node_zcl_state_rsp(uint8_t tsn, const zbc_hue_state_t *st, uint8_t *out, size_t cap);

/* What an LED strip says it shows. `gradient` is its colours, NULL or empty
 * while it shows a colour temperature: two or more are reported as a gradient,
 * one as a plain colour. */
zbc_hue_state_t node_zcl_strip_state(bool on, uint8_t level, const zbc_gradient_t *gradient);

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
