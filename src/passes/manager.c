/* manager.c — the pass registry, pass runs (provenance run, check, timing) and pass scripts. */
#include "passes/manager.h"

#include "passes/builtin.h"

#include "ir/check.h"
#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Debug builds check around every pass whatever the options say (CLAUDE.md rule 3). */
#ifdef NDEBUG
enum { ALWAYS_CHECK = 0 };
#else
enum { ALWAYS_CHECK = 1 };
#endif

/* A location is part of one log message, so it never needs more than a message's bytes. */
enum { MS_PER_S = 1000, NS_PER_MS = 1000000, LOC_BUF = ODIN3_LOG_BUF, READ_CHUNK = 4096 };

/* Lowest and highest characters of a pass name (printable ASCII, no blank). */
enum { NAME_CHAR_MIN = '!', NAME_CHAR_MAX = '~' };

/* A registered (plugin) pass. */
typedef struct pass_slot {
    const odin3_pass_def *def;
} pass_slot;

static odin3_vec g_passes; /* pass_slot */
static bool g_passes_ready = false;
static odin3_pass_options g_options;

/* --- registry ------------------------------------------------------------------------------- */

uint32_t odin3_pass_count(void) {
    return odin3_builtin_pass_count + (uint32_t)g_passes.len;
}

const odin3_pass_def *odin3_pass_at(uint32_t index) {
    if (index < odin3_builtin_pass_count) {
        return odin3_builtin_passes[index];
    }
    if (index >= odin3_pass_count()) {
        return NULL;
    }
    const pass_slot *slot = odin3_vec_cat(&g_passes, index - odin3_builtin_pass_count);
    return slot->def;
}

bool odin3_pass_arg_is(odin3_bytes word, const char *text) {
    size_t len = strlen(text);
    return word.len == len && (len == 0 || memcmp(word.ptr, text, len) == 0);
}

const odin3_pass_def *odin3_pass_find(odin3_bytes name) {
    for (uint32_t i = 0; i < odin3_pass_count(); i++) {
        const odin3_pass_def *def = odin3_pass_at(i);
        if (odin3_pass_arg_is(name, def->name)) {
            return def;
        }
    }
    return NULL;
}

static bool name_ok(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (const char *at = name; *at != '\0'; at++) {
        if (*at < NAME_CHAR_MIN || *at > NAME_CHAR_MAX || *at == ';' || *at == '#') {
            return false;
        }
    }
    return true;
}

odin3_status odin3_pass_register_def(const odin3_pass_def *def) {
    if (def == NULL || def->run == NULL || def->help == NULL || !name_ok(def->name)) {
        odin3_log(ODIN3_LOG_ERROR, "pass_register: NULL definition, run or help, or a bad name");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_pass_find(odin3_bytes_cstr(def->name)) != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "pass_register: pass '%s' already registered", def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!g_passes_ready) {
        odin3_vec_init(&g_passes, sizeof(pass_slot));
        g_passes_ready = true;
    }
    pass_slot *slot = odin3_vec_push(&g_passes);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->def = def;
    return ODIN3_OK;
}

void odin3_pass_set_options(odin3_pass_options opts) {
    g_options = opts;
}

odin3_pass_options odin3_pass_get_options(void) {
    return g_options;
}

/* --- pass runs ------------------------------------------------------------------------------ */

static double now_ms(void) {
    struct timespec ts = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * MS_PER_S + (double)ts.tv_nsec / NS_PER_MS;
}

/*
 * The design check before or after a pass. The check before reports errors only (its warnings are
 * counted, not delivered): the check after the previous pass already showed them, so a warning is
 * printed once per pass, not twice. The caller's log level is restored afterwards.
 */
