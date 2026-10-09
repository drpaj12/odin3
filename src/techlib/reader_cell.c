/* reader_cell.c — .o3lib cell statements: cell, param, ports, fn, seq, memory, end. */
#include "techlib/reader_internal.h"
#include "techlib/width.h"

#include <string.h>

enum { NOT_FOUND = -1 };

/* What an integer expression over the parameters must evaluate to with the defaults: 1..max. */
typedef struct int_rule {
    const char *what;
    int64_t max;
} int_rule;

void odin3_rd_cell_init(odin3_rd_cell *cell) {
    memset(cell, 0, sizeof *cell);
    odin3_vec_init(&cell->ports, sizeof(odin3_rd_port));
    odin3_vec_init(&cell->params, sizeof(odin3_rd_param));
    odin3_vec_init(&cell->fns, sizeof(odin3_techlib_fn));
    odin3_vec_init(&cell->seqs, sizeof(odin3_techlib_seq));
    odin3_vec_init(&cell->mports, sizeof(odin3_techlib_memport));
}

void odin3_rd_cell_free(odin3_rd_cell *cell) {
    odin3_vec_free(&cell->ports);
    odin3_vec_free(&cell->params);
    odin3_vec_free(&cell->fns);
    odin3_vec_free(&cell->seqs);
    odin3_vec_free(&cell->mports);
}

static void cell_reset(odin3_rd_cell *cell) {
    odin3_vec_clear(&cell->ports);
    odin3_vec_clear(&cell->params);
    odin3_vec_clear(&cell->fns);
    odin3_vec_clear(&cell->seqs);
    odin3_vec_clear(&cell->mports);
    cell->area = 0;
    cell->delay = 0;
    cell->has_memory = false;
    cell->total_width = 0;
    memset(&cell->memory, 0, sizeof cell->memory);
}

const char *odin3_rd_cell_name(const odin3_reader *rd) {
    return odin3_strtab_get(rd->strtab, rd->cell.name);
}

/* --- names --------------------------------------------------------------------------------- */

static int port_index(const odin3_rd_cell *cell, uint32_t name) {
    for (size_t i = 0; i < cell->ports.len; i++) {
        const odin3_rd_port *port = odin3_vec_cat(&cell->ports, i);
        if (port->name == name) {
            return (int)i;
        }
    }
    return NOT_FOUND;
}

static int param_index(const odin3_rd_cell *cell, uint32_t name) {
    for (size_t i = 0; i < cell->params.len; i++) {
        const odin3_rd_param *param = odin3_vec_cat(&cell->params, i);
        if (param->name == name) {
            return (int)i;
        }
    }
    return NOT_FOUND;
}

static odin3_rd_port *port_at(odin3_reader *rd, uint32_t idx) {
    return odin3_vec_at(&rd->cell.ports, idx);
}

/* Words that cannot name a port or parameter: modifiers and the `init x` value. */
static bool reserved(odin3_span name) {
    return odin3_rd_is(name, "signed") || odin3_rd_is(name, "clock") || odin3_rd_is(name, "x");
}

/* Interns a new port or parameter name; an error if reserved or the cell already uses it. */
static odin3_status declare_name(odin3_reader *rd, odin3_span name, uint32_t *id) {
    if (reserved(name)) {
        return odin3_rd_err(rd, odin3_rd_at(rd, name.col), "'%.*s' is a reserved word",
                            (int)name.len, name.ptr);
    }
    odin3_status st = odin3_rd_intern(rd, name, id);
    if (st != ODIN3_OK) {
        return st;
    }
    if (port_index(&rd->cell, *id) != NOT_FOUND || param_index(&rd->cell, *id) != NOT_FOUND) {
        return odin3_rd_err(rd, odin3_rd_at(rd, name.col), "duplicate name '%.*s' in cell '%s'",
                            (int)name.len, name.ptr, odin3_rd_cell_name(rd));
    }
    return ODIN3_OK;
}

/* The port called name, or NOT_FOUND. */
static int find_port(const odin3_reader *rd, odin3_span name) {
    uint32_t id = 0;
    if (!odin3_strtab_find(rd->strtab, (odin3_bytes){name.ptr, name.len}, &id)) {
        return NOT_FOUND;
    }
    return port_index(&rd->cell, id);
}

static odin3_status take_input(odin3_reader *rd, odin3_span name, uint32_t *idx) {
    int found = find_port(rd, name);
    const odin3_rd_loc loc = odin3_rd_at(rd, name.col);
    if (found == NOT_FOUND) {
        return odin3_rd_err(rd, loc, "'%.*s' is not a port of cell '%s'", (int)name.len, name.ptr,
                            odin3_rd_cell_name(rd));
    }
    if (port_at(rd, (uint32_t)found)->def.dir != ODIN3_DIR_IN) {
        return odin3_rd_err(rd, loc, "'%.*s' is not an input port of cell '%s'", (int)name.len,
                            name.ptr, odin3_rd_cell_name(rd));
    }
    *idx = (uint32_t)found;
    return ODIN3_OK;
}

