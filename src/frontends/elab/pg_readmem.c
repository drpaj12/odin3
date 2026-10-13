/*
 * pg_readmem.c — `$readmemh`/`$readmemb` as a statement of an `initial` (2D Task 24; EL-40, S4),
 * parsed as Yosys 0.55's AstNode::readmem does (simplify.cc 4446-4570): a file is read line by
 * line; a block comment is blanked, a token is a run of non-blanks, `//` ends the line, `@hex` sets
 * the address, any other token is a word, a sized literal of the memory's width in base 16 or 2
 * (digits 0-9 a-f x z ? and `_`; fewer digits than the width are zero-extended, or x/z-extended
 * when the leading digit is x/z; more are truncated), written at the address, which then steps
 * from `start` toward `finish` (default: the lowest and highest index), the file ending when the
 * address passes the last. The path of a relative file is the source file's directory (point
 * (y)), never the working directory (Yosys tries it first: spec 17). The words go into the state
 * of the initial run (pg_init.c) and become the memory's INIT with its other initial values.
 */
#include "frontends/elab/pg_init.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab2_mem.h"
#include "frontends/elab/elab_internal.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/file.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RM_MIN_ARGS = 2, RM_MAX_ARGS = 4, RM_HEX_BITS = 4, RM_RADIX_HEX = 16, RM_PATH = 4096 };

/* The largest address (and argument) taken: 2^40 words. The cursor steps by one a word, so a file
 * under the byte cap cannot carry it anywhere near int64's end (PR 2D-6 review I3). */
#define RM_ADDR_MAX ((int64_t)1 << 40)

/* How much of the first over-wide word the warning shows. */
enum { RM_WORD_SHOWN = 40 };

/* The call being run: where it is, the file and the memory. */
typedef struct rm_run {
    odin3_init *init;
    odin3_ast_id call;
    const char *task; /* "$readmemh" or "$readmemb" */
    const char *file; /* as written */
    bool hex;
    uint32_t mem;      /* the table index of the array */
    int64_t low, high; /* its lowest and highest declared index */
    int64_t cursor, finish;
    int increment;
    bool finished; /* the cursor has passed the last word to write (an explicit finish) */
    bool full;     /* the cursor has left the array: the words that follow are dropped */
    uint32_t dropped;
    int64_t first_dropped; /* the address of the first of them */
    uint32_t wide;         /* words with non-zero digits beyond the memory's width (truncated) */
    uint32_t wide_line;    /* the line of the first of them */
    char wide_word[RM_WORD_SHOWN + 1U];
} rm_run;

/* `bad address '@zz' in 'f' line N`, `bad word 'g' in 'f' line N`, at the call. */
static void rm_error(const rm_run *rm, const char *what, const char *token, uint32_t line) {
    odin3_elab2 *e2 = rm->init->e2;
    odin3_elab2_error(e2, odin3_ast_loc(e2->elab, rm->call), "%s '%s' in '%s' line %u", what, token,
                      rm->file, line);
}

/* --- the arguments ------------------------------------------------------------------------------
 */

/* The array named by the second argument, one-dimensional. */
static odin3_status rm_memory(rm_run *rm, odin3_ast_id arg) {
    odin3_elab2 *e2 = rm->init->e2;
    odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, arg);
    odin3_sym_id sym = kind == ODIN3_AST_IDENT || kind == ODIN3_AST_HIER_NAME
                           ? odin3_elab_sym_of(&e2->bind, arg)
                           : (odin3_sym_id){0};
    uint32_t mem = sym.v != 0 ? odin3_elab2_mem_find(e2, sym) : MEM_NONE;
    const odin3_elab_mem *rec = mem != MEM_NONE ? odin3_elab2_mem_get(e2, mem) : NULL;
    odin3_loc loc = odin3_ast_loc(e2->elab, rm->call);
    if (rec == NULL) {
        odin3_elab2_error(e2, loc, "the second argument of %s must be a memory", rm->task);
        return ODIN3_ERR_PARSE;
    }
    if (rec->n_dims != 1) {
        odin3_elab2_error(e2, loc, "%s needs a one-dimensional array; '%s' has %u dimensions",
                          rm->task, odin3_strtab_get(odin3_design_strtab(e2->design), rec->name),
                          rec->n_dims);
        return ODIN3_ERR_PARSE;
    }
    rm->mem = mem;
    rm->low = rec->offset;
    rm->high = (int64_t)rec->offset + (int64_t)rec->size - 1;
    return ODIN3_OK;
}

