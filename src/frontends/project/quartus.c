/*
 * quartus.c — the Quartus .qpf/.qsf/.qip reader (PRJ-12..14, the oracle's parse_quartus).
 */
#include "ast/srcman.h"
#include "frontends/project/paths.h"
#include "frontends/project/project.h"
#include "frontends/project/tcl.h"
#include "ir/design.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { DETAIL_MAX = 128 };

/* --- small helpers ------------------------------------------------------------------------- */

static bool bytes_eq(odin3_bytes left, odin3_bytes right) {
    return left.len == right.len && (left.len == 0 || memcmp(left.ptr, right.ptr, left.len) == 0);
}

static bool is_text(odin3_bytes bytes, const char *text) {
    return bytes_eq(bytes, odin3_bytes_cstr(text));
}

static bool is_digit(uint8_t byte) {
    return byte >= '0' && byte <= '9';
}

static bool in_set(uint8_t byte, const char *set) {
    return byte != '\0' && strchr(set, byte) != NULL;
}

/* The bytes of strtab ID str. */
static odin3_bytes str_bytes(const odin3_project *proj, uint32_t str) {
    const odin3_strtab *tab = odin3_design_strtab(proj->design);
    return (odin3_bytes){odin3_strtab_get(tab, str), odin3_strtab_len(tab, str)};
}

/* Everything after the last sep in text (all of it when there is none). */
static odin3_bytes after_last(odin3_bytes text, char sep) {
    const char *bytes = text.ptr;
    size_t j = text.len;
    while (j > 0 && bytes[j - 1] != sep) {
        j--;
    }
    return (odin3_bytes){bytes + j, text.len - j};
}

/* Whether text is the oracle's NUMBER: -?[0-9][0-9_]* or ([0-9][0-9_]*)?'[sS]?[base][digits]+. */
static bool is_plain_number(const uint8_t *text, size_t len) {
    size_t j = len > 0 && text[0] == '-';
    if (j >= len || !is_digit(text[j])) {
        return false;
    }
    while (j < len && (is_digit(text[j]) || text[j] == '_')) {
        j++;
    }
    return j == len;
}

/* The length of the run of bytes from set starting at word[from]. */
static size_t run_of(odin3_bytes word, size_t from, const char *set) {
    const uint8_t *text = word.ptr;
    size_t j = from;
    while (j < word.len && in_set(text[j], set)) {
        j++;
    }
    return j - from;
}

static bool is_number(odin3_bytes word) {
    const uint8_t *text = word.ptr;
    size_t len = word.len;
    if (is_plain_number(text, len)) {
        return true;
    }
    size_t j = 0;
    if (len > 0 && is_digit(text[0])) {
        j = 1 + run_of(word, 1, "0123456789_");
    }
    if (j >= len || text[j] != '\'') {
        return false;
    }
    j++;
    j += run_of(word, j, "sS") > 0;
    if (j >= len || !in_set(text[j], "bBoOdDhH")) {
        return false;
    }
    size_t digits = run_of(word, j + 1, "0123456789abcdefABCDEFxXzZ_?");
    return digits > 0 && j + 1 + digits == len;
}

/* --- state --------------------------------------------------------------------------------- */

/* One .qsf or .qip being run: its file, text, commands and cursor (the explicit qip stack). */
typedef struct qsf_frame {
    odin3_proj_path path;
    odin3_proj_text text;
    odin3_vec commands; /* odin3_tcl_command */
    size_t next;
} qsf_frame;

typedef struct queued_param {
    const odin3_tcl_word *entity, *to, *name, *value; /* entity, to: NULL when absent */
    odin3_loc at;
} queued_param;

typedef struct queued_rule {
    const odin3_tcl_word *entity, *to;
    const char *subject;
    odin3_loc at;
} queued_rule;

typedef struct qsf_state {
    odin3_project *proj;
    odin3_srcman *sm;
    odin3_arena *arena;      /* every word of every file, until the read ends */
    odin3_bytes revision;    /* in the arena or the strtab */
    odin3_bytes project_dir; /* the .qsf's directory: plain paths resolve against it */
    const odin3_tcl_word *top;
    odin3_loc top_at;
    bool has_device;
    size_t device; /* index of the DEVICE entry in proj->arch */
    odin3_vec params, rules, stack;
} qsf_state;

/* The command being run and its location (its first word's line). */
typedef struct cmd_ctx {
    const odin3_tcl_command *cmd;
    odin3_loc at;
    odin3_srcbuf_id buffer;
} cmd_ctx;

typedef struct qsf_err {
    odin3_proj_err_kind kind;
    const char *detail;
} qsf_err;

/* A located error at at (unlocated at 0) naming nnames names. */
static odin3_status fail_at(odin3_project *proj, odin3_loc at, qsf_err what,
                            const odin3_bytes *names, uint32_t nnames) {
    odin3_proj_scope scope = at.v != 0 ? ODIN3_PROJ_LOCATED : ODIN3_PROJ_UNLOCATED;
    odin3_proj_err err = {what.kind, scope, at, NULL, what.detail, names, nnames};
    return odin3_project_fail(proj, &err);
}

