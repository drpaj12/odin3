/*
 * lower_fixture.h — hand-building one-cell modules for the 3B unit tests (plan Task 1, point 5,
 * Task 2 step 4): a module holding one cell of a given type and parameters, every cell port a
 * module port of the same name, plus a before/after simulation comparison. Test code: it asserts
 * on every IR call instead of returning statuses.
 */
#ifndef ODIN3_TESTS_UNIT_LOWER_FIXTURE_H
#define ODIN3_TESTS_UNIT_LOWER_FIXTURE_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct lf {
    odin3_design *design;
    odin3_pass_ctx ctx; /* the pass run "lower", for odin3_lower_run */
} lf;

/* The parameters of a binary cell ($and, $add, $eq, ...) or a unary one ($not, $neg, $reduce_*). */
typedef struct lf_binary {
    bool a_signed, b_signed;
    uint32_t a_width, b_width, y_width;
} lf_binary;

/* A new design with one pass run named "lower". */
void lf_begin(lf *fix);

/* Destroys the design. */
void lf_end(lf *fix);

uint32_t lf_str(lf *fix, const char *text);

/* The cell parameters, in definition order (the unit tests' abbreviation for odin3_value_int). */
void lf_binary_params(const lf_binary *spec, odin3_value out[5]);
void lf_unary_params(const lf_binary *spec, odin3_value out[3]);

/* A SOURCE record at file:line, made in a pass run named "read_verilog". */
odin3_prov_id lf_source(lf *fix, const char *file, uint32_t line);

/* A new module, no ports. */
odin3_module *lf_module(lf *fix, const char *name);

/*
 * What lf_cell builds: the cell type's name, its parameter values (n_params must equal the
 * definition's count; NULL params: the definition's defaults), the cell's name (NULL: unnamed) and
 * its provenance (0: none).
 */
typedef struct lf_cell_spec {
    const char *type;
    const odin3_value *params;
    uint32_t n_params;
    const char *name;
    odin3_prov_id prov;
} lf_cell_spec;

/*
 * Adds the cell to mod with every port of the cell a module port of the same name (an input port
 * of the cell an input of the module, an output an output), each as wide as the cell's port, and
 * connects the cell's pins to the nets of those port wires. Returns the cell node.
 */
odin3_node_id lf_cell(lf *fix, odin3_module *mod, const lf_cell_spec *spec);

/* lf_cell with `prefix` put before every port wire name, so several cells can share a module. */
odin3_node_id lf_cell_prefixed(lf *fix, odin3_module *mod, const lf_cell_spec *spec,
                               const char *prefix);

/* A one-module design with one cell, the common case: module "m" in a new fixture. */
odin3_node_id lf_one_cell(lf *fix, const lf_cell_spec *spec, odin3_module **mod);

/* The net of bit `bit` of the module's wire `name`. */
odin3_net_id lf_wnet(lf *fix, odin3_module *mod, const char *name, uint32_t bit);

/* The gate-level nodes of mod by type name: number of live nodes of that type. */
uint32_t lf_count_type(lf *fix, odin3_module *mod, const char *type);

/* Live nodes of mod, and live nodes that are not port nodes (the cells). */
uint32_t lf_live_nodes(const odin3_module *mod);

/*
 * Appends a text that changes whenever a live node of any module does: per module, per live node in
 * ID order its type, name, provenance and the net on each pin (with that net's name). Unlike
 * write_rtlil it covers every cell type (latches and $memrd have no RTLIL form).
 */
odin3_status lf_signature(lf *fix, odin3_strbuf *out);

/* Appends write_rtlil's text of the design (odin3_rtlil_text). */
odin3_status lf_rtlil(lf *fix, odin3_strbuf *out);

/* How lf_sim_equal drives the two designs. */
typedef struct lf_sim_opts {
    uint32_t exhaustive_bits; /* total input bits up to which every vector is tried */
    uint32_t random_vectors;  /* above that: this many seeded random vectors */
    uint64_t seed;
} lf_sim_opts;

/* The outcome of lf_sim_equal. */
typedef struct lf_sim_result {
    bool equal;
    uint64_t vectors; /* vectors compared */
    char why[160];    /* the first mismatch, or why the simulators could not be built */
} lf_sim_result;

/*
 * Simulates module a of design A and module b of design B (the same ports, in the same order) on
 * the same vectors and compares every output bit after each: exhaustively when the total input
 * width is at most opts->exhaustive_bits, else on opts->random_vectors seeded random vectors.
 * Both modules must build in the simulator (clock-free combinational cells).
 */
lf_sim_result lf_sim_equal(odin3_design *design_a, odin3_module_id top_a, odin3_design *design_b,
                           odin3_module_id top_b, const lf_sim_opts *opts);

/*
 * The size estimate (lw_est_*) of the rule for cell type `type` on a cell with these parameters
 * and port widths (in the definition's port order, the output last).
 */
uint64_t lf_estimate(const char *type, const odin3_value *params, const uint32_t *widths,
                     uint32_t n_ports);

/* Live bit nodes of mod that are not constants (the gates), and live word nodes. */
uint32_t lf_gate_nodes(lf *fix, odin3_module *mod);
uint32_t lf_word_nodes(lf *fix, odin3_module *mod);

/* Most input or output bits of a module lf_sim_model drives. */
enum { LF_MODEL_BITS = 1024 };

/*
 * A reference model written in the test (for the cells with no simulate hook): `in` holds the
 * module's input bits, port by port, LSB first (0 or 1); the model writes the output bits likewise
 * into `out` and sets care[k] = 0 for a bit it leaves open (an x of the cell).
 */
typedef struct lf_model_io {
    const uint8_t *in;
    uint8_t *out;
    uint8_t *care;
} lf_model_io;
typedef void (*lf_model_fn)(const lf_model_io *io, const void *user);

/*
 * Simulates module `top` of the design on the vectors of `opts` (as lf_sim_equal) and compares
 * every output bit the model cares about with the model's.
 */
lf_sim_result lf_sim_model(odin3_design *design, odin3_module_id top, lf_model_fn model,
                           const void *user, const lf_sim_opts *opts);

#endif