/* An input declared with `clock` (seq clocks and the clocks of sync memory ports). */
static odin3_status take_clock(odin3_reader *rd, odin3_span name, uint32_t *idx) {
    odin3_status st = take_input(rd, name, idx);
    if (st == ODIN3_OK && !port_at(rd, *idx)->mods.clock) {
        return odin3_rd_err(rd, odin3_rd_at(rd, name.col),
                            "'%.*s' is not a clock input of cell '%s' (declare it with 'clock')",
                            (int)name.len, name.ptr, odin3_rd_cell_name(rd));
    }
    return st;
}

/* The undriven output a fn or seq statement starts with. */
static odin3_status take_output(odin3_reader *rd, odin3_span *rest, uint32_t *idx) {
    odin3_span name;
    if (!odin3_rd_ident(rest, &name)) {
        return odin3_rd_err(rd, odin3_rd_at(rd, rest->col), "expected an output port name");
    }
    int found = find_port(rd, name);
    const odin3_rd_loc loc = odin3_rd_at(rd, name.col);
    if (found == NOT_FOUND || port_at(rd, (uint32_t)found)->def.dir != ODIN3_DIR_OUT) {
        return odin3_rd_err(rd, loc, "'%.*s' is not an output port of cell '%s'", (int)name.len,
                            name.ptr, odin3_rd_cell_name(rd));
    }
    const odin3_rd_port *port = port_at(rd, (uint32_t)found);
    if (port->driver_line != 0) {
        return odin3_rd_err(rd, loc, "output '%.*s' is already driven (line %u)", (int)name.len,
                            name.ptr, port->driver_line);
    }
    *idx = (uint32_t)found;
    return ODIN3_OK;
}

/* Every identifier of expr names a parameter (or, when ports_ok, a port) declared so far. */
static odin3_status check_idents(odin3_reader *rd, const odin3_expr *expr, bool ports_ok) {
    odin3_vec_clear(&rd->idents);
    odin3_status st = odin3_expr_collect_idents(expr, &rd->idents);
    for (size_t i = 0; st == ODIN3_OK && i < rd->idents.len; i++) {
        const odin3_expr *const *node = (const odin3_expr *const *)odin3_vec_cat(&rd->idents, i);
        uint32_t ident = (*node)->ident;
        bool known = param_index(&rd->cell, ident) != NOT_FOUND ||
                     (ports_ok && port_index(&rd->cell, ident) != NOT_FOUND);
        if (!known) {
            st = odin3_rd_err(rd, odin3_rd_at(rd, (*node)->col), "'%s' is not a %s of cell '%s'",
                              odin3_strtab_get(rd->strtab, ident),
                              ports_ok ? "port or parameter" : "parameter", odin3_rd_cell_name(rd));
        }
    }
    return st;
}

static bool default_lookup(const void *user, uint32_t ident, int64_t *value) {
    const odin3_rd_cell *cell = user;
    int idx = param_index(cell, ident);
    if (idx == NOT_FOUND) {
        return false;
    }
    const odin3_rd_param *param = odin3_vec_cat(&cell->params, (size_t)idx);
    *value = param->dflt;
    return true;
}

/* A parsed integer expression and its value with the default parameters. */
typedef struct int_value {
    const odin3_expr *expr;
    int64_t value;
} int_value;

/*
 * An integer expression over the parameters, in 1..rule->max with their defaults. Afterwards
 * rd->idents holds its identifier nodes (empty for a constant expression).
 */
