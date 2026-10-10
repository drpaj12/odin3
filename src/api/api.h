/*
 * api.h — helpers shared by the public ABI wrappers in src/api (private: never in odin3.h).
 */
#ifndef ODIN3_API_API_H
#define ODIN3_API_API_H

#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stdint.h>

/* The module-local ID stores an odin3_ref can name. */
typedef enum odin3_api_store {
    ODIN3_API_NODE,
    ODIN3_API_PIN,
    ODIN3_API_NET,
    ODIN3_API_WIRE
} odin3_api_store;

/* The module with ID module, NULL for a NULL design or an ID that is not a module of it. */
const odin3_module *odin3_api_module(const odin3_design *design, uint32_t module);

/* One past the last ID of store in module. */
uint32_t odin3_api_end(const odin3_module *module, odin3_api_store store);

/* Number of live objects of store (NODE, NET or WIRE) in module (O(store)). */
uint32_t odin3_api_live_count(const odin3_module *module, odin3_api_store store);

/* The module of ref when ref.id is an ID of store in it (1 .. end - 1), else NULL. */
const odin3_module *odin3_api_ref(const odin3_design *design, odin3_ref ref, odin3_api_store store);

/*
 * The module of obj (a node, net or wire of that module, or the module itself) with *ref set to
 * obj as the IR names it; NULL for a NULL design, an unknown module, a kind out of range or an ID
 * that is 0 or past its store's end (*ref is then unspecified).
 */
const odin3_module *odin3_api_obj(const odin3_design *design, odin3_obj obj, odin3_objref *ref);

/* Logs "<fn>: invalid argument …" at ODIN3_LOG_ERROR and returns ODIN3_ERR_INVALID_ARG. */
odin3_status odin3_api_invalid(const char *fn);

/* The design's string for strtab ID str ("" for 0); stable until the design is destroyed. */
const char *odin3_api_str(const odin3_design *design, uint32_t str);

/*
 * Interns text in the design's strtab (a cache for computed text: not an IR change, hence the
 * const design) and sets *out to the stable copy (strtab strings never move, so earlier strings
 * stay valid). ODIN3_ERR_NO_MEMORY (*out unchanged) on out of memory.
 */
odin3_status odin3_api_text(const odin3_design *design, odin3_bytes text, const char **out);

#endif