static odin3_status check_around(odin3_design *design, const char *pass, bool before) {
    if (!ALWAYS_CHECK && !g_options.check) {
        return ODIN3_OK;
    }
    odin3_log_level level = odin3_log_get_level();
    if (before && level > ODIN3_LOG_ERROR) {
        (void)odin3_log_set_level(ODIN3_LOG_ERROR);
    }
    odin3_status st =
        odin3_check_design(design, (odin3_check_opts){ODIN3_CHECK_FULL, ODIN3_VIEW_NONE});
    (void)odin3_log_set_level(level);
    if (st == ODIN3_ERR_CHECK) {
        odin3_log(ODIN3_LOG_ERROR, "pass %s: check %s the pass failed", pass,
                  before ? "before" : "after");
    }
    return st;
}

static odin3_status open_run(odin3_design *design, const char *name, odin3_pass_ctx *ctx) {
    uint32_t name_str = 0;
    odin3_status st = odin3_design_intern(design, odin3_bytes_cstr(name), &name_str);
    if (st == ODIN3_OK) {
        st = odin3_pass_run_begin(design, name_str, ctx);
    }
    return st;
}

/* The pass and the check after it; a failing pass keeps its status, the check still runs. */
static odin3_status run_checked(odin3_pass_ctx *ctx, const odin3_pass_def *def, odin3_bytes args) {
    odin3_status st = def->run(ctx, ctx->design, args, def->user);
    if (st != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "pass %s: failed: %s", def->name, odin3_status_string(st));
    }
    odin3_status post = check_around(ctx->design, def->name, false);
    return st != ODIN3_OK ? st : post;
}

static odin3_status run_def(odin3_design *design, const odin3_pass_def *def, odin3_bytes args) {
    double start = now_ms();
    odin3_pass_ctx ctx = {0};
    odin3_status st = open_run(design, def->name, &ctx);
    if (st != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "pass %s: cannot open its provenance run: %s", def->name,
                  odin3_status_string(st));
        return st;
    }
    st = check_around(design, def->name, true);
    if (st == ODIN3_OK) {
        st = run_checked(&ctx, def, args);
    }
    odin3_log(ODIN3_LOG_INFO, "pass %s: %.3f ms", def->name, now_ms() - start);
    return st;
}

/* True when every double quote in args is closed. */
static bool quotes_closed(odin3_bytes args) {
    bool open = false;
    for (size_t i = 0; i < args.len; i++) {
        open = ((const char *)args.ptr)[i] == '"' ? !open : open;
    }
    return !open;
}

odin3_status odin3_pass_run(odin3_design *design, const char *name, odin3_bytes args) {
    if (design == NULL || name == NULL || (args.ptr == NULL && args.len > 0)) {
        odin3_log(ODIN3_LOG_ERROR, "pass_run: NULL design, pass name or arguments");
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_pass_def *def = odin3_pass_find(odin3_bytes_cstr(name));
    if (def == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "unknown pass '%s'", name);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!quotes_closed(args)) {
        odin3_log(ODIN3_LOG_ERROR, "pass %s: unterminated quote in the arguments", name);
        return ODIN3_ERR_INVALID_ARG;
    }
    return run_def(design, def, args);
}

/* --- arguments ------------------------------------------------------------------------------ */

static bool is_blank(char chr) {
    return chr == ' ' || chr == '\t' || chr == '\r' || chr == '\n';
}

/* bytes without leading and trailing blanks. */
static odin3_bytes trim(odin3_bytes bytes) {
    const char *at = bytes.ptr;
    size_t len = bytes.len;
    while (len > 0 && is_blank(at[0])) {
        at++;
        len--;
    }
    while (len > 0 && is_blank(at[len - 1])) {
        len--;
    }
    return (odin3_bytes){len > 0 ? at : NULL, len};
}

odin3_bytes odin3_pass_arg_next(odin3_bytes *rest) {
    odin3_bytes left = trim(*rest);
    const char *at = left.ptr;
    if (left.len == 0) {
        *rest = (odin3_bytes){0};
        return (odin3_bytes){0};
    }
    bool quoted = at[0] == '"';
    size_t start = quoted ? 1 : 0;
    size_t end = start;
    while (end < left.len && (quoted ? at[end] != '"' : !is_blank(at[end]) && at[end] != '"')) {
        end++;
    }
    size_t next = quoted && end < left.len ? end + 1 : end; /* past the closing quote */
    *rest = (odin3_bytes){next < left.len ? at + next : NULL, left.len - next};
    return (odin3_bytes){at + start, end - start};
}