static odin3_status param_expr(odin3_reader *rd, odin3_span text, const int_rule *rule,
                               int_value *out) {
    const odin3_rd_loc loc = odin3_rd_at(rd, odin3_rd_trim(text).col);
    odin3_status st = odin3_rd_expr(rd, text, &out->expr);
    if (st == ODIN3_OK) {
        st = check_idents(rd, out->expr, false);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_expr_env env = {default_lookup, &rd->cell, rd->strtab};
    odin3_expr_error err = {0, ""};
    st = odin3_expr_eval_int_quiet(out->expr, &env, &out->value, &err);
    if (st == ODIN3_ERR_NO_MEMORY) {
        return st;
    }
    if (st != ODIN3_OK) {
        return odin3_rd_err(rd, odin3_rd_at(rd, err.col),
                            "%s does not evaluate with the default parameters: %s", rule->what,
                            err.text);
    }
    if (out->value < 1 || out->value > rule->max) {
        return odin3_rd_err(rd, loc, "%s must be in 1..%lld (is %lld with the default parameters)",
                            rule->what, (long long)rule->max, (long long)out->value);
    }
    return ODIN3_OK;
}

static odin3_status no_blackbox(const odin3_reader *rd) {
    if (rd->cell.kind == ODIN3_TECHLIB_BLACKBOX) {
        return odin3_rd_err(rd, odin3_rd_at(rd, 0), "a blackbox cell has no fn, seq or memory");
    }
    return ODIN3_OK;
}

/* --- cell ---------------------------------------------------------------------------------- */

static bool kind_of(odin3_span word, odin3_techlib_kind *kind) {
    static const char *const names[] = {"gate", "hard", "blackbox"};
    static const odin3_techlib_kind kinds[] = {ODIN3_TECHLIB_GATE, ODIN3_TECHLIB_HARD,
                                               ODIN3_TECHLIB_BLACKBOX};
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        if (odin3_rd_is(word, names[i])) {
            *kind = kinds[i];
            return true;
        }
    }
    return false;
}

static size_t digits(odin3_span word, size_t from) {
    size_t end = from;
    while (end < word.len && word.ptr[end] >= '0' && word.ptr[end] <= '9') {
        end++;
    }
    return end - from;
}

/* Digits with an optional fraction: 1, 0.25, 12.5. */
static bool is_number(odin3_span word) {
    size_t whole = digits(word, 0);
    if (whole == 0) {
        return false;
    }
    if (whole == word.len) {
        return true;
    }
    if (word.ptr[whole] != '.') {
        return false;
    }
    size_t frac = digits(word, whole + 1);
    return frac > 0 && whole + 1 + frac == word.len;
}

/* `area NUM` or `delay NUM` on the cell line. */
static odin3_status cell_option(odin3_reader *rd, odin3_span *rest, odin3_span key) {
    const odin3_rd_loc loc = odin3_rd_at(rd, key.col);
    uint32_t *slot = NULL;
    if (odin3_rd_is(key, "area")) {
        slot = &rd->cell.area;
    } else if (odin3_rd_is(key, "delay")) {
        slot = &rd->cell.delay;
    } else {
        return odin3_rd_err(rd, loc, "unexpected '%.*s'", (int)key.len, key.ptr);
    }
    if (*slot != 0) {
        return odin3_rd_err(rd, loc, "'%.*s' given twice", (int)key.len, key.ptr);
    }
    odin3_span num;
    if (!odin3_rd_word(rest, &num)) {
        return odin3_rd_err(rd, loc, "expected a number after '%.*s'", (int)key.len, key.ptr);
    }
    if (!is_number(num)) {
        return odin3_rd_err(rd, odin3_rd_at(rd, num.col), "'%.*s' is not a number", (int)num.len,
                            num.ptr);
    }
    return odin3_rd_intern(rd, num, slot);
}

static odin3_status check_cell_name(odin3_reader *rd, odin3_span name, uint32_t id) {
    const odin3_rd_loc loc = odin3_rd_at(rd, name.col);
    for (size_t i = 0; i < rd->pending.len; i++) {
        const odin3_rd_pending *done = odin3_vec_cat(&rd->pending, i);
        if (done->name == id) {
            return odin3_rd_err(rd, loc, "duplicate cell '%.*s' (the first is on line %u)",
                                (int)name.len, name.ptr, done->line);
        }
    }
    if (odin3_celltype_find(rd->design, id, NULL)) {
        return odin3_rd_err(rd, loc, "cell type '%.*s' already exists in the design", (int)name.len,
                            name.ptr);
    }
    return ODIN3_OK;
}