/* The constant start or finish address. */
static odin3_status rm_address(rm_run *rm, odin3_ast_id arg, int64_t *out) {
    odin3_elab2 *e2 = rm->init->e2;
    const odin3_ceval_req req = {arg, {0, false, false}, "address"};
    odin3_cval value;
    bool is_const = false;
    odin3_status st = odin3_ceval_try(rm->init->ev, &req, e2->scratch, &value, &is_const);
    if (st == ODIN3_OK && (!is_const || !odin3_cval_to_i64(&value, out))) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, arg),
                          "an address argument of %s is not constant", rm->task);
        return ODIN3_ERR_PARSE;
    }
    if (st == ODIN3_OK && (*out > RM_ADDR_MAX || *out < -RM_ADDR_MAX)) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, arg),
                          "an address argument of %s is out of range (beyond 2^40)", rm->task);
        return ODIN3_ERR_PARSE;
    }
    return st;
}

/* The arguments: file name, memory, the optional addresses. */
static odin3_status rm_args(rm_run *rm) {
    const odin3_ast *ast = rm->init->e2->elab;
    odin3_ast_span args = odin3_ast_children(ast, rm->call);
    odin3_loc loc = odin3_ast_loc(ast, rm->call);
    if (args.n < RM_MIN_ARGS || args.n > RM_MAX_ARGS) {
        odin3_elab2_error(rm->init->e2, loc, "%s takes 2 to 4 arguments; %u given", rm->task,
                          args.n);
        return ODIN3_ERR_PARSE;
    }
    if (odin3_ast_kind_of(ast, args.ids[0]) != ODIN3_AST_STRING) {
        odin3_elab2_error(rm->init->e2, loc, "the first argument of %s must be a string", rm->task);
        return ODIN3_ERR_PARSE;
    }
    rm->file = odin3_ast_name_str(ast, args.ids[0]);
    odin3_status st = rm_memory(rm, args.ids[1]);
    rm->cursor = rm->low;
    rm->finish = rm->high + 1;
    if (st == ODIN3_OK && args.n > 2) {
        st = rm_address(rm, args.ids[2], &rm->cursor);
    }
    if (st == ODIN3_OK && args.n > RM_MIN_ARGS + 1U) {
        st = rm_address(rm, args.ids[3], &rm->finish);
    }
    rm->increment = rm->cursor <= rm->finish ? 1 : -1;
    return st;
}

/* --- the file -----------------------------------------------------------------------------------
 */

/* The path of the file: absolute as given, else beside the source file holding the call. */
static odin3_status rm_path(const rm_run *rm, odin3_strbuf *path) {
    const odin3_elab2 *e2 = rm->init->e2;
    odin3_strbuf_clear(path);
    if (rm->file[0] == '/') {
        return odin3_strbuf_append(path, odin3_bytes_cstr(rm->file));
    }
    odin3_srcman *sm = NULL;
    odin3_srcpos pos;
    odin3_srcbuf_info info;
    odin3_status st = odin3_design_get_srcman(e2->design, &sm);
    if (st == ODIN3_OK && odin3_srcman_decode(sm, odin3_ast_loc(e2->elab, rm->call), &pos) &&
        odin3_srcman_buffer_info(sm, (odin3_srcbuf_id){pos.buffer}, &info)) {
        const char *source = odin3_strtab_get(odin3_design_strtab(e2->design),
                                              info.resolved != 0 ? info.resolved : info.name);
        const char *slash = strrchr(source, '/');
        st = slash == NULL
                 ? ODIN3_OK
                 : odin3_strbuf_append(path, (odin3_bytes){source, (size_t)(slash - source) + 1U});
    }
    return st != ODIN3_OK ? st : odin3_strbuf_append(path, odin3_bytes_cstr(rm->file));
}