/* --- scripts -------------------------------------------------------------------------------- */

/* One command of a script: the pass (found before anything runs) and where it was written. */
typedef struct script_cmd {
    odin3_bytes name;
    odin3_bytes args;
    const odin3_pass_def *def;
    uint32_t line;  /* 1-based line of the command */
    uint32_t index; /* 1-based command number */
} script_cmd;

/* The command's location: "origin:line" or "origin: command k". */
static void format_loc(const odin3_script_src *src, const script_cmd *cmd, char *buf, size_t size) {
    if (src->loc == ODIN3_SCRIPT_BY_LINE) {
        (void)snprintf(buf, size, "%s:%u", src->origin, cmd->line);
    } else {
        (void)snprintf(buf, size, "%s: command %u", src->origin, cmd->index);
    }
}

/* Appends the command text (no separator, comment removed) unless it is blank. */
static odin3_status add_cmd(odin3_vec *cmds, odin3_bytes text, uint32_t line) {
    odin3_bytes rest = trim(text);
    if (rest.len == 0) {
        return ODIN3_OK;
    }
    script_cmd *cmd = odin3_vec_push(cmds);
    if (cmd == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *cmd = (script_cmd){.line = line, .index = (uint32_t)cmds->len};
    cmd->name = odin3_pass_arg_next(&rest);
    cmd->args = trim(rest);
    return ODIN3_OK;
}

/*
 * Index of the first ';', '\n' or '#' at or after pos outside double quotes (text.len when none);
 * *open is set when a quote is still open at the newline or the end.
 */
static size_t segment_end(odin3_bytes text, size_t pos, bool *open) {
    const char *chars = text.ptr;
    bool quoted = false;
    for (; pos < text.len && chars[pos] != '\n'; pos++) {
        if (chars[pos] == '"') {
            quoted = !quoted;
        } else if (!quoted && (chars[pos] == ';' || chars[pos] == '#')) {
            break;
        }
    }
    *open = quoted;
    return pos;
}

/* Index of the newline ending the comment that starts at pos (text.len when none). */
static size_t comment_end(odin3_bytes text, size_t pos) {
    const char *chars = text.ptr;
    while (pos < text.len && chars[pos] != '\n') {
        pos++;
    }
    return pos;
}

static odin3_status unterminated(const odin3_script_src *src, const odin3_vec *cmds,
                                 uint32_t line) {
    script_cmd cmd = {.line = line, .index = (uint32_t)cmds->len + 1};
    char loc[LOC_BUF];
    format_loc(src, &cmd, loc, sizeof loc);
    odin3_log(ODIN3_LOG_ERROR, "%s: unterminated quote", loc);
    return ODIN3_ERR_PARSE;
}

/* Splits text into commands (see odin3_pass_run_script). */
static odin3_status split_script(odin3_bytes text, const odin3_script_src *src, odin3_vec *cmds) {
    const char *chars = text.ptr;
    uint32_t line = 1;
    size_t pos = 0;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && pos < text.len) {
        bool open = false;
        size_t end = segment_end(text, pos, &open);
        st = open ? unterminated(src, cmds, line)
                  : add_cmd(cmds, (odin3_bytes){chars + pos, end - pos}, line);
        end = end < text.len && chars[end] == '#' ? comment_end(text, end) : end;
        line += end < text.len && chars[end] == '\n' ? 1 : 0;
        pos = end + 1;
    }
    return st;
}