odin3_status odin3_rd_st_cell(odin3_reader *rd, odin3_span rest) {
    odin3_span name;
    odin3_span kind_word;
    odin3_techlib_kind kind = ODIN3_TECHLIB_GATE;
    if (!odin3_rd_word(&rest, &name)) {
        return odin3_rd_err(rd, odin3_rd_at(rd, rest.col), "expected a cell name");
    }
    if (!odin3_rd_word(&rest, &kind_word) || !kind_of(kind_word, &kind)) {
        return odin3_rd_err(rd, odin3_rd_at(rd, kind_word.col),
                            "expected the cell kind gate, hard or blackbox");
    }
    uint32_t id = 0;
    odin3_status st = odin3_rd_intern(rd, name, &id);
    if (st == ODIN3_OK) {
        st = check_cell_name(rd, name, id);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    cell_reset(&rd->cell);
    rd->cell.name = id;
    rd->cell.line = rd->line;
    rd->cell.kind = kind;
    odin3_span key;
    while (st == ODIN3_OK && odin3_rd_word(&rest, &key)) {
        st = cell_option(rd, &rest, key);
    }
    rd->in_cell = st == ODIN3_OK;
    return st;
}

/* --- param and ports ----------------------------------------------------------------------- */

odin3_status odin3_rd_st_param(odin3_reader *rd, odin3_span rest) {
    odin3_span name;
    odin3_span kind;
    if (!odin3_rd_ident(&rest, &name)) {
        return odin3_rd_err(rd, odin3_rd_at(rd, rest.col), "expected a parameter name");
    }
    uint32_t id = 0;
    odin3_status st = declare_name(rd, name, &id);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!odin3_rd_word(&rest, &kind) || !odin3_rd_is(kind, "int")) {
        return odin3_rd_err(rd, odin3_rd_at(rd, kind.col), "only 'int' parameters are supported");
    }
    const odin3_expr *expr = NULL;
    int64_t value = 0;
    st = odin3_rd_expr(rd, rest, &expr);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_expr_error err = {0, ""};
    const odin3_expr_env names = {NULL, NULL, rd->strtab}; /* names identifiers in messages */
    st = odin3_expr_eval_int_quiet(expr, &names, &value, &err);
    if (st != ODIN3_OK && st != ODIN3_ERR_NO_MEMORY) {
        return odin3_rd_err(rd, odin3_rd_at(rd, err.col),
                            "the default of '%.*s' must be a constant integer: %s", (int)name.len,
                            name.ptr, err.text);
    }
    odin3_rd_param *param = st == ODIN3_OK ? odin3_vec_push(&rd->cell.params) : NULL;
    if (param == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *param = (odin3_rd_param){id, value};
    return ODIN3_OK;
}

/* Strips trailing `signed` / `clock` words (any order, each once) off the end of rest. */
static odin3_status strip_mods(const odin3_reader *rd, odin3_span *rest, odin3_techlib_port *mods) {
    for (;;) {
        odin3_span text = odin3_rd_trim(*rest);
        size_t start = text.len;
        while (start > 0 && !odin3_rd_space(text.ptr[start - 1])) {
            start--;
        }
        const odin3_span last = {text.ptr + start, text.len - start, text.col + (uint32_t)start};
        bool *flag = NULL;
        if (odin3_rd_is(last, "signed")) {
            flag = &mods->is_signed;
        } else if (odin3_rd_is(last, "clock")) {
            flag = &mods->clock;
        } else {
            *rest = text;
            return ODIN3_OK;
        }
        if (*flag) {
            return odin3_rd_err(rd, odin3_rd_at(rd, last.col), "'%.*s' given twice", (int)last.len,
                                last.ptr);
        }
        *flag = true;
        text.len = start;
        *rest = text;
    }
}

/* The cell's parameter names (strtab IDs, declaration order) into rd->ids. */
static odin3_status param_names(odin3_reader *rd) {
    odin3_vec_clear(&rd->ids);
    for (size_t i = 0; i < rd->cell.params.len; i++) {
        const odin3_rd_param *param = odin3_vec_cat(&rd->cell.params, i);
        uint32_t *slot = odin3_vec_push(&rd->ids);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = param->name;
    }
    return ODIN3_OK;
}

/*
 * The width rule of a port: a constant (any expression without identifiers, folded), a
 * parameter's name, or a compiled expression.
 */
static odin3_status port_width(odin3_reader *rd, odin3_span text, odin3_port_def *def) {
    static const int_rule rule = {"width", ODIN3_READER_MAX_WIDTH};
    int_value width = {NULL, 0};
    odin3_status st = param_expr(rd, text, &rule, &width);
    if (st != ODIN3_OK) {
        return st;
    }
    rd->cell.total_width += (uint64_t)width.value;
    if (rd->cell.total_width > ODIN3_READER_MAX_TOTAL_WIDTH) {
        return odin3_rd_err(rd, odin3_rd_at(rd, odin3_rd_trim(text).col),
                            "the ports of cell '%s' total %llu bits with the default parameters, "
                            "above %u",
                            odin3_rd_cell_name(rd), (unsigned long long)rd->cell.total_width,
                            ODIN3_READER_MAX_TOTAL_WIDTH);
    }
    if (rd->idents.len == 0) {
        def->width = (uint32_t)width.value;
        def->scalar = def->width == 1;
        return ODIN3_OK;
    }
    if (width.expr->kind == ODIN3_EXPR_IDENT) {
        def->width_param = odin3_strtab_get(rd->strtab, width.expr->ident);
        return ODIN3_OK;
    }
    st = param_names(rd);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_width_source src = {rd->arena, rd->strtab, width.expr, rd->ids.data,
                                    (uint32_t)rd->ids.len};
    return odin3_width_expr_compile(&src, &def->width_expr);
}

static odin3_status declare_port(odin3_reader *rd, odin3_span rest, odin3_dir dir) {
    odin3_span name;
    if (!odin3_rd_ident(&rest, &name)) {
        return odin3_rd_err(rd, odin3_rd_at(rd, rest.col), "expected a port name");
    }
    odin3_rd_port port;
    memset(&port, 0, sizeof port);
    odin3_status st = declare_name(rd, name, &port.name);
    if (st == ODIN3_OK) {
        st = strip_mods(rd, &rest, &port.mods);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    if (port.mods.clock && dir != ODIN3_DIR_IN) {
        return odin3_rd_err(rd, odin3_rd_at(rd, name.col), "'clock' is only allowed on an input");
    }
    if (rest.len == 0) {
        return odin3_rd_err(rd, odin3_rd_at(rd, rest.col), "expected a width after '%.*s'",
                            (int)name.len, name.ptr);
    }
    port.def.name = odin3_strtab_get(rd->strtab, port.name);
    port.def.dir = dir;
    port.line = rd->line;
    st = port_width(rd, rest, &port.def);
    odin3_rd_port *slot = st == ODIN3_OK ? odin3_vec_push(&rd->cell.ports) : NULL;
    if (slot != NULL) {
        *slot = port;
    }
    return st == ODIN3_OK && slot == NULL ? ODIN3_ERR_NO_MEMORY : st;
}

odin3_status odin3_rd_st_in(odin3_reader *rd, odin3_span rest) {
    return declare_port(rd, rest, ODIN3_DIR_IN);
}

odin3_status odin3_rd_st_out(odin3_reader *rd, odin3_span rest) {
    return declare_port(rd, rest, ODIN3_DIR_OUT);
}

odin3_status odin3_rd_st_inout(odin3_reader *rd, odin3_span rest) {
    return declare_port(rd, rest, ODIN3_DIR_INOUT);
}

/* --- fn and seq ---------------------------------------------------------------------------- */

odin3_status odin3_rd_st_fn(odin3_reader *rd, odin3_span rest) {
    odin3_techlib_fn fn = {0, NULL, rd->line};
    odin3_status st = no_blackbox(rd);
    if (st == ODIN3_OK) {
        st = take_output(rd, &rest, &fn.port);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    if (!odin3_rd_lit(&rest, "=")) {
        return odin3_rd_err(rd, odin3_rd_at(rd, rest.col), "expected '=' after the output");
    }
    st = odin3_rd_expr(rd, rest, &fn.expr);
    if (st == ODIN3_OK) {
        st = check_idents(rd, fn.expr, true);
    }
    odin3_techlib_fn *slot = st == ODIN3_OK ? odin3_vec_push(&rd->cell.fns) : NULL;
    if (slot == NULL) {
        return st == ODIN3_OK ? ODIN3_ERR_NO_MEMORY : st;
    }
    *slot = fn;
    port_at(rd, fn.port)->driver_line = rd->line;
    return ODIN3_OK;
}

static bool trigger_of(odin3_span word, odin3_techlib_trigger *trigger) {
    static const char *const names[] = {"posedge", "negedge", "high", "low"};
    static const odin3_techlib_trigger triggers[] = {ODIN3_TECHLIB_POSEDGE, ODIN3_TECHLIB_NEGEDGE,
                                                     ODIN3_TECHLIB_HIGH, ODIN3_TECHLIB_LOW};
    for (size_t i = 0; i < sizeof triggers / sizeof triggers[0]; i++) {
        if (odin3_rd_is(word, names[i])) {
            *trigger = triggers[i];
            return true;
        }
    }
    return false;
}

/* Optional `init EXPR`; `init x` (or none) leaves *init NULL. */
static odin3_status seq_init(odin3_reader *rd, odin3_span rest, const odin3_expr **init) {
    odin3_span word;
    if (!odin3_rd_word(&rest, &word)) {
        return ODIN3_OK;
    }
    if (!odin3_rd_is(word, "init")) {
        return odin3_rd_err(rd, odin3_rd_at(rd, word.col), "unexpected '%.*s' (expected 'init')",
                            (int)word.len, word.ptr);
    }
    odin3_span text = odin3_rd_trim(rest);
    if (odin3_rd_is(text, "x") || odin3_rd_is(text, "X")) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_rd_expr(rd, text, init);
    return st == ODIN3_OK ? check_idents(rd, *init, false) : st;
}

/* `TRIGGER CLK [init EXPR]` after the '@'. */
static odin3_status seq_trigger(odin3_reader *rd, odin3_span rest, odin3_techlib_seq *seq) {
    odin3_span word;
    odin3_span clock;
    if (!odin3_rd_word(&rest, &word) || !trigger_of(word, &seq->trigger)) {
        return odin3_rd_err(rd, odin3_rd_at(rd, word.col),
                            "expected posedge, negedge, high or low after '@'");
    }
    if (!odin3_rd_ident(&rest, &clock)) {
        return odin3_rd_err(rd, odin3_rd_at(rd, rest.col), "expected a clock port after '%.*s'",
                            (int)word.len, word.ptr);
    }
    odin3_status st = take_clock(rd, clock, &seq->clock);
    return st == ODIN3_OK ? seq_init(rd, rest, &seq->init) : st;
}

odin3_status odin3_rd_st_seq(odin3_reader *rd, odin3_span rest) {
    odin3_techlib_seq seq;
    memset(&seq, 0, sizeof seq);
    seq.line = rd->line;
    odin3_status st = no_blackbox(rd);
    if (st == ODIN3_OK) {
        st = take_output(rd, &rest, &seq.port);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    if (!odin3_rd_lit(&rest, "<=")) {
        return odin3_rd_err(rd, odin3_rd_at(rd, rest.col), "expected '<=' after the output");
    }
    const char *at_sign = memchr(rest.ptr, '@', rest.len);
    if (at_sign == NULL) {
        return odin3_rd_err(rd, odin3_rd_at(rd, rest.col), "expected '@' and a trigger");
    }
    const odin3_span data = {rest.ptr, (size_t)(at_sign - rest.ptr), rest.col};
    odin3_rd_advance(&rest, data.len + 1);
    st = odin3_rd_expr(rd, data, &seq.data);
    st = st == ODIN3_OK ? check_idents(rd, seq.data, true) : st;
    st = st == ODIN3_OK ? seq_trigger(rd, rest, &seq) : st;
    odin3_techlib_seq *slot = st == ODIN3_OK ? odin3_vec_push(&rd->cell.seqs) : NULL;
    if (slot == NULL) {
        return st == ODIN3_OK ? ODIN3_ERR_NO_MEMORY : st;
    }
    *slot = seq;
    port_at(rd, seq.port)->driver_line = rd->line;
    return ODIN3_OK;
}

/* --- memory -------------------------------------------------------------------------------- */

odin3_status odin3_rd_st_memory(odin3_reader *rd, odin3_span rest) {
    static const int_rule rule = {"memory words", INT64_MAX};
    odin3_status st = no_blackbox(rd);
    if (st != ODIN3_OK) {
        return st;
    }
    if (rd->cell.has_memory) {
        return odin3_rd_err(rd, odin3_rd_at(rd, 0),
                            "a cell has at most one memory (the first is on line %u)",
                            rd->cell.memory.line);
    }
    odin3_span word;
    if (!odin3_rd_word(&rest, &word) || !odin3_rd_is(word, "words")) {
        return odin3_rd_err(rd, odin3_rd_at(rd, word.col), "expected 'words' after 'memory'");
    }
    int_value words = {NULL, 0};
    st = param_expr(rd, rest, &rule, &words);
    rd->cell.memory.words = words.expr;
    if (st == ODIN3_OK) {
        rd->cell.has_memory = true;
        rd->cell.memory.line = rd->line;
    }
    return st;
}

static odin3_status need_memory(const odin3_reader *rd, const char *keyword) {
    if (!rd->cell.has_memory) {
        return odin3_rd_err(rd, odin3_rd_at(rd, 0), "'%s' needs a 'memory' statement before it",
                            keyword);
    }
    return ODIN3_OK;
}

odin3_status odin3_rd_st_mem_width(odin3_reader *rd, odin3_span rest) {
    static const int_rule rule = {"memory width", UINT32_MAX};
    odin3_status st = need_memory(rd, "width");
    if (st != ODIN3_OK) {
        return st;
    }
    if (rd->cell.memory.width != NULL) {
        return odin3_rd_err(rd, odin3_rd_at(rd, 0), "'width' given twice");
    }
    int_value width = {NULL, 0};
    st = param_expr(rd, rest, &rule, &width);
    rd->cell.memory.width = st == ODIN3_OK ? width.expr : NULL;
    return st;
}

/* Copies a vec's elements into the arena; NULL for an empty vec. *ok false on out of memory. */
static void *copy_array(odin3_arena *arena, const odin3_vec *vec, bool *ok) {
    if (vec->len == 0) {
        return NULL;
    }
    void *mem = odin3_arena_alloc(arena, vec->len * vec->elem_size);
    if (mem == NULL) {
        *ok = false;
        return NULL;
    }
    memcpy(mem, vec->data, vec->len * vec->elem_size);
    return mem;
}

/* The input ports a `write`/`read` statement lists, into rd->ids; a sync port's first is a clock.
 */
static odin3_status mem_port_list(odin3_reader *rd, odin3_span rest, bool sync) {
    odin3_vec_clear(&rd->ids);
    odin3_span name;
    while (odin3_rd_ident(&rest, &name)) {
        uint32_t idx = 0;
        odin3_status st =
            sync && rd->ids.len == 0 ? take_clock(rd, name, &idx) : take_input(rd, name, &idx);
        if (st != ODIN3_OK) {
            return st;
        }
        uint32_t *slot = odin3_vec_push(&rd->ids);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = idx;
    }
    return odin3_rd_end(rd, rest);
}

static odin3_status mem_port(odin3_reader *rd, odin3_span rest, bool write) {
    const char *keyword = write ? "write" : "read";
    odin3_status st = need_memory(rd, keyword);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_span mode;
    if (!odin3_rd_word(&rest, &mode) ||
        (!odin3_rd_is(mode, "sync") && !odin3_rd_is(mode, "async"))) {
        return odin3_rd_err(rd, odin3_rd_at(rd, mode.col), "expected sync or async after '%s'",
                            keyword);
    }
    odin3_techlib_memport port = {write, odin3_rd_is(mode, "sync"), NULL, 0, rd->line};
    st = mem_port_list(rd, rest, port.sync);
    if (st != ODIN3_OK) {
        return st;
    }
    if (port.sync && rd->ids.len == 0) {
        return odin3_rd_err(rd, odin3_rd_at(rd, 0), "a sync %s port lists its clock port first",
                            keyword);
    }
    bool ok = true;
    port.ports = copy_array(rd->arena, &rd->ids, &ok);
    port.n_ports = (uint32_t)rd->ids.len;
    odin3_techlib_memport *slot = ok ? odin3_vec_push(&rd->cell.mports) : NULL;
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = port;
    return ODIN3_OK;
}

odin3_status odin3_rd_st_mem_write(odin3_reader *rd, odin3_span rest) {
    return mem_port(rd, rest, true);
}

odin3_status odin3_rd_st_mem_read(odin3_reader *rd, odin3_span rest) {
    return mem_port(rd, rest, false);
}

/* --- end: drivers, definition, library data ------------------------------------------------ */

static bool has_read_port(const odin3_rd_cell *cell) {
    for (size_t i = 0; i < cell->mports.len; i++) {
        const odin3_techlib_memport *port = odin3_vec_cat(&cell->mports, i);
        if (!port->write) {
            return true;
        }
    }
    return false;
}

/* The memory drives every output no fn or seq drives. */
static odin3_status finish_memory(odin3_reader *rd) {
    odin3_rd_cell *cell = &rd->cell;
    const odin3_rd_loc loc = {cell->memory.line, 0};
    if (cell->memory.width == NULL) {
        return odin3_rd_err(rd, loc, "the memory needs a 'width' statement");
    }
    if (!has_read_port(cell)) {
        return odin3_rd_err(rd, loc, "the memory needs a 'read' port");
    }
    odin3_vec_clear(&rd->ids);
    for (uint32_t i = 0; i < (uint32_t)cell->ports.len; i++) {
        const odin3_rd_port *port = port_at(rd, i);
        uint32_t *slot = NULL;
        if (port->def.dir == ODIN3_DIR_OUT && port->driver_line == 0) {
            slot = odin3_vec_push(&rd->ids);
            if (slot == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *slot = i;
        }
    }
    if (rd->ids.len == 0) {
        return odin3_rd_err(rd, loc, "the memory drives no output (each has a fn or seq)");
    }
    bool ok = true;
    cell->memory.outs = copy_array(rd->arena, &rd->ids, &ok);
    cell->memory.n_outs = (uint32_t)rd->ids.len;
    for (size_t i = 0; ok && i < rd->ids.len; i++) {
        const uint32_t *idx = odin3_vec_cat(&rd->ids, i);
        port_at(rd, *idx)->driver_line = cell->memory.line;
    }
    return ok ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* Review Focus 4: outside a blackbox every output needs a driver. */
static odin3_status check_drivers(const odin3_reader *rd) {
    if (rd->cell.kind == ODIN3_TECHLIB_BLACKBOX) {
        return ODIN3_OK;
    }
    for (size_t i = 0; i < rd->cell.ports.len; i++) {
        const odin3_rd_port *port = odin3_vec_cat(&rd->cell.ports, i);
        if (port->def.dir == ODIN3_DIR_OUT && port->driver_line == 0) {
            return odin3_rd_err(rd, (odin3_rd_loc){port->line, 0},
                                "output '%s' of cell '%s' has no fn, seq or memory driving it",
                                port->def.name, odin3_rd_cell_name(rd));
        }
    }
    return ODIN3_OK;
}

static odin3_granularity gran_of(odin3_techlib_kind kind) {
    switch (kind) {
    case ODIN3_TECHLIB_GATE:
        return ODIN3_GRAN_BIT;
    case ODIN3_TECHLIB_HARD:
        return ODIN3_GRAN_HARD;
    default:
        return ODIN3_GRAN_BLACKBOX;
    }
}

static const odin3_celltype_def *build_def(odin3_reader *rd) {
    const odin3_rd_cell *cell = &rd->cell;
    odin3_celltype_def *def = odin3_arena_alloc(rd->arena, sizeof *def);
    odin3_port_def *ports = odin3_arena_alloc(rd->arena, sizeof *ports * (cell->ports.len + 1));
    odin3_param_def *params = odin3_arena_alloc(rd->arena, sizeof *params * (cell->params.len + 1));
    if (def == NULL || ports == NULL || params == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < cell->ports.len; i++) {
        const odin3_rd_port *port = odin3_vec_cat(&cell->ports, i);
        ports[i] = port->def;
    }
    for (size_t i = 0; i < cell->params.len; i++) {
        const odin3_rd_param *param = odin3_vec_cat(&cell->params, i);
        params[i] = (odin3_param_def){odin3_strtab_get(rd->strtab, param->name), ODIN3_VAL_INT,
                                      odin3_value_int(param->dflt)};
    }
    *def = (odin3_celltype_def){odin3_rd_cell_name(rd),
                                gran_of(cell->kind),
                                0,
                                ports,
                                (uint32_t)cell->ports.len,
                                params,
                                (uint32_t)cell->params.len,
                                NULL,
                                NULL,
                                NULL,
                                NULL};
    return def;
}

static bool build_memory(odin3_reader *rd, odin3_techlib_cell *lib) {
    const odin3_rd_cell *cell = &rd->cell;
    lib->memory = NULL;
    if (!cell->has_memory) {
        return true;
    }
    odin3_techlib_memory *mem = odin3_arena_alloc(rd->arena, sizeof *mem);
    if (mem == NULL) {
        return false;
    }
    bool ok = true;
    *mem = cell->memory;
    mem->mports = copy_array(rd->arena, &cell->mports, &ok);
    mem->n_mports = (uint32_t)cell->mports.len;
    lib->memory = mem;
    return ok;
}

static const odin3_techlib_cell *build_lib(odin3_reader *rd) {
    const odin3_rd_cell *cell = &rd->cell;
    odin3_techlib_cell *lib = odin3_arena_alloc(rd->arena, sizeof *lib);
    odin3_techlib_port *mods = odin3_arena_alloc(rd->arena, sizeof *mods * (cell->ports.len + 1));
    if (lib == NULL || mods == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < cell->ports.len; i++) {
        const odin3_rd_port *port = odin3_vec_cat(&cell->ports, i);
        mods[i] = port->mods;
    }
    bool ok = true;
    const odin3_techlib_fn *fns = copy_array(rd->arena, &cell->fns, &ok);
    const odin3_techlib_seq *seqs = copy_array(rd->arena, &cell->seqs, &ok);
    *lib = (odin3_techlib_cell){.library = rd->library,
                                .file = rd->file,
                                .line = cell->line,
                                .kind = cell->kind,
                                .area = cell->area,
                                .delay = cell->delay,
                                .ports = mods,
                                .n_ports = (uint32_t)cell->ports.len,
                                .fns = fns,
                                .n_fns = (uint32_t)cell->fns.len,
                                .seqs = seqs,
                                .n_seqs = (uint32_t)cell->seqs.len,
                                .memory = NULL};
    return ok && build_memory(rd, lib) ? lib : NULL;
}

odin3_status odin3_rd_st_cell_end(odin3_reader *rd, odin3_span rest) {
    odin3_status st = odin3_rd_end(rd, rest);
    if (st == ODIN3_OK && rd->cell.has_memory) {
        st = finish_memory(rd);
    }
    st = st == ODIN3_OK ? check_drivers(rd) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_rd_pending done = {rd->cell.name, rd->cell.line, build_def(rd), build_lib(rd)};
    odin3_rd_pending *slot =
        done.def != NULL && done.lib != NULL ? odin3_vec_push(&rd->pending) : NULL;
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = done;
    rd->in_cell = false;
    return ODIN3_OK;
}
