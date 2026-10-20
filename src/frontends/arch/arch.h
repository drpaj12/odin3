/*
 * arch.h — the VPR architecture record (Phase 4.1, docs/specs/2026-10-20-4.1-arch-import-design.md)
 * and the importer behind `read_arch`.
 *
 * The record holds what Phase 4 items 4.2 to 4.6 need from a VPR XML architecture: the `<models>`
 * (ports, clock flags, combinational sinks, the VPR port sizes), every pb_type leaf with its path
 * from the top-level block, the LUT size K, the interconnect pack patterns and the directlist, and
 * the `.o3lib` text generated for the models. All strings and arrays live in the record's arena;
 * the design owns the record (odin3_design_get_arch). Nothing here touches the IR.
 */
#ifndef ODIN3_FRONTENDS_ARCH_ARCH_H
#define ODIN3_FRONTENDS_ARCH_ARCH_H

#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* A model port (spec M-2, M-3). Widths are VPR's: the max and min num_pins over the leaves that
 * use the model; 0 when no leaf uses the port. */
typedef struct odin3_arch_mport {
    const char *name;
    bool is_out;
    bool is_clock;
    bool non_clock_global;
    const char *clock;        /* the `clock` attribute, NULL when absent */
    const char *const *sinks; /* combinational_sink_ports */
    uint32_t n_sinks;
    uint32_t max_width, min_width;
    uint32_t line;
} odin3_arch_mport;

typedef struct odin3_arch_model {
    const char *name;
    odin3_arch_mport *ports; /* XML order, inputs then outputs as written */
    uint32_t n_ports;
    uint32_t line;
    bool registered;  /* became a library cell (spec M-5) */
    bool is_template; /* registered as one of the four VTR primitives (spec L-2), not a black box */
} odin3_arch_model;

typedef enum odin3_arch_leaf_kind {
    ODIN3_ARCH_LEAF_INPUT,
    ODIN3_ARCH_LEAF_OUTPUT,
    ODIN3_ARCH_LEAF_NAMES,
    ODIN3_ARCH_LEAF_LATCH,
    ODIN3_ARCH_LEAF_SUBCKT
} odin3_arch_leaf_kind;

typedef enum odin3_arch_pin_dir {
    ODIN3_ARCH_PIN_IN,
    ODIN3_ARCH_PIN_OUT,
    ODIN3_ARCH_PIN_CLOCK
} odin3_arch_pin_dir;

/* A pb_type port: `input`, `output` or `clock` element. */
typedef struct odin3_arch_pin {
    const char *name;
    odin3_arch_pin_dir dir;
    uint32_t num_pins;
    const char *port_class; /* NULL when absent */
} odin3_arch_pin;

/* One step of a leaf's path: the pb_type, the mode of its parent that holds it (NULL for a
 * top-level block or a parent without modes) and its num_pb. */
typedef struct odin3_arch_level {
    const char *pb;
    const char *in_mode;
    uint32_t num_pb;
} odin3_arch_level;

typedef struct odin3_arch_leaf {
    odin3_arch_leaf_kind kind;
    const char *model;              /* the `.subckt` model name, NULL otherwise */
    const char *cls;                /* the `class` attribute, NULL when absent */
    const odin3_arch_level *levels; /* top-level block first, the leaf last */
    uint32_t n_levels;
    const odin3_arch_pin *pins;
    uint32_t n_pins;
    uint32_t num_pb;
    uint32_t line;
} odin3_arch_leaf;

typedef struct odin3_arch_pack {
    const char *name, *in_port, *out_port; /* the last two may be NULL */
} odin3_arch_pack;

/* An `<interconnect>` child (direct, mux or complete) of the pb_type `pb` (mode `in_mode`). */
typedef struct odin3_arch_ic {
    const char *kind; /* "direct", "mux" or "complete" */
    const char *name, *input, *output;
    const char *pb, *in_mode;
    const odin3_arch_pack *packs;
    uint32_t n_packs;
    uint32_t line;
} odin3_arch_ic;

/* A `<directlist>/<direct>` entry (inter-block connection). */
typedef struct odin3_arch_direct {
    const char *name, *from_pin, *to_pin;
    int32_t x_offset, y_offset, z_offset;
    uint32_t line;
} odin3_arch_direct;

typedef struct odin3_arch {
    odin3_arena *arena;
    const char *file;    /* the source name */
    const char *library; /* the generated library's name */
    odin3_arch_model *models;
    uint32_t n_models;
    uint32_t n_registered;
    odin3_vec leaves;  /* odin3_arch_leaf */
    odin3_vec ics;     /* odin3_arch_ic */
    odin3_vec directs; /* odin3_arch_direct */
    uint32_t lut_k, lut_k_min;
    bool has_adder;
    odin3_strbuf text; /* the generated .o3lib text */
} odin3_arch;

