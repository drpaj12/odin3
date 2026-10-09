/*
 * celltype.h — cell-type definitions (IR-8), the process-global registry and per-design table
 * (IR-11), and black-box declaration (IR-7b).
 */
#ifndef ODIN3_IR_CELLTYPE_H
#define ODIN3_IR_CELLTYPE_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stdint.h>

/* Direction as seen from the cell: a $port_in's pin is OUT (it drives the module's net). */
typedef enum odin3_dir { ODIN3_DIR_IN, ODIN3_DIR_OUT, ODIN3_DIR_INOUT } odin3_dir;

/* IR-9 granularity tags. */
typedef enum odin3_granularity {
    ODIN3_GRAN_WORD,
    ODIN3_GRAN_BIT,
    ODIN3_GRAN_HARD,
    ODIN3_GRAN_BLACKBOX,
    ODIN3_GRAN_MODULE,
    ODIN3_GRAN_PORT
} odin3_granularity;

/*
 * Definition flags. TRISTATE: output pins may share a net with other tristate/inout drivers (a
 * bus, check rule 4). ANYVIEW: legal in every view whatever the granularity (constant cells, IR-9;
 * check rule 10).
 */
enum { ODIN3_CT_TRISTATE = 1U << 0, ODIN3_CT_ANYVIEW = 1U << 1 };

typedef struct odin3_celltype_def odin3_celltype_def;
typedef struct odin3_width_expr odin3_width_expr;

/*
 * A compiled width expression (the fourth width rule), produced outside the IR (the tech library,
 * src/techlib). The IR only calls its hooks. check runs when a definition is registered: NULL when
 * every identifier of the expression names an INT parameter of def, else a description of the
 * problem. eval computes the width for params (one value per parameter of def):
 * ODIN3_ERR_INVALID_ARG (logged; the caller adds the location) when a parameter it reads is not
 * an INT or the result is not an integer in [0, UINT32_MAX]; *width is untouched then.
 * Registration never copies a width expression: it must live as long as every design holding a
 * type that uses it (a producer allocates it in odin3_celltype_arena).
 */
struct odin3_width_expr {
    const char *(*check)(const odin3_width_expr *wexpr, const odin3_celltype_def *def);
    odin3_status (*eval)(const odin3_width_expr *wexpr, const odin3_celltype_def *def,
                         const odin3_value *params, uint32_t *width);
    const void *impl; /* the producer's compiled form */
};

/*
 * One port. Width rule, first match wins: width_fn (any function of the parameters), else
 * width_expr (an integer expression over INT parameters of the same type), else width_param (the
 * name of an INT parameter of the same type), else the constant width. scalar records whether the
 * port is written without brackets (writers reproduce it).
 */
typedef struct odin3_port_def {
    const char *name;
    odin3_dir dir;
    bool scalar;
    uint32_t width;          /* used when no other rule is set */
    const char *width_param; /* name of an INT parameter giving the width */
    uint32_t (*width_fn)(const odin3_value *params, uint32_t port);
    const odin3_width_expr *width_expr; /* not copied; see odin3_width_expr */
} odin3_port_def;

/* One parameter: name, kind and default value (IR-10). */
typedef struct odin3_param_def {
    const char *name;
    odin3_value_kind kind;
    odin3_value dflt;
} odin3_param_def;

/* Answer of a const_value hook (IR-4). */
typedef enum odin3_const {
    ODIN3_CONST_NONE,
    ODIN3_CONST_0,
    ODIN3_CONST_1,
    ODIN3_CONST_X,
    ODIN3_CONST_Z
} odin3_const;

/*
 * A cell-type definition. Hooks receive one value per parameter definition, in order. Names must
 * be non-empty; port names are unique, parameter names are unique, a width_param names an INT
 * parameter of the same definition, a width_expr has both hooks and passes its check, and every
 * default has its parameter's kind and passes odin3_value_valid.
 */
struct odin3_celltype_def {
    const char *name;
    odin3_granularity gran;
    uint32_t flags;
    const odin3_port_def *ports;
    uint32_t n_ports;
    const odin3_param_def *params;
    uint32_t n_params;
    odin3_status (*verify)(const odin3_value *params);     /* may be NULL */
    odin3_const (*const_value)(const odin3_value *params); /* may be NULL */
};