static odin3_status syntax_at(odin3_project *proj, odin3_loc at, const char *detail,
                              const odin3_tcl_word *word) {
    qsf_err what = {ODIN3_PROJ_ERR_SYNTAX, detail};
    return fail_at(proj, at, what, word != NULL ? &word->text : NULL, word != NULL);
}

/* --- files and frames ---------------------------------------------------------------------- */

static void free_frame(qsf_frame *frame) {
    odin3_proj_text_free(&frame->text);
    odin3_vec_free(&frame->commands);
}

/* Loads path (named at parent), tokenizes it whole and pushes it on the stack. */
static odin3_status open_frame(qsf_state *st, const odin3_proj_path *path, odin3_loc parent,
                               bool qip) {
    qsf_frame frame = {.path = *path, .next = 0};
    odin3_vec_init(&frame.commands, sizeof(odin3_tcl_command));
    odin3_status res = odin3_project_load(st->proj, path, parent, &frame.text);
    odin3_bytes resolved = str_bytes(st->proj, path->resolved);
    odin3_tcl_opts opts = {st->arena, qip, odin3_path_dir(resolved)};
    odin3_tcl_err err = {0, NULL};
    if (res == ODIN3_OK) {
        odin3_bytes text = {frame.text.bytes, frame.text.len};
        res = odin3_tcl_tokenize(text, &opts, &frame.commands, &err);
    }
    if (res == ODIN3_ERR_PARSE && err.what != NULL) {
        odin3_loc at = odin3_srcman_loc(st->sm, frame.text.buffer, err.off);
        res = fail_at(st->proj, at, (qsf_err){ODIN3_PROJ_ERR_SYNTAX, err.what}, NULL, 0);
    }
    qsf_frame *slot = res == ODIN3_OK ? odin3_vec_push(&st->stack) : NULL;
    if (slot == NULL) {
        free_frame(&frame);
        return res != ODIN3_OK ? res : ODIN3_ERR_NO_MEMORY;
    }
    *slot = frame;
    return ODIN3_OK;
}

static const qsf_frame *frame_at(const qsf_state *st, size_t idx) {
    return odin3_vec_cat(&st->stack, idx);
}

/* `qip_cycle` at the QIP_FILE: the .qip files from the one at idx to the innermost, then qip. */
static odin3_status qip_cycle(qsf_state *st, const cmd_ctx *cc, size_t idx,
                              const odin3_proj_path *qip) {
    size_t count = st->stack.len - idx + 1;
    odin3_bytes *names = odin3_arena_alloc(st->arena, count * sizeof *names);
    if (names == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t k = idx; k < st->stack.len; k++) {
        names[k - idx] = str_bytes(st->proj, frame_at(st, k)->path.resolved);
    }
    names[count - 1] = str_bytes(st->proj, qip->resolved);
    qsf_err what = {ODIN3_PROJ_ERR_QIP_CYCLE, "QIP_FILE cycle"};
    return fail_at(st->proj, cc->at, what, names, (uint32_t)count);
}

/* QIP_FILE: the .qip must exist, not be open already, and stay within the depth cap; it is read
 * now (its commands run before the next command of the file that names it). */
static odin3_status push_qip(qsf_state *st, const cmd_ctx *cc, const odin3_tcl_word *value) {
    odin3_proj_path qip;
    odin3_proj_path_req req = {st->project_dir, value->text, cc->at, ODIN3_PROJ_WANT_FILE};
    odin3_status res = odin3_project_path(st->proj, &req, &qip);
    for (size_t k = 1; res == ODIN3_OK && k < st->stack.len; k++) {
        if (frame_at(st, k)->path.resolved == qip.resolved) {
            return qip_cycle(st, cc, k, &qip);
        }
    }
    if (res == ODIN3_OK && st->stack.len - 1 >= st->proj->limits.qip_depth) {
        char detail[DETAIL_MAX];
        (void)snprintf(detail, sizeof detail, "nested .qip files exceed the depth limit of %u",
                       (unsigned)st->proj->limits.qip_depth);
        odin3_bytes name = str_bytes(st->proj, qip.resolved);
        return fail_at(st->proj, cc->at, (qsf_err){ODIN3_PROJ_ERR_SYNTAX, detail}, &name, 1);
    }
    odin3_loc parent = odin3_srcman_loc(st->sm, cc->buffer, value->off);
    return res != ODIN3_OK ? res : open_frame(st, &qip, parent, true);
}

/* --- options ------------------------------------------------------------------------------- */

typedef enum qsf_opt {
    OPT_NAME,
    OPT_TO,
    OPT_FROM,
    OPT_ENTITY,
    OPT_SECTION_ID,
    OPT_LIBRARY,
    OPT_HDL_VERSION,
    OPT_TAG,
    OPT_COMMENT,
    OPT_DISABLE,
    OPT_REMOVE,
    OPT_COUNT
} qsf_opt;

static const char *const k_options[OPT_COUNT] = {
    "-name",        "-to",  "-from",    "-entity",  "-section_id", "-library",
    "-hdl_version", "-tag", "-comment", "-disable", "-remove",
};