/* Finds every command's pass; logs each unknown name. ODIN3_ERR_PARSE when any is unknown. */
static odin3_status find_passes(odin3_vec *cmds, const odin3_script_src *src) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; i < cmds->len; i++) {
        script_cmd *cmd = odin3_vec_at(cmds, i);
        cmd->def = odin3_pass_find(cmd->name);
        if (cmd->def == NULL) {
            char loc[LOC_BUF];
            format_loc(src, cmd, loc, sizeof loc);
            odin3_log(ODIN3_LOG_ERROR, "%s: unknown pass '%.*s'", loc, (int)cmd->name.len,
                      (const char *)cmd->name.ptr);
            st = ODIN3_ERR_PARSE;
        }
    }
    return st;
}

static odin3_status run_cmds(odin3_design *design, const odin3_vec *cmds,
                             const odin3_script_src *src) {
    for (size_t i = 0; i < cmds->len; i++) {
        const script_cmd *cmd = odin3_vec_cat(cmds, i);
        odin3_status st = run_def(design, cmd->def, cmd->args);
        if (st != ODIN3_OK) {
            char loc[LOC_BUF];
            format_loc(src, cmd, loc, sizeof loc);
            odin3_log(ODIN3_LOG_ERROR, "%s: pass '%s' failed: %s", loc, cmd->def->name,
                      odin3_status_string(st));
            return st;
        }
    }
    return ODIN3_OK;
}

/* Splits and resolves the script, then runs it on design (NULL: resolve only). */
static odin3_status script_do(odin3_design *design, odin3_bytes text, const odin3_script_src *src) {
    if (src->origin == NULL || (text.ptr == NULL && text.len > 0)) {
        odin3_log(ODIN3_LOG_ERROR, "pass script: NULL origin or text");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_vec cmds;
    odin3_vec_init(&cmds, sizeof(script_cmd));
    odin3_status st = split_script(text, src, &cmds);
    if (st == ODIN3_OK) {
        st = find_passes(&cmds, src);
    }
    if (st == ODIN3_OK && design != NULL) {
        st = run_cmds(design, &cmds, src);
    }
    odin3_vec_free(&cmds);
    return st;
}

odin3_status odin3_pass_run_script(odin3_design *design, odin3_bytes text, odin3_script_src src) {
    if (design == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "pass_run_script: NULL design");
        return ODIN3_ERR_INVALID_ARG;
    }
    return script_do(design, text, &src);
}

odin3_status odin3_pass_resolve_script(odin3_bytes text, odin3_script_src src) {
    return script_do(NULL, text, &src);
}

/* Reads the whole file at path into buf. */
static odin3_status read_all(const char *path, odin3_strbuf *buf) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "%s: cannot read the script", path);
        return ODIN3_ERR_IO;
    }
    char chunk[READ_CHUNK];
    odin3_status st = ODIN3_OK;
    size_t got = 0;
    while (st == ODIN3_OK && (got = fread(chunk, 1, sizeof chunk, file)) > 0) {
        st = odin3_strbuf_append(buf, (odin3_bytes){chunk, got});
    }
    if (st == ODIN3_OK && ferror(file)) {
        odin3_log(ODIN3_LOG_ERROR, "%s: cannot read the script", path);
        st = ODIN3_ERR_IO;
    }
    (void)fclose(file);
    return st;
}

/* Reads the script file at path, then script_do on it. */
static odin3_status script_file_do(odin3_design *design, const char *path) {
    if (path == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "pass script file: NULL path");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    odin3_status st = read_all(path, &buf);
    if (st == ODIN3_OK) {
        odin3_script_src src = {path, ODIN3_SCRIPT_BY_LINE};
        st = script_do(design, (odin3_bytes){buf.data, buf.len}, &src);
    }
    odin3_strbuf_free(&buf);
    return st;
}

odin3_status odin3_pass_run_script_file(odin3_design *design, const char *path) {
    if (design == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "pass_run_script_file: NULL design");
        return ODIN3_ERR_INVALID_ARG;
    }
    return script_file_do(design, path);
}

odin3_status odin3_pass_resolve_script_file(const char *path) {
    return script_file_do(NULL, path);
}