/* Reads the file: the located errors for a file that cannot be opened and one over the cap. */
static odin3_status rm_read(const rm_run *rm, odin3_file_bytes *bytes) {
    odin3_elab2 *e2 = rm->init->e2;
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = rm_path(rm, &path);
    if (st == ODIN3_OK) {
        st = odin3_file_read_all(path.data != NULL ? path.data : "", e2->limits->max_readmem_bytes,
                                 bytes);
    }
    odin3_loc loc = odin3_ast_loc(e2->elab, rm->call);
    if (st == ODIN3_ERR_PARSE && bytes->too_big) {
        odin3_elab2_error(e2, loc, "'%s' is larger than %u bytes (the $readmem limit)", rm->file,
                          e2->limits->max_readmem_bytes);
    } else if (st == ODIN3_ERR_IO || rm->file[0] == '\0') {
        odin3_elab2_error(e2, loc, "cannot open '%s' for %s", rm->file, rm->task);
        st = ODIN3_ERR_PARSE;
    }
    odin3_strbuf_free(&path);
    return st;
}

/* --- words -------------------------------------------------------------------------------------
 */

/* The 4-state digits besides 0-9 and a-f. */
enum { DIG_X = 16, DIG_Z = 17, DIG_TEN = 10 };

/* The value of one hexadecimal digit character, -1 for any other. */
static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + DIG_TEN;
    }
    return ch >= 'A' && ch <= 'F' ? ch - 'A' + DIG_TEN : -1;
}

/* The value of one digit character in the radix (x, z and ? are the 4-state digits); -1: bad. */
static int digit_value(char ch, bool hex) {
    if (ch == 'x' || ch == 'X') {
        return DIG_X;
    }
    if (ch == 'z' || ch == 'Z' || ch == '?') {
        return DIG_Z;
    }
    int value = hex_value(ch);
    return value < (hex ? RM_RADIX_HEX : 2) ? value : -1;
}

/* Bit `pos` of the digit with value dig (x and z digits are x and z in every bit). */
static uint8_t digit_bit(int dig, uint32_t pos) {
    if (dig == DIG_X) {
        return (uint8_t)ODIN3_BIT_X;
    }
    if (dig == DIG_Z) {
        return (uint8_t)ODIN3_BIT_Z;
    }
    return (((unsigned)dig >> pos) & 1U) != 0 ? (uint8_t)ODIN3_BIT_1 : (uint8_t)ODIN3_BIT_0;
}

/* bits[0 .. width) <- the digits of token, LSB first, extended as a sized literal is; false when
 * a character is not a digit. *lost is set when a bit past the width is not 0 (the word is
 * truncated: leading zeros are fine). */
static bool word_bits(const char *token, bool hex, uint8_t *bits, uint32_t width, bool *lost) {
    size_t len = strlen(token);
    uint32_t per = hex ? RM_HEX_BITS : 1U;
    uint32_t pos = 0;
    uint8_t top = (uint8_t)ODIN3_BIT_0;
    for (size_t i = len; i > 0; i--) {
        if (token[i - 1U] == '_') {
            continue;
        }
        int dig = digit_value(token[i - 1U], hex);
        if (dig < 0) {
            return false;
        }
        for (uint32_t sub = 0; sub < per; sub++, pos++) {
            top = digit_bit(dig, sub);
            if (pos < width) {
                bits[pos] = top;
            } else if (top != (uint8_t)ODIN3_BIT_0) {
                *lost = true;
            }
        }
    }
    uint8_t pad =
        top == (uint8_t)ODIN3_BIT_X || top == (uint8_t)ODIN3_BIT_Z ? top : (uint8_t)ODIN3_BIT_0;
    for (; pos < width; pos++) {
        bits[pos] = pad;
    }
    return len > 0;
}

/* The word at the cursor, when it is inside the array; one outside it is counted for the warning.
 */