/*
 * Adds a process-global definition (plugins; built-ins come from the static table in
 * cells/builtin.c). Copies nothing: def and everything it points to must outlive the process.
 * Designs created afterwards instantiate it; existing designs do not see it.
 * ODIN3_ERR_INVALID_ARG (logged) for an invalid definition or a name already registered;
 * ODIN3_ERR_NO_MEMORY on out of memory. Not thread-safe.
 */
odin3_status odin3_celltype_register_global(const odin3_celltype_def *def);

/* True and *out when the design has a cell type named by strtab ID name_str. */
bool odin3_celltype_find(const odin3_design *design, uint32_t name_str, odin3_celltype_id *out);

/*
 * The definition of id, NULL for an invalid or out-of-range ID. A local definition stays valid
 * until the type is redefined (module ports, IR-7) or the design is destroyed.
 */
const odin3_celltype_def *odin3_celltype_get(const odin3_design *design, odin3_celltype_id id);

/*
 * Width of port `port` of type id for the given parameter values (one per parameter definition;
 * may be NULL only for a type without parameters). 0 when id or port is out of range, the width
 * parameter is not an INT in [0, UINT32_MAX], or the width expression fails to evaluate (logged).
 */
uint32_t odin3_celltype_port_width(const odin3_design *design, odin3_celltype_id id,
                                   const odin3_value *params, uint32_t port);

/* A port of a cell type, with the parameter values it is sized by (as for port_width). */
typedef struct odin3_port_query {
    odin3_celltype_id type;
    const odin3_value *params;
    uint32_t port;
} odin3_port_query;

/*
 * odin3_celltype_port_width with the failure reported: ODIN3_ERR_INVALID_ARG (logged) for the
 * cases where port_width returns 0 as an error; *width is untouched then. A width of 0 from a
 * valid rule is ODIN3_OK.
 */
odin3_status odin3_celltype_port_width_checked(const odin3_design *design,
                                               const odin3_port_query *query, uint32_t *width);

/* Number of live nodes of type id (0 for an invalid ID). */
uint32_t odin3_celltype_instances(const odin3_design *design, odin3_celltype_id id);

/*
 * Adds a design-local type (module, black box). The definition (names, ports, parameters and
 * their default payloads) is deep-copied into the design's arena, so def may be temporary.
 * ODIN3_ERR_INVALID_ARG (logged) for an invalid definition or a name the design already has;
 * ODIN3_ERR_NO_MEMORY on out of memory. The table is unchanged on failure.
 */
odin3_status odin3_celltype_add_local(odin3_design *design, const odin3_celltype_def *def,
                                      odin3_celltype_id *out);

/*
 * IR-7b: declares a black box. If the name is already registered, the declaration must match the
 * registered type's ports (count, names, directions, constant widths; a width given by a parameter,
 * function or expression never matches) and that type is reused; otherwise a new local type of
 * granularity BLACKBOX (whatever def->gran says) is added. Either way the type is appended to the
 * design's declared-model list. ODIN3_ERR_INVALID_ARG (logged) for an invalid definition or a
 * mismatch; ODIN3_ERR_NO_MEMORY on out of memory. Nothing changes on failure.
 */
odin3_status odin3_celltype_declare_blackbox(odin3_design *design, const odin3_celltype_def *def,
                                             odin3_celltype_id *out);

/* Declared-model list (declaration order; a model declared twice appears twice). */
uint32_t odin3_design_declared_model_count(const odin3_design *design);

/* Entry index of the declared-model list; {0} when index is out of range. */
odin3_celltype_id odin3_design_declared_model(const odin3_design *design, uint32_t index);

/*
 * The arena local cell-type definitions live in. Data a local type points to without it being
 * copied (width expressions, tech-library data) is allocated here so it lives as long as the
 * design.
 */
odin3_arena *odin3_celltype_arena(const odin3_design *design);

/* Tech-library data of a cell (fields in techlib/reader.h); opaque to the IR. */
typedef struct odin3_techlib_cell odin3_techlib_cell;

/*
 * Attaches tech-library data to local type id, replacing any; lib (may be NULL) is not copied and
 * must live as long as the design (odin3_celltype_arena). ODIN3_ERR_INVALID_ARG (logged) for an
 * invalid ID or a type that is not local.
 */
odin3_status odin3_celltype_set_lib(odin3_design *design, odin3_celltype_id id,
                                    const odin3_techlib_cell *lib);

/* The tech-library data attached to id; NULL when none or for an invalid ID. */
const odin3_techlib_cell *odin3_celltype_lib(const odin3_design *design, odin3_celltype_id id);

#endif