/* An empty record; NULL on out of memory. */
odin3_arch *odin3_arch_create(void);
void odin3_arch_destroy(odin3_arch *arch); /* NULL is a no-op */

/* A source in memory: the name used in messages and the XML bytes. */
typedef struct odin3_arch_src {
    const char *name;
    odin3_bytes text;
} odin3_arch_src;

/*
 * Parses the XML in src into arch (models and pb_type facts, spec M-1..M-5, P-1..P-7; no library
 * text, no registration). PARSE (logged as file:line: message) for any error; NO_MEMORY. On
 * failure arch holds a partial record; destroy it.
 */
odin3_status odin3_arch_parse(odin3_arch *arch, const odin3_arch_src *src);

/* Builds arch->text, the library for the registered models (spec L-1..L-6); counts the models
 * that become cells in arch->n_registered. NO_MEMORY. */
odin3_status odin3_arch_make_library(odin3_arch *arch);

/*
 * The whole import: parse, make the library, register it with the design's tech-library reader and
 * attach the record to the design (odin3_design_get_arch). A failure leaves the design without a
 * record and without new types (INVALID_ARG, logged, when the design already has a record).
 */
odin3_status odin3_arch_read_text(odin3_design *design, const odin3_arch_src *src);

/* odin3_arch_read_text on the file at path; IO (logged) when it cannot be read. */
odin3_status odin3_arch_read(odin3_design *design, const char *path);

/* --- queries (all const; model and leaf indices are in document order) ------------------------ */

uint32_t odin3_arch_lut_k(const odin3_arch *arch);     /* 0 when the arch has no `.names` leaf */
uint32_t odin3_arch_lut_k_min(const odin3_arch *arch); /* 0 likewise */
bool odin3_arch_has_adder(const odin3_arch *arch);
uint32_t odin3_arch_model_count(const odin3_arch *arch);
const odin3_arch_model *odin3_arch_model_at(const odin3_arch *arch, uint32_t index);
const odin3_arch_model *odin3_arch_model_find(const odin3_arch *arch, const char *name);
uint32_t odin3_arch_leaf_count(const odin3_arch *arch);
const odin3_arch_leaf *odin3_arch_leaf_at(const odin3_arch *arch, uint32_t index);
/* The index of the next `.subckt` leaf of `model` at or after `from`; false when there is none. */
bool odin3_arch_next_leaf_of(const odin3_arch *arch, const char *model, uint32_t from,
                             uint32_t *index);
uint32_t odin3_arch_ic_count(const odin3_arch *arch);
const odin3_arch_ic *odin3_arch_ic_at(const odin3_arch *arch, uint32_t index);
uint32_t odin3_arch_direct_count(const odin3_arch *arch);
const odin3_arch_direct *odin3_arch_direct_at(const odin3_arch *arch, uint32_t index);
const char *odin3_arch_library_text(const odin3_arch *arch);

/* --- accessors of partial_map (4.2 spec section 9) ---------------------------------------------
 */

/* The VPR width (max num_pins over the leaves) of port `port` of `model`; 0 when model is NULL, the
 * port is absent, or no leaf uses it. */
uint32_t odin3_arch_model_port_width(const odin3_arch_model *model, const char *port);

/* True when the arch has the model and it took the library template of that name (spec L-2): a
 * model of the same name with other ports is a black box and gives false. */
bool odin3_arch_model_is_template(const odin3_arch *arch, const char *model);

/* The most address pins of any leaf of `single_port_ram` or `dual_port_ram`: the largest num_pins
 * of an input whose name contains "addr" (VTR's determine_memory_addr_width). 0 without such a
 * leaf. */
uint32_t odin3_arch_max_addr_pins(const odin3_arch *arch);

/* One mode of a RAM model: the address pins and data pins of one leaf. */
typedef struct odin3_arch_ram_mode {
    uint32_t addr, data;
} odin3_arch_ram_mode;

/* The leaves of `model` in document order as (address, data) pins: the largest num_pins of an
 * input named "*addr*" and of an input named "*data*". Fills at most `cap` entries of `out` (NULL
 * allowed with cap 0) and returns the number of leaves. */
uint32_t odin3_arch_modes_of(const odin3_arch *arch, const char *model, odin3_arch_ram_mode *out,
                             uint32_t cap);

#endif