enum {
    ALL_OPTIONS = (1U << OPT_COUNT) - 1U,
    FLAG_OPTIONS = (1U << OPT_DISABLE) | (1U << OPT_REMOVE),
    FILE_OPTIONS = (1U << OPT_NAME) | (1U << OPT_LIBRARY) | (1U << OPT_HDL_VERSION),
    SINGLE_OPTIONS = 1U << OPT_NAME,
    RULE_OPTIONS = (1U << OPT_NAME) | (1U << OPT_TO) | (1U << OPT_ENTITY),
    PARAM_OPTIONS = RULE_OPTIONS,
};

/* A command's options (the last of each wins; flags point at themselves) and its one value. */
typedef struct qsf_opts {
    const odin3_tcl_word *opt[OPT_COUNT];
    const odin3_tcl_word *value;
} qsf_opts;

/* An option: '-' then a byte that is not an ASCII digit (`-1` is a value). */
static bool is_option(const odin3_tcl_word *word) {
    const uint8_t *text = word->text.ptr;
    return word->text.len > 1 && text[0] == '-' && !is_digit(text[1]);
}

static int option_index(const odin3_tcl_word *word) {
    for (int k = 0; k < OPT_COUNT; k++) {
        if (is_text(word->text, k_options[k])) {
            return k;
        }
    }
    return -1;
}

/* The oracle's qsf_options over the words after the command name. */
static odin3_status parse_options(qsf_state *st, const cmd_ctx *cc, unsigned allowed,
                                  qsf_opts *out) {
    *out = (qsf_opts){{NULL}, NULL};
    const odin3_tcl_word *words = cc->cmd->words;
    uint32_t nwords = cc->cmd->nwords;
    uint32_t values = 0;
    for (uint32_t j = 1; j < nwords; j++) {
        if (!is_option(&words[j])) {
            out->value = &words[j];
            values++;
            continue;
        }
        int opt = option_index(&words[j]);
        if (opt < 0 || (allowed & (1U << opt)) == 0) {
            qsf_err what = {ODIN3_PROJ_ERR_UNKNOWN_OPTION, "option not allowed here"};
            return fail_at(st->proj, cc->at, what, &words[j].text, 1);
        }
        if ((FLAG_OPTIONS & (1U << opt)) == 0) {
            if (j + 1 == nwords) {
                return syntax_at(st->proj, cc->at, "option needs a value", &words[j]);
            }
            j++;
        }
        out->opt[opt] = &words[j];
    }
    if (values != 1) {
        char detail[DETAIL_MAX];
        (void)snprintf(detail, sizeof detail, "expected one value, got %u", (unsigned)values);
        return syntax_at(st->proj, cc->at, detail, NULL);
    }
    return ODIN3_OK;
}

/* --- assignments --------------------------------------------------------------------------- */

typedef struct file_name {
    const char *name;
    odin3_lang lang;
} file_name;

static const file_name k_files[] = {
    {"VERILOG_FILE", ODIN3_LANG_VERILOG}, {"SYSTEMVERILOG_FILE", ODIN3_LANG_SYSTEMVERILOG},
    {"VHDL_FILE", ODIN3_LANG_VHDL},       {"VQM_FILE", ODIN3_LANG_VQM},
    {"EDIF_FILE", ODIN3_LANG_EDIF},
};

typedef enum single_name {
    SINGLE_TOP,
    SINGLE_SEARCH_PATH,
    SINGLE_USER_LIBRARIES,
    SINGLE_VERILOG_MACRO,
    SINGLE_FAMILY,
    SINGLE_DEVICE,
    SINGLE_QIP_FILE,
    SINGLE_COUNT
} single_name;

static const char *const k_singles[SINGLE_COUNT] = {
    "TOP_LEVEL_ENTITY", "SEARCH_PATH", "USER_LIBRARIES", "VERILOG_MACRO",
    "FAMILY",           "DEVICE",      "QIP_FILE",
};

/* A mapping assignment: the value that makes a soft rule on subject, and the default (none). */
typedef struct rule_name {
    const char *name;
    const char *rule_value;
    const char *subject;
    const char *default_value;
} rule_name;

static const rule_name k_rules[] = {
    {"AUTO_DSP_RECOGNITION", "OFF", "$mul", "ON"},
    {"AUTO_RAM_RECOGNITION", "OFF", "$mem", "ON"},
    {"DSP_BLOCK_BALANCING", "LOGIC ELEMENTS", "$mul", "AUTO"},
    {"MULTSTYLE", "LOGIC", "$mul", "AUTO"},
    {"RAMSTYLE", "LOGIC", "$mem", "AUTO"},
};

typedef enum name_kind { NAME_FILE, NAME_SINGLE, NAME_RULE, NAME_OTHER } name_kind;

/* What an (upper-cased) assignment name is: its kind and index in its table. */
typedef struct name_class {
    name_kind kind;
    size_t idx;
} name_class;

static name_class classify(odin3_bytes name) {
    for (size_t k = 0; k < sizeof k_files / sizeof k_files[0]; k++) {
        if (is_text(name, k_files[k].name)) {
            return (name_class){NAME_FILE, k};
        }
    }
    for (size_t k = 0; k < SINGLE_COUNT; k++) {
        if (is_text(name, k_singles[k])) {
            return (name_class){NAME_SINGLE, k};
        }
    }
    for (size_t k = 0; k < sizeof k_rules / sizeof k_rules[0]; k++) {
        if (is_text(name, k_rules[k].name)) {
            return (name_class){NAME_RULE, k};
        }
    }
    return (name_class){NAME_OTHER, 0};
}

