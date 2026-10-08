/*
 * ids.h — typed 32-bit IR object IDs (IR-5); 0 means none.
 */
#ifndef ODIN3_IR_IDS_H
#define ODIN3_IR_IDS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t v;
} odin3_node_id; /* module-local */
typedef struct {
    uint32_t v;
} odin3_pin_id; /* module-local */
typedef struct {
    uint32_t v;
} odin3_net_id; /* module-local */
typedef struct {
    uint32_t v;
} odin3_wire_id; /* module-local */
typedef struct {
    uint32_t v;
} odin3_module_id; /* design-global */
typedef struct {
    uint32_t v;
} odin3_celltype_id; /* design-global */
typedef struct {
    uint32_t v;
} odin3_prov_id; /* design-global */
typedef struct {
    uint32_t v;
} odin3_passrun_id; /* design-global */

static inline bool odin3_node_valid(odin3_node_id id) {
    return id.v != 0;
}
static inline bool odin3_pin_valid(odin3_pin_id id) {
    return id.v != 0;
}
static inline bool odin3_net_valid(odin3_net_id id) {
    return id.v != 0;
}
static inline bool odin3_wire_valid(odin3_wire_id id) {
    return id.v != 0;
}
static inline bool odin3_module_valid(odin3_module_id id) {
    return id.v != 0;
}
static inline bool odin3_celltype_valid(odin3_celltype_id id) {
    return id.v != 0;
}
static inline bool odin3_prov_valid(odin3_prov_id id) {
    return id.v != 0;
}
static inline bool odin3_passrun_valid(odin3_passrun_id id) {
    return id.v != 0;
}

#endif