static odin3_status rm_store(rm_run *rm, const uint8_t *bits) {
    if (rm->cursor < rm->low || rm->cursor > rm->high) {
        rm->first_dropped = rm->dropped == 0 ? rm->cursor : rm->first_dropped;
        rm->dropped++;
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK;
    const odin3_elab_mem *mem = odin3_elab2_mem_get(rm->init->e2, rm->mem);
    odin3_init_wordref ref = {rm->mem, (uint32_t)(rm->cursor - rm->low)};
    uint8_t *word = odin3_init_word(rm->init, &ref, &st);
    if (word == NULL) {
        return st;
    }
    st = odin3_init_journal(rm->init, word, mem->width);
    memcpy(word, bits, mem->width);
    return st;
}

/* Whether the cursor has passed the last word to write (Yosys's two tests). */
static bool rm_done(const rm_run *rm) {
    return rm->cursor == rm->finish + rm->increment ||
           (rm->increment > 0 && rm->cursor > rm->high) ||
           (rm->increment < 0 && rm->cursor < rm->low);
}

/* The cursor has passed the last word to write: at an explicit finish inside the array the load
 * ends; past the end of the array the file is read on, to count the words it cannot place. */
static void rm_stop(rm_run *rm) {
    if (rm->cursor < rm->low || rm->cursor > rm->high) {
        rm->full = true;
    } else {
        rm->finished = true;
    }
}

/* The tokens of one line (the comments already blanked). */
typedef struct rm_line {
    char *text;
    uint32_t number;
} rm_line;

/* The address of an `@hex` token: hex digits only (no sign, no `0x`, no `_`), at most 2^40. A bad
 * or oversized one is a located error, never clamped. */
static odin3_status rm_at_address(rm_run *rm, const rm_line *line, const char *token) {
    int64_t address = 0;
    bool ok = token[1] != '\0';
    for (const char *ch = token + 1; ok && *ch != '\0'; ch++) {
        int digit = hex_value(*ch);
        ok = digit >= 0 && address <= (RM_ADDR_MAX - digit) / RM_RADIX_HEX;
        address = ok ? address * RM_RADIX_HEX + digit : address;
    }
    if (!ok) {
        bool shaped = token[1] != '\0';
        for (const char *ch = token + 1; shaped && *ch != '\0'; ch++) {
            shaped = hex_value(*ch) >= 0;
        }
        rm_error(rm, shaped ? "address out of range (beyond 2^40)" : "bad address", token,
                 line->number);
        return ODIN3_ERR_PARSE;
    }
    rm->cursor = address;
    return ODIN3_OK;
}

static odin3_status rm_token(rm_run *rm, const rm_line *line, const char *token, uint8_t *scratch) {
    if (token[0] == '@') {
        return rm_at_address(rm, line, token);
    }
    if (rm->full) { /* past the end of the array (Yosys stops reading here): counted, not read */
        rm->first_dropped = rm->dropped == 0 ? rm->cursor : rm->first_dropped;
        rm->dropped++;
        rm->cursor += rm->increment;
        return ODIN3_OK;
    }
    const odin3_elab_mem *mem = odin3_elab2_mem_get(rm->init->e2, rm->mem);
    bool lost = false;
    if (!word_bits(token, rm->hex, scratch, mem->width, &lost)) {
        rm_error(rm, "bad word", token, line->number);
        return ODIN3_ERR_PARSE;
    }
    if (lost && rm->wide++ == 0) {
        (void)snprintf(rm->wide_word, sizeof rm->wide_word, "%s", token);
        rm->wide_line = line->number;
    }
    odin3_status st = rm_store(rm, scratch);
    rm->cursor += rm->increment;
    return st;
}

/* A line with its block comments blanked (in_comment carries across lines). */
static void blank_comments(char *line, bool *in_comment) {
    for (size_t i = 0; line[i] != '\0'; i++) {
        if (*in_comment && line[i] == '*' && line[i + 1U] == '/') {
            line[i] = ' ';
            line[i + 1U] = ' ';
            *in_comment = false;
            continue;
        }
        if (!*in_comment && line[i] == '/' && line[i + 1U] == '*') {
            *in_comment = true;
        }
        if (*in_comment) {
            line[i] = ' ';
        }
    }
}

/* The tokens of a line, until `//` or the last word to write. */
static odin3_status rm_tokens(rm_run *rm, rm_line *line, uint8_t *scratch) {
    static const char blanks[] = " \t\r\n";
    char *save = NULL;
    odin3_status st = ODIN3_OK;
    for (const char *tok = strtok_r(line->text, blanks, &save); st == ODIN3_OK && tok != NULL;
         tok = strtok_r(NULL, blanks, &save)) {
        if (tok[0] == '/' && tok[1] == '/') {
            break;
        }
        st = rm_token(rm, line, tok, scratch);
        if (st == ODIN3_OK && tok[0] != '@' && !rm->full && rm_done(rm)) {
            rm_stop(rm);
            if (rm->finished) {
                break;
            }
        }
    }
    if (st == ODIN3_OK && !rm->full && rm_done(rm)) {
        rm_stop(rm);
    }
    return st;
}

/* The file's lines in turn (empty ones counted: the line numbers of the errors). */
static odin3_status rm_parse(rm_run *rm, const odin3_file_bytes *bytes) {
    odin3_elab2 *e2 = rm->init->e2;
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, rm->mem);
    uint8_t *scratch = odin3_arena_alloc(e2->scratch, mem->width);
    char *copy = odin3_arena_alloc(e2->scratch, bytes->len + 1U);
    if (scratch == NULL || copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (bytes->len > 0) {
        memcpy(copy, bytes->data, bytes->len);
    }
    copy[bytes->len] = '\0';
    bool in_comment = false;
    odin3_status st = ODIN3_OK;
    uint32_t number = 0;
    for (size_t at = 0; st == ODIN3_OK && !rm->finished && at <= bytes->len;) {
        char *end = strchr(copy + at, '\n');
        size_t next = end != NULL ? (size_t)(end - copy) + 1U : bytes->len + 1U;
        if (end != NULL) {
            *end = '\0';
        }
        rm_line line = {copy + at, ++number};
        blank_comments(line.text, &in_comment);
        st = rm_tokens(rm, &line, scratch);
        at = next;
    }
    return st;
}

odin3_status odin3_elab2_readmem(const odin3_elab2 *e2, odin3_ast_id call,
                                 const odin3_ceval_store *store) {
    if (e2 == NULL || store == NULL || store->user == NULL ||
        odin3_ast_kind_of(e2->elab, call) != ODIN3_AST_TASK_CALL ||
        (odin3_ast_flags(e2->elab, call) & ODIN3_AST_F_TASK_CALL_SYSTEM) == 0) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_readmem: bad argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    rm_run rm = {.init = store->user, .call = call};
    rm.task = odin3_ast_name_str(e2->elab, call);
    rm.hex = strcmp(rm.task, "$readmemh") == 0;
    odin3_file_bytes bytes = {0};
    odin3_status st = rm_args(&rm);
    st = st != ODIN3_OK ? st : rm_read(&rm, &bytes);
    st = st != ODIN3_OK ? st : rm_parse(&rm, &bytes);
    if (st == ODIN3_OK && rm.dropped > 0) {
        odin3_elab2_warn(rm.init->e2, odin3_ast_loc(e2->elab, call),
                         "%s of '%s' dropped %u word%s outside the array (the first at address "
                         "%" PRId64 ")",
                         rm.task, rm.file, rm.dropped, rm.dropped == 1 ? "" : "s",
                         rm.first_dropped);
    }
    if (st == ODIN3_OK && rm.wide > 0) {
        odin3_elab2_warn(rm.init->e2, odin3_ast_loc(e2->elab, call),
                         "%s of '%s' truncated %u word%s wider than the memory's %u bits (the "
                         "first, '%s', at line %u)",
                         rm.task, rm.file, rm.wide, rm.wide == 1 ? "" : "s",
                         odin3_elab2_mem_get(e2, rm.mem)->width, rm.wide_word, rm.wide_line);
    }
    odin3_file_bytes_free(&bytes);
    return st;
}