/* An ASCII upper-cased copy of text in the arena ({NULL, 0} on out of memory, len kept 0). */
static odin3_status upper(qsf_state *st, odin3_bytes text, odin3_bytes *out) {
    char *copy = odin3_arena_alloc(st->arena, text.len + 1);
    if (copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const char *bytes = text.ptr;
    for (size_t i = 0; i < text.len; i++) {
        char byte = bytes[i];
        if (byte >= 'a' && byte <= 'z') {
            byte = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"[byte - 'a'];
        }
        copy[i] = byte;
    }
    *out = (odin3_bytes){copy, text.len};
    return ODIN3_OK;
}

static odin3_status design_file(qsf_state *st, const cmd_ctx *cc, odin3_lang lang,
                                const qsf_opts *opts) {
    odin3_proj_file file = {.lang = (uint8_t)lang};
    odin3_proj_path_req req = {st->project_dir, opts->value->text, cc->at, ODIN3_PROJ_WANT_FILE};
    odin3_status res = odin3_project_path(st->proj, &req, &file.path);
    const odin3_tcl_word *lib = opts->opt[OPT_LIBRARY];
    odin3_bytes library = lib != NULL ? lib->text : odin3_bytes_cstr("work");
    res = res != ODIN3_OK ? res : odin3_project_intern(st->proj, library, &file.library);
    res = res != ODIN3_OK ? res : odin3_project_add_file(st->proj, &file);
    if (res == ODIN3_OK && opts->opt[OPT_HDL_VERSION] != NULL) {
        res = odin3_project_add_ignored(st->proj, odin3_bytes_cstr("-hdl_version"));
    }
    return res;
}

/* SEARCH_PATH (include and library directories) or USER_LIBRARIES: ';'-separated, empty parts
 * dropped, each an existing directory. */
static odin3_status directories(qsf_state *st, const cmd_ctx *cc, odin3_bytes value, bool include) {
    const char *text = value.ptr;
    odin3_status res = ODIN3_OK;
    for (size_t from = 0, j = 0; res == ODIN3_OK && j <= value.len; j++) {
        if (j < value.len && text[j] != ';') {
            continue;
        }
        odin3_proj_path dir;
        odin3_proj_path_req req = {
            st->project_dir, {text + from, j - from}, cc->at, ODIN3_PROJ_WANT_DIR};
        if (j > from) {
            res = odin3_project_path(st->proj, &req, &dir);
            if (res == ODIN3_OK && include) {
                res = odin3_project_add_dir(st->proj, ODIN3_PROJ_INCDIRS, &dir);
            }
            res = res != ODIN3_OK ? res : odin3_project_add_dir(st->proj, ODIN3_PROJ_LIBDIRS, &dir);
        }
        from = j + 1;
    }
    return res;
}

/* FAMILY or DEVICE: one DEVICE arch entry, appended at the first of either, then updated. */
static odin3_status device(qsf_state *st, bool family, odin3_bytes value) {
    uint32_t str = 0;
    odin3_status res = odin3_project_intern(st->proj, value, &str);
    if (res == ODIN3_OK && !st->has_device) {
        odin3_proj_arch arch = {.kind = ODIN3_PROJ_ARCH_DEVICE};
        res = odin3_project_add_arch(st->proj, &arch);
        st->has_device = res == ODIN3_OK;
        st->device = st->proj->arch.len - 1;
    }
    if (res == ODIN3_OK) {
        odin3_proj_arch *arch = odin3_vec_at(&st->proj->arch, st->device);
        if (family) {
            arch->family = str;
            arch->has_family = true;
        } else {
            arch->device = str;
            arch->has_device = true;
        }
    }
    return res;
}

static odin3_status single(qsf_state *st, const cmd_ctx *cc, single_name name,
                           const qsf_opts *opts) {
    const odin3_tcl_word *value = opts->value;
    switch (name) {
    case SINGLE_TOP:
        st->top = value; /* the last one wins (Quartus) */
        st->top_at = cc->at;
        return ODIN3_OK;
    case SINGLE_SEARCH_PATH:
    case SINGLE_USER_LIBRARIES:
        return directories(st, cc, value->text, name == SINGLE_SEARCH_PATH);
    case SINGLE_VERILOG_MACRO:
        return odin3_project_add_define(st->proj, value->text, cc->at);
    case SINGLE_FAMILY:
    case SINGLE_DEVICE:
        return device(st, name == SINGLE_FAMILY, value->text);
    default:
        return push_qip(st, cc, value);
    }
}

/* A mapping assignment: its rule value queues a soft rule, its default makes none, any other value
 * is ignored and listed under the name. */
static odin3_status mapping(qsf_state *st, const cmd_ctx *cc, const rule_name *rule,
                            const qsf_opts *opts) {
    odin3_bytes value = {NULL, 0};
    odin3_status res = upper(st, opts->value->text, &value);
    if (res != ODIN3_OK || is_text(value, rule->default_value)) {
        return res;
    }
    if (!is_text(value, rule->rule_value)) {
        return odin3_project_add_ignored(st->proj, odin3_bytes_cstr(rule->name));
    }
    queued_rule *slot = odin3_vec_push(&st->rules);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (queued_rule){opts->opt[OPT_ENTITY], opts->opt[OPT_TO], rule->subject, cc->at};
    return ODIN3_OK;
}

/* The one `-name N` of an assignment (scanned like the oracle: every "-name" but the last word). */
static odin3_status assignment_name(qsf_state *st, const cmd_ctx *cc, odin3_bytes *name) {
    const odin3_tcl_word *words = cc->cmd->words;
    uint32_t count = 0;
    for (uint32_t j = 1; j + 1 < cc->cmd->nwords; j++) {
        if (is_text(words[j].text, "-name")) {
            *name = words[j + 1].text;
            count++;
        }
    }
    if (count != 1) {
        return syntax_at(st->proj, cc->at, "an assignment needs one -name", NULL);
    }
    return upper(st, *name, name);
}

static odin3_status assignment(qsf_state *st, const cmd_ctx *cc) {
    static const unsigned allowed[] = {FILE_OPTIONS, SINGLE_OPTIONS, RULE_OPTIONS, ALL_OPTIONS};
    odin3_bytes name = {NULL, 0};
    odin3_status res = assignment_name(st, cc, &name);
    name_class what = classify(name);
    qsf_opts opts;
    res = res != ODIN3_OK ? res : parse_options(st, cc, allowed[what.kind], &opts);
    if (res != ODIN3_OK) {
        return res;
    }
    switch (what.kind) {
    case NAME_FILE:
        return design_file(st, cc, k_files[what.idx].lang, &opts);
    case NAME_SINGLE:
        return single(st, cc, (single_name)what.idx, &opts);
    case NAME_RULE:
        return mapping(st, cc, &k_rules[what.idx], &opts);
    default:
        return odin3_project_add_ignored(st->proj, name);
    }
}

static odin3_status set_parameter(qsf_state *st, const cmd_ctx *cc) {
    qsf_opts opts;
    odin3_status res = parse_options(st, cc, PARAM_OPTIONS, &opts);
    if (res != ODIN3_OK) {
        return res;
    }
    if (opts.opt[OPT_NAME] == NULL) {
        return syntax_at(st->proj, cc->at, "set_parameter needs -name", NULL);
    }
    queued_param *slot = odin3_vec_push(&st->params);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (queued_param){opts.opt[OPT_ENTITY], opts.opt[OPT_TO], opts.opt[OPT_NAME], opts.value,
                           cc->at};
    return ODIN3_OK;
}

static odin3_status command(qsf_state *st, const cmd_ctx *cc) {
    odin3_bytes name = cc->cmd->words[0].text;
    if (is_text(name, "set_global_assignment") || is_text(name, "set_instance_assignment") ||
        is_text(name, "set_io_assignment")) {
        return assignment(st, cc);
    }
    if (is_text(name, "set_location_assignment")) {
        qsf_opts opts;
        odin3_status res = parse_options(st, cc, ALL_OPTIONS, &opts);
        return res != ODIN3_OK ? res
                               : odin3_project_add_ignored(st->proj, odin3_bytes_cstr("LOCATION"));
    }
    if (is_text(name, "set_parameter")) {
        return set_parameter(st, cc);
    }
    return odin3_project_add_ignored(st->proj, name);
}

/* Runs the stack's commands in order; a QIP_FILE pushes its .qip, popped when it is done. */
static odin3_status run_frames(qsf_state *st) {
    odin3_status res = ODIN3_OK;
    while (res == ODIN3_OK && st->stack.len > 0) {
        qsf_frame *top = odin3_vec_at(&st->stack, st->stack.len - 1);
        if (top->next == top->commands.len) {
            free_frame(top);
            odin3_vec_pop(&st->stack);
            continue;
        }
        const odin3_tcl_command *cmd = odin3_vec_cat(&top->commands, top->next++);
        cmd_ctx cc = {cmd, odin3_srcman_loc(st->sm, top->text.buffer, cmd->off), top->text.buffer};
        res = command(st, &cc); /* may push a frame: top is stale after it */
    }
    return res;
}

/* --- after the file: top, parameters, rules ------------------------------------------------ */

/* A word's text when present and non-empty, else fallback (the oracle's `x or y`). */
static odin3_bytes text_or(const odin3_tcl_word *word, odin3_bytes fallback) {
    return word != NULL && word->text.len > 0 ? word->text : fallback;
}

/* The next non-empty '|' part of to at or after *pos, as its component after the last ':'. */
static bool next_component(odin3_bytes to, size_t *pos, odin3_bytes *out) {
    const char *text = to.ptr;
    while (*pos < to.len) {
        size_t start = *pos;
        size_t end = start;
        while (end < to.len && text[end] != '|') {
            end++;
        }
        *pos = end + 1;
        if (end > start) {
            *out = after_last((odin3_bytes){text + start, end - start}, ':');
            return true;
        }
    }
    return false;
}

/* The oracle's anchored(): `*` or the entity without -to; else the -to instance path anchored at
 * the entity or the top's module name (unless it already starts there), joined with '.'. */
typedef struct scope_req {
    const odin3_tcl_word *entity, *to;
    odin3_bytes top_mod;
} scope_req;

static odin3_status anchored(const scope_req *req, odin3_strbuf *out) {
    odin3_strbuf_clear(out);
    if (req->to == NULL) {
        return odin3_strbuf_append(out, text_or(req->entity, odin3_bytes_cstr("*")));
    }
    odin3_bytes anchor = text_or(req->entity, req->top_mod);
    odin3_bytes part = {NULL, 0};
    size_t pos = 0;
    bool prefix = !next_component(req->to->text, &pos, &part) || !bytes_eq(part, anchor);
    odin3_status res = prefix ? odin3_strbuf_append(out, anchor) : ODIN3_OK;
    pos = 0;
    for (size_t count = prefix; res == ODIN3_OK && next_component(req->to->text, &pos, &part);
         count++) {
        res = count > 0 ? odin3_strbuf_append(out, odin3_bytes_cstr(".")) : ODIN3_OK;
        res = res != ODIN3_OK ? res : odin3_strbuf_append(out, part);
    }
    return res;
}

static odin3_status add_param(qsf_state *st, const queued_param *param, odin3_bytes top_mod,
                              odin3_strbuf *scope) {
    if (param->entity != NULL && !bytes_eq(param->entity->text, top_mod)) {
        return syntax_at(st->proj, param->at, "set_parameter -entity must name the top",
                         param->entity);
    }
    odin3_proj_param out = {.type = ODIN3_PARAM_STRING, .at = param->at};
    odin3_bytes value = param->value->text;
    const char *text = value.ptr;
    if (is_number(value)) {
        out.type = ODIN3_PARAM_NUMBER;
    } else if (value.len >= 2 && text[0] == '"' && text[value.len - 1] == '"') {
        value = (odin3_bytes){text + 1, value.len - 2};
    }
    odin3_status res = ODIN3_OK;
    if (param->to != NULL && param->to->text.len > 0) { /* `-to ""` counts as absent */
        scope_req req = {param->entity, param->to, top_mod};
        res = anchored(&req, scope);
    } else {
        odin3_strbuf_clear(scope);
        res = odin3_strbuf_append(scope, top_mod);
    }
    res = res != ODIN3_OK
              ? res
              : odin3_project_intern(st->proj, (odin3_bytes){scope->data, scope->len}, &out.scope);
    res = res != ODIN3_OK ? res : odin3_project_intern(st->proj, param->name->text, &out.name);
    res = res != ODIN3_OK ? res : odin3_project_intern(st->proj, value, &out.value);
    return res != ODIN3_OK ? res : odin3_project_add_param(st->proj, &out);
}

static odin3_status add_rule(qsf_state *st, const queued_rule *rule, odin3_bytes top_mod,
                             odin3_strbuf *scope) {
    odin3_proj_rule out = {.kind = ODIN3_PROJ_RULE_MAP, .at = rule->at};
    out.u.map.action = ODIN3_PROJ_ACTION_SOFT;
    scope_req req = {rule->entity, rule->to, top_mod};
    odin3_status res = anchored(&req, scope);
    res = res != ODIN3_OK ? res
                          : odin3_project_intern(st->proj, (odin3_bytes){scope->data, scope->len},
                                                 &out.u.map.scope);
    res = res != ODIN3_OK
              ? res
              : odin3_project_intern(st->proj, odin3_bytes_cstr(rule->subject), &out.u.map.subject);
    return res != ODIN3_OK ? res
                           : odin3_project_add_rule(st->proj, &out, (odin3_proj_ids){NULL, 0});
}

/* The top (TOP_LEVEL_ENTITY, else the revision at the .qsf as a whole), then the queued
 * parameters and rules, which need it. */
static odin3_status finish_qsf(qsf_state *st, const odin3_proj_path *qsf) {
    odin3_bytes top = text_or(st->top, st->revision);
    odin3_status res = st->top != NULL ? odin3_project_set_top(st->proj, top, st->top_at)
                                       : odin3_project_set_top_file(st->proj, top, qsf);
    odin3_bytes top_mod = after_last(top, '.');
    odin3_strbuf scope;
    odin3_strbuf_init(&scope);
    for (size_t k = 0; res == ODIN3_OK && k < st->params.len; k++) {
        res = add_param(st, odin3_vec_cat(&st->params, k), top_mod, &scope);
    }
    for (size_t k = 0; res == ODIN3_OK && k < st->rules.len; k++) {
        res = add_rule(st, odin3_vec_cat(&st->rules, k), top_mod, &scope);
    }
    odin3_strbuf_free(&scope);
    return res;
}

/* --- .qpf and the revision ----------------------------------------------------------------- */

static size_t skip_spaces(odin3_bytes line, size_t pos) {
    size_t len = odin3_py_space_len(line, pos);
    while (len > 0) {
        pos += len;
        len = odin3_py_space_len(line, pos);
    }
    return pos;
}

typedef enum qpf_kind { QPF_SKIP, QPF_ASSIGN, QPF_BAD } qpf_kind;

/* A .qpf line: blank or '#' (skipped), else the whole-line match of
 * \s*([A-Z_]+)\s*=\s*"([^"]*)"\s* with its name offset (in the line) and value. */
typedef struct qpf_line {
    odin3_bytes name, value;
    size_t name_at;
} qpf_line;

static qpf_kind parse_qpf_line(odin3_bytes line, qpf_line *out) {
    const char *text = line.ptr;
    size_t j = skip_spaces(line, 0);
    if (j == line.len || text[j] == '#') {
        return QPF_SKIP;
    }
    size_t start = j;
    while (j < line.len && ((text[j] >= 'A' && text[j] <= 'Z') || text[j] == '_')) {
        j++;
    }
    out->name = (odin3_bytes){text + start, j - start};
    out->name_at = start;
    j = skip_spaces(line, j);
    if (j == start || j >= line.len || text[j] != '=') {
        return QPF_BAD;
    }
    j = skip_spaces(line, j + 1);
    if (j >= line.len || text[j] != '"') {
        return QPF_BAD;
    }
    const char *close = memchr(text + j + 1, '"', line.len - j - 1);
    if (close == NULL) {
        return QPF_BAD;
    }
    out->value = (odin3_bytes){text + j + 1, (size_t)(close - (text + j + 1))};
    return skip_spaces(line, (size_t)(close - text) + 1) == line.len ? QPF_ASSIGN : QPF_BAD;
}

/* A revision named in a .qpf: its value (in the arena) and the loc of its line. */
typedef struct qpf_revision {
    odin3_bytes name;
    odin3_loc at;
} qpf_revision;

static odin3_status add_revision(qsf_state *st, const qpf_line *line, odin3_loc at,
                                 odin3_vec *revs) {
    char *copy = odin3_arena_strndup(st->arena, line->value.ptr, line->value.len);
    qpf_revision *slot = copy != NULL ? odin3_vec_push(revs) : NULL;
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (qpf_revision){{copy, line->value.len}, at};
    return ODIN3_OK;
}

/* Every PROJECT_REVISION of the .qpf text, in order; any other line must be NAME = "value". */
static odin3_status qpf_revisions(qsf_state *st, const odin3_proj_text *text, odin3_vec *revs) {
    for (uint32_t k = 0; k < text->lines.len; k++) {
        uint32_t off = 0;
        qpf_line line = {{NULL, 0}, {NULL, 0}, 0};
        odin3_bytes bytes = odin3_proj_text_line(text, k, &off);
        qpf_kind kind = parse_qpf_line(bytes, &line);
        if (kind == QPF_BAD) {
            odin3_loc at = odin3_srcman_loc(st->sm, text->buffer, off);
            return syntax_at(st->proj, at, "expected NAME = \"value\"", NULL);
        }
        if (kind == QPF_ASSIGN && is_text(line.name, "PROJECT_REVISION")) {
            odin3_loc at = odin3_srcman_loc(st->sm, text->buffer, off + (uint32_t)line.name_at);
            odin3_status res = add_revision(st, &line, at, revs);
            if (res != ODIN3_OK) {
                return res;
            }
        }
    }
    return ODIN3_OK;
}

static odin3_status unknown_revision(qsf_state *st, const odin3_quartus_req *req) {
    qsf_err what = {ODIN3_PROJ_ERR_UNKNOWN_REVISION, "no such revision"};
    return fail_at(st->proj, req->at, what, &req->revision, 1);
}

/* The revision: the wanted one when listed, else the only one (several: ambiguous_revision). */
static odin3_status choose_revision(qsf_state *st, const odin3_quartus_req *req,
                                    const odin3_vec *revs) {
    const qpf_revision *list = revs->data;
    if (req->has_revision) {
        bool listed = revs->len == 0 && bytes_eq(req->revision, st->revision);
        for (size_t k = 0; k < revs->len && !listed; k++) {
            listed = bytes_eq(req->revision, list[k].name);
        }
        st->revision = req->revision;
        return listed ? ODIN3_OK : unknown_revision(st, req);
    }
    if (revs->len > 1) {
        odin3_bytes *names = odin3_arena_alloc(st->arena, revs->len * sizeof *names);
        if (names == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        for (size_t k = 0; k < revs->len; k++) {
            names[k] = list[k].name;
        }
        qsf_err what = {ODIN3_PROJ_ERR_AMBIGUOUS_REVISION,
                        "several revisions; choose one with --revision"};
        return fail_at(st->proj, list[0].at, what, names, (uint32_t)revs->len);
    }
    st->revision = revs->len == 1 ? list[0].name : st->revision;
    return ODIN3_OK;
}

/* The name of a resolved path and its Python Path.stem / Path.suffix split (dot = len: none). */
typedef struct path_name {
    odin3_bytes name;
    size_t dot;
} path_name;

static path_name name_of(odin3_bytes path) {
    path_name out = {after_last(path, '/'), 0};
    const char *text = out.name.ptr;
    size_t dot = out.name.len;
    while (dot > 0 && text[dot - 1] != '.') {
        dot--;
    }
    bool suffix = dot > 1 && dot < out.name.len; /* the '.' at dot - 1: not first, not last */
    out.dot = suffix ? dot - 1 : out.name.len;
    return out;
}

/* The chosen revision's <revision>.qsf beside the .qpf: it must exist (else file-level
 * missing_file at the .qpf); given = the .qpf's given directory joined with the name. */
static odin3_status revision_qsf(qsf_state *st, const odin3_proj_path *qpf, odin3_proj_path *qsf) {
    odin3_strbuf name;
    odin3_strbuf path;
    odin3_strbuf_init(&name);
    odin3_strbuf_init(&path);
    odin3_bytes given_dir = odin3_path_dir(str_bytes(st->proj, qpf->given));
    const char *dir_text = given_dir.ptr;
    odin3_status res = odin3_strbuf_append(&name, given_dir);
    if (res == ODIN3_OK && given_dir.len > 0 && dir_text[given_dir.len - 1] != '/') {
        res = odin3_strbuf_append(&name, odin3_bytes_cstr("/"));
    }
    size_t base = name.len;
    res = res != ODIN3_OK ? res : odin3_strbuf_append(&name, st->revision);
    res = res != ODIN3_OK ? res : odin3_strbuf_append(&name, odin3_bytes_cstr(".qsf"));
    odin3_bytes leaf = {name.data + base, name.len - base};
    odin3_path_join join = {odin3_path_dir(str_bytes(st->proj, qpf->resolved)), leaf};
    res = res != ODIN3_OK ? res : odin3_path_resolve(join, &path);
    if (res == ODIN3_OK && odin3_path_stat(path.data) != ODIN3_PATH_FILE) {
        odin3_bytes missing = {path.data, path.len};
        odin3_proj_err err = {
            ODIN3_PROJ_ERR_MISSING_FILE, ODIN3_PROJ_FILE, {0}, qpf, "file not found", &missing, 1};
        res = odin3_project_fail(st->proj, &err);
    }
    *qsf = (odin3_proj_path){0, 0, {0}};
    res = res != ODIN3_OK
              ? res
              : odin3_project_intern(st->proj, (odin3_bytes){name.data, name.len}, &qsf->given);
    res = res != ODIN3_OK
              ? res
              : odin3_project_intern(st->proj, (odin3_bytes){path.data, path.len}, &qsf->resolved);
    odin3_strbuf_free(&name);
    odin3_strbuf_free(&path);
    return res;
}

static odin3_status from_qpf(qsf_state *st, const odin3_proj_path *qpf,
                             const odin3_quartus_req *req, odin3_proj_path *qsf) {
    odin3_proj_text text;
    odin3_vec revs;
    odin3_vec_init(&revs, sizeof(qpf_revision));
    odin3_status res = odin3_project_load(st->proj, qpf, req->at, &text);
    res = res != ODIN3_OK ? res : qpf_revisions(st, &text, &revs);
    res = res != ODIN3_OK ? res : choose_revision(st, req, &revs);
    res = res != ODIN3_OK ? res : revision_qsf(st, qpf, qsf);
    odin3_vec_free(&revs);
    odin3_proj_text_free(&text);
    return res;
}

/* A .qpf names the .qsf through its revision; any other entry is the .qsf (revision = its stem). */
static odin3_status choose_qsf(qsf_state *st, const odin3_proj_path *file,
                               const odin3_quartus_req *req, odin3_proj_path *qsf) {
    path_name name = name_of(str_bytes(st->proj, file->resolved));
    const char *text = name.name.ptr;
    st->revision = (odin3_bytes){text, name.dot};
    odin3_bytes suffix = {text + name.dot, name.name.len - name.dot};
    if (is_text(suffix, ".qpf")) {
        return from_qpf(st, file, req, qsf);
    }
    *qsf = *file;
    if (req->has_revision && !bytes_eq(req->revision, st->revision)) {
        return unknown_revision(st, req);
    }
    return ODIN3_OK;
}

static void free_state(qsf_state *st) {
    for (size_t k = 0; k < st->stack.len; k++) {
        free_frame(odin3_vec_at(&st->stack, k));
    }
    odin3_vec_free(&st->stack);
    odin3_vec_free(&st->params);
    odin3_vec_free(&st->rules);
    odin3_arena_destroy(st->arena);
}

odin3_status odin3_read_quartus(odin3_project *proj, const odin3_proj_path *file,
                                const odin3_quartus_req *req) {
    qsf_state st = {.proj = proj};
    odin3_vec_init(&st.params, sizeof(queued_param));
    odin3_vec_init(&st.rules, sizeof(queued_rule));
    odin3_vec_init(&st.stack, sizeof(qsf_frame));
    st.arena = odin3_arena_create(0);
    odin3_status res =
        st.arena != NULL ? odin3_design_get_srcman(proj->design, &st.sm) : ODIN3_ERR_NO_MEMORY;
    odin3_proj_path qsf = {0, 0, {0}};
    res = res != ODIN3_OK ? res : choose_qsf(&st, file, req, &qsf);
    st.project_dir = odin3_path_dir(str_bytes(proj, qsf.resolved));
    res = res != ODIN3_OK ? res : open_frame(&st, &qsf, req->at, false);
    res = res != ODIN3_OK ? res : run_frames(&st);
    res = res != ODIN3_OK ? res : finish_qsf(&st, &qsf);
    free_state(&st);
    return res;
}
