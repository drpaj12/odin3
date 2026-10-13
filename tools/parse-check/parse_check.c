/*
 * parse_check.c — one-file reads with the token map, and the location oracle (2C spec §7.2; plan
 * Task 9). Shared by odin3-parse-check and tests/unit/test_parse_check.c.
 */
#include "parse_check.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "frontends/project/paths.h"
#include "frontends/verilog/parser.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/read_verilog.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum {
    PC_CWD_BYTES = 4096,
    PC_MSG_BYTES = 512,
    PC_DECIMAL = 10,
    PC_DECIMAL_DIGITS = 19, /* the most that always fit in 64 bits */
    PC_VALUE_BITS = 64
};

/* The library a directly given file belongs to (the pass's default for direct files). */
static const char k_work[] = "work";

/* --- reading -------------------------------------------------------------------------------- */

/* path as a direct design file of design: given as written, resolved against the working
 * directory, library work, no naming loc. */
static odin3_status pc_source(odin3_design *design, const char *path, odin3_pp_source *src) {
    char cwd[PC_CWD_BYTES];
    if (getcwd(cwd, sizeof cwd) == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3-parse-check: cannot read the working directory");
        return ODIN3_ERR_IO;
    }
    *src = (odin3_pp_source){0};
    odin3_strbuf resolved;
    odin3_strbuf_init(&resolved);
    odin3_status st = odin3_path_resolve(
        (odin3_path_join){odin3_bytes_cstr(cwd), odin3_bytes_cstr(path)}, &resolved);
    st = st != ODIN3_OK ? st : odin3_design_intern(design, odin3_bytes_cstr(path), &src->given);
    st = st != ODIN3_OK ? st
                        : odin3_design_intern(design, (odin3_bytes){resolved.data, resolved.len},
                                              &src->resolved);
    st = st != ODIN3_OK ? st : odin3_design_intern(design, odin3_bytes_cstr(k_work), &src->library);
    odin3_strbuf_free(&resolved);
    return st;
}

/* The stream's bytes: path preprocessed again on a scratch design, as the reader did. */
static odin3_status pc_stream_bytes(const char *path, odin3_strbuf *out) {
    odin3_design *scratch = odin3_design_create();
    if (scratch == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_pp *pp = NULL;
    odin3_pp_source src;
    odin3_pp_config config = {0};
    odin3_pp_stream stream = {0};
    odin3_status st = pc_source(scratch, path, &src);
    st = st != ODIN3_OK ? st : odin3_pp_create(scratch, &config, &pp);
    st = st != ODIN3_OK ? st : odin3_pp_run(pp, &src, &stream);
    st = st != ODIN3_OK ? st : odin3_strbuf_append(out, stream.text);
    odin3_pp_destroy(pp);
    odin3_design_destroy(scratch);
    return st;
}

/* The project FILE buffer named given (the read's one stream); 0 when there is none. */
static odin3_srcbuf_id pc_stream_buffer(odin3_design *design, uint32_t given) {
    odin3_srcman *sm = NULL;
    odin3_srcbuf_id found = {0};
    if (odin3_design_get_srcman(design, &sm) != ODIN3_OK) {
        return found;
    }
    odin3_srcbuf_info info;
    for (odin3_srcbuf_id buf = {1}; odin3_srcman_buffer_info(sm, buf, &info); buf.v++) {
        if (info.kind == ODIN3_SRCBUF_FILE && info.parent == 0 && info.name == given) {
            found = buf;
        }
    }
    return found;
}

odin3_status pc_read_file(const char *path, pc_read *out) {
    memset(out, 0, sizeof *out);
    odin3_vec_init(&out->tokens, sizeof(odin3_vp_loc));
    odin3_strbuf_init(&out->stream);
    out->design = odin3_design_create();
    if (out->design == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_pp_source src;
    odin3_status st = pc_source(out->design, path, &src);
    if (st == ODIN3_OK) {
        odin3_rv_opts opts = {&src, 1, (odin3_passrun_id){1}, &out->tokens, false};
        st = odin3_read_verilog_files(out->design, &opts, &out->ast);
    }
    if (st == ODIN3_OK && odin3_ast_root_count(out->ast) != 1) {
        odin3_log(ODIN3_LOG_ERROR, "%s: the read gave %u units, not 1", path,
                  (unsigned)odin3_ast_root_count(out->ast));
        st = ODIN3_ERR_PARSE;
    }
    if (st == ODIN3_OK) {
        out->unit = odin3_ast_root(out->ast, 0);
        out->stream_buf = pc_stream_buffer(out->design, src.given);
        st = pc_stream_bytes(path, &out->stream);
    }
    return st;
}

void pc_read_free(pc_read *read) {
    if (read == NULL) {
        return;
    }
    odin3_ast_destroy(read->ast);
    odin3_design_destroy(read->design);
    odin3_vec_free(&read->tokens);
    odin3_strbuf_free(&read->stream);
    memset(read, 0, sizeof *read);
}

/* --- the oracle's token map ----------------------------------------------------------------- */

/*
 * The oracle's state: the read, its source manager, the token map with each token's stream
 * length (0 for the end of input or a token whose last byte could not be found), the maps from a
 * token's loc and from its end to its index, and the parent of every node.
 */
typedef struct pc_oracle {
    const pc_read *read;
    const odin3_srcman *sm;
    const odin3_vp_loc *toks;
    uint32_t ntok;
    uint32_t *lens;
    odin3_u64map *starts;
    odin3_u64map *ends;
    odin3_ast_parent_index parents;
    uint32_t failures;
} pc_oracle;

/* The stream length of token i: the bytes up to the one whose loc is end - 1 (the last byte),
 * searched before the next token; 0 when not found. */
static uint32_t pc_token_len(pc_oracle *orc, odin3_srcman_cursor *cur, uint32_t idx) {
    const odin3_vp_loc *tok = &orc->toks[idx];
    uint32_t next = idx + 1 < orc->ntok ? orc->toks[idx + 1].off : (uint32_t)orc->read->stream.len;
    if (tok->end.v <= tok->loc.v || tok->off >= next || next > orc->read->stream.len) {
        return 0;
    }
    uint32_t guess = tok->end.v - tok->loc.v;
    if (guess <= next - tok->off &&
        odin3_srcman_cursor_loc(cur, tok->off + guess - 1).v + 1 == tok->end.v) {
        return guess;
    }
    for (uint32_t at = tok->off; at < next; at++) {
        if (odin3_srcman_cursor_loc(cur, at).v + 1 == tok->end.v) {
            return at - tok->off + 1;
        }
    }
    return 0;
}

/* A token's loc is its first byte's: the stream map's loc at its offset (not the end of input,
 * which carries the last token's end). */
static void pc_check_token_loc(pc_oracle *orc, odin3_srcman_cursor *cur, uint32_t idx) {
    const odin3_vp_loc *tok = &orc->toks[idx];
    if (idx + 1 == orc->ntok || tok->off >= orc->read->stream.len) {
        return;
    }
    odin3_loc want = odin3_srcman_cursor_loc(cur, tok->off);
    if (want.v != tok->loc.v) {
        odin3_diag(orc->read->design, ODIN3_LOG_ERROR, tok->loc,
                   "location oracle: token %u at stream offset %u: loc %u, not the map's %u",
                   (unsigned)idx, (unsigned)tok->off, (unsigned)tok->loc.v, (unsigned)want.v);
        orc->failures++;
    }
}

static odin3_status pc_map_put(odin3_u64map *map, uint32_t key, uint32_t value) {
    uint64_t old = 0;
    if (key == 0 || odin3_u64map_get(map, key, &old)) {
        return ODIN3_OK; /* the first token at a loc wins (none share one) */
    }
    return odin3_u64map_put(map, (odin3_kv){key, value});
}

static odin3_status pc_oracle_open(pc_oracle *orc, const pc_read *read) {
    memset(orc, 0, sizeof *orc);
    orc->read = read;
    orc->toks = read->tokens.data;
    orc->ntok = (uint32_t)read->tokens.len;
    odin3_srcman *sm = NULL;
    if (odin3_design_get_srcman(read->design, &sm) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    orc->sm = sm;
    orc->lens = odin3_util_calloc(((size_t)orc->ntok + 1U) * sizeof *orc->lens);
    orc->starts = odin3_u64map_create(orc->ntok);
    orc->ends = odin3_u64map_create(orc->ntok);
    if (orc->lens == NULL || orc->starts == NULL || orc->ends == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_srcman_cursor cur;
    odin3_srcman_cursor_init(&cur, sm, read->stream_buf.v);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < orc->ntok; i++) {
        pc_check_token_loc(orc, &cur, i);
        orc->lens[i] = pc_token_len(orc, &cur, i);
        st = pc_map_put(orc->starts, orc->toks[i].loc.v, i);
        st = st != ODIN3_OK ? st : pc_map_put(orc->ends, orc->toks[i].end.v, i);
    }
    if (orc->ntok == 0 || orc->toks[orc->ntok - 1].off != read->stream.len) {
        odin3_log(ODIN3_LOG_ERROR,
                  "location oracle: the token map does not end at the stream's "
                  "end (%zu bytes)",
                  read->stream.len);
        orc->failures++;
    }
    return st != ODIN3_OK ? st : odin3_ast_parent_index_build(read->ast, &orc->parents);
}

static void pc_oracle_close(pc_oracle *orc) {
    odin3_util_free(orc->lens);
    odin3_u64map_destroy(orc->starts);
    odin3_u64map_destroy(orc->ends);
    odin3_ast_parent_index_destroy(&orc->parents);
}

/* The token at loc (a token's first byte) or ending at end; false when there is none. */
static bool pc_tok_at(const odin3_u64map *map, odin3_loc at, uint32_t *idx) {
    uint64_t value = 0;
    if (at.v == 0 || !odin3_u64map_get(map, at.v, &value)) {
        return false;
    }
    *idx = (uint32_t)value;
    return true;
}

static odin3_bytes pc_tok_text(const pc_oracle *orc, uint32_t idx) {
    return (odin3_bytes){(const uint8_t *)orc->read->stream.data + orc->toks[idx].off,
                         orc->lens[idx]};
}

static bool pc_text_is(odin3_bytes text, const char *word) {
    size_t len = strlen(word);
    return text.len == len && memcmp(text.ptr, word, len) == 0;
}

/* text equals one of the '|'-separated words of list. */
static bool pc_text_in(odin3_bytes text, const char *list) {
    for (const char *word = list; *word != '\0';) {
        const char *bar = strchr(word, '|');
        size_t len = bar != NULL ? (size_t)(bar - word) : strlen(word);
        if (text.len == len && memcmp(text.ptr, word, len) == 0) {
            return true;
        }
        word += bar != NULL ? len + 1 : len;
    }
    return false;
}

static bool pc_text_starts(odin3_bytes text, const char *prefix) {
    size_t len = strlen(prefix);
    return text.len >= len && memcmp(text.ptr, prefix, len) == 0;
}

/* text starts with one of the '|'-separated prefixes of list. */
static bool pc_text_starts_any(odin3_bytes text, const char *list) {
    for (const char *word = list; *word != '\0';) {
        const char *bar = strchr(word, '|');
        size_t len = bar != NULL ? (size_t)(bar - word) : strlen(word);
        if (text.len >= len && memcmp(text.ptr, word, len) == 0) {
            return true;
        }
        word += bar != NULL ? len + 1 : len;
    }
    return false;
}

/* Tokens that close a construct: what may follow a node's last child inside the node. */
static const char k_closers[] = ")|]|}|;|*)|end|join|endcase|endmodule|endprimitive|endfunction|"
                                "endtask|endgenerate|endspecify|endconfig|endtable";

/* --- failures ------------------------------------------------------------------------------- */

/* One failure, located at the node and naming it: "location oracle: KIND[:name] (node N): …". */
static void pc_fail(pc_oracle *orc, odin3_ast_id node, const char *fmt, ...) ODIN3_PRINTF(3, 4);
static void pc_fail(pc_oracle *orc, odin3_ast_id node, const char *fmt, ...) {
    char msg[PC_MSG_BYTES];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    const odin3_ast *ast = orc->read->ast;
    const char *name = odin3_ast_name_str(ast, node);
    odin3_diag(orc->read->design, ODIN3_LOG_ERROR, odin3_ast_loc(ast, node),
               "location oracle: %s%s%s (node %u): %s",
               odin3_ast_kind_name(odin3_ast_kind_of(ast, node)), *name != '\0' ? ":" : "", name,
               (unsigned)node.v, msg);
    orc->failures++;
}

/* --- the per-kind table --------------------------------------------------------------------- */

typedef enum pc_lead {
    PC_LEAD_ANY = 0,   /* no spelling rule */
    PC_LEAD_WORD,      /* one of the words */
    PC_LEAD_PREFIX,    /* starts with one of the words */
    PC_LEAD_SUB,       /* the sub-kind's spelling */
    PC_LEAD_NAME,      /* the node's name (an escaped identifier without its backslash) */
    PC_LEAD_NUMBER,    /* a NUMBER's first piece */
    PC_LEAD_DIRECTIVE, /* a backtick and the sub-kind's spelling */
    PC_LEAD_CHILD      /* its first child's first token (a DEFPARAM pair) */
} pc_lead;

/* When a kind's closing token applies. */
typedef enum pc_close_when {
    PC_CLOSE_ALWAYS = 0,
    PC_CLOSE_ITEM,   /* not inside a LIST (an ANSI header port, a header parameter) */
    PC_CLOSE_STMT,   /* not a for header's initialisation or step */
    PC_CLOSE_OPENED, /* only when the node starts at the opener named in words */
    PC_CLOSE_NAMED   /* only when the node has a name */
} pc_close_when;

typedef struct pc_rule {
    uint8_t lead;
    uint8_t when;
    const char *words; /* PC_LEAD_WORD / PC_LEAD_PREFIX, or PC_CLOSE_OPENED's opener */
    const char *close; /* the token the node ends at; NULL = no rule */
} pc_rule;

static const pc_rule k_rules[ODIN3_AST_KIND_COUNT] = {
    [ODIN3_AST_DIRECTIVE] = {PC_LEAD_DIRECTIVE, 0, NULL, NULL},
    [ODIN3_AST_MODULE] = {PC_LEAD_WORD, 0, "module|macromodule", "endmodule"},
    [ODIN3_AST_UDP_DECL] = {PC_LEAD_PREFIX, 0, "primitive", "endprimitive"},
    [ODIN3_AST_CONFIG_DECL] = {PC_LEAD_PREFIX, 0, "config", "endconfig"},
    [ODIN3_AST_LIST] = {PC_LEAD_ANY, PC_CLOSE_OPENED, "(|#", ")"},
    [ODIN3_AST_PORT_DECL] = {PC_LEAD_SUB, PC_CLOSE_ITEM, NULL, ";"},
    [ODIN3_AST_DECLARATOR] = {PC_LEAD_NAME, 0, NULL, NULL},
    [ODIN3_AST_RANGE] = {PC_LEAD_WORD, 0, "[", "]"},
    [ODIN3_AST_NET_DECL] = {PC_LEAD_SUB, 0, NULL, ";"},
    [ODIN3_AST_VAR_DECL] = {PC_LEAD_SUB, PC_CLOSE_ITEM, NULL, ";"},
    [ODIN3_AST_PARAM_DECL] = {PC_LEAD_SUB, PC_CLOSE_ITEM, NULL, ";"},
    [ODIN3_AST_DEFPARAM] = {PC_LEAD_CHILD, 0, NULL, ""},
    [ODIN3_AST_GENVAR_DECL] = {PC_LEAD_WORD, 0, "genvar", ";"},
    [ODIN3_AST_EVENT_DECL] = {PC_LEAD_WORD, 0, "event", ";"},
    [ODIN3_AST_STRENGTH] = {PC_LEAD_WORD, 0, "(", ")"},
    [ODIN3_AST_DELAY] = {PC_LEAD_WORD, 0, "#", NULL},
    [ODIN3_AST_CONT_ASSIGN] = {PC_LEAD_WORD, 0, "assign", ";"},
    [ODIN3_AST_ALWAYS] = {PC_LEAD_SUB, 0, NULL, NULL},
    [ODIN3_AST_INITIAL] = {PC_LEAD_WORD, 0, "initial", NULL},
    [ODIN3_AST_INSTANTIATION] = {PC_LEAD_NAME, 0, NULL, ";"},
    [ODIN3_AST_INSTANCE] = {PC_LEAD_ANY, 0, NULL, ")"},
    [ODIN3_AST_CONNECTION] = {PC_LEAD_ANY, PC_CLOSE_NAMED, NULL, ")"},
    [ODIN3_AST_GATE_DECL] = {PC_LEAD_SUB, 0, NULL, ";"},
    [ODIN3_AST_FUNCTION_DECL] = {PC_LEAD_WORD, 0, "function", "endfunction"},
    [ODIN3_AST_TASK_DECL] = {PC_LEAD_WORD, 0, "task", "endtask"},
    [ODIN3_AST_SPECIFY_BLOCK] = {PC_LEAD_PREFIX, 0, "specify", "endspecify"},
    [ODIN3_AST_ATTR] = {PC_LEAD_NAME, 0, NULL, NULL},
    [ODIN3_AST_GENERATE] = {PC_LEAD_WORD, 0, "generate", "endgenerate"},
    [ODIN3_AST_GEN_FOR] = {PC_LEAD_WORD, 0, "for", NULL},
    [ODIN3_AST_GEN_IF] = {PC_LEAD_WORD, 0, "if", NULL},
    [ODIN3_AST_GEN_CASE] = {PC_LEAD_WORD, 0, "case|casez|casex", "endcase"},
    [ODIN3_AST_GEN_BLOCK] = {PC_LEAD_ANY, PC_CLOSE_OPENED, "begin", "end"},
    [ODIN3_AST_SEQ_BLOCK] = {PC_LEAD_WORD, 0, "begin", "end"},
    [ODIN3_AST_PAR_BLOCK] = {PC_LEAD_WORD, 0, "fork", "join"},
    [ODIN3_AST_BLOCKING_ASSIGN] = {PC_LEAD_ANY, PC_CLOSE_STMT, NULL, ";"},
    [ODIN3_AST_NONBLOCKING_ASSIGN] = {PC_LEAD_ANY, PC_CLOSE_STMT, NULL, ";"},
    [ODIN3_AST_PROC_CONT_ASSIGN] = {PC_LEAD_SUB, 0, NULL, ";"},
    [ODIN3_AST_IF] = {PC_LEAD_WORD, 0, "if", NULL},
    [ODIN3_AST_CASE] = {PC_LEAD_SUB, 0, NULL, "endcase"},
    [ODIN3_AST_FOR] = {PC_LEAD_WORD, 0, "for", NULL},
    [ODIN3_AST_WHILE] = {PC_LEAD_WORD, 0, "while", NULL},
    [ODIN3_AST_REPEAT] = {PC_LEAD_WORD, 0, "repeat", NULL},
    [ODIN3_AST_FOREVER] = {PC_LEAD_WORD, 0, "forever", NULL},
    [ODIN3_AST_TIMING_STMT] = {PC_LEAD_PREFIX, 0, "#|@", NULL},
    [ODIN3_AST_WAIT] = {PC_LEAD_WORD, 0, "wait", NULL},
    [ODIN3_AST_DISABLE] = {PC_LEAD_WORD, 0, "disable", ";"},
    [ODIN3_AST_EVENT_TRIGGER] = {PC_LEAD_WORD, 0, "->", ";"},
    [ODIN3_AST_TASK_CALL] = {PC_LEAD_NAME, 0, NULL, ";"},
    [ODIN3_AST_NULL_STMT] = {PC_LEAD_WORD, 0, ";", ";"},
    [ODIN3_AST_EVENT_CONTROL] = {PC_LEAD_PREFIX, 0, "@", NULL},
    [ODIN3_AST_EVENT_EXPR] = {PC_LEAD_SUB, 0, NULL, NULL},
    [ODIN3_AST_NUMBER] = {PC_LEAD_NUMBER, 0, NULL, NULL},
    [ODIN3_AST_REAL] = {PC_LEAD_PREFIX, 0, "0|1|2|3|4|5|6|7|8|9", NULL},
    [ODIN3_AST_STRING] = {PC_LEAD_PREFIX, 0, "\"", NULL},
    [ODIN3_AST_IDENT] = {PC_LEAD_NAME, 0, NULL, NULL},
    [ODIN3_AST_CONCAT] = {PC_LEAD_WORD, 0, "{", "}"},
    [ODIN3_AST_REPLICATE] = {PC_LEAD_WORD, 0, "{", "}"},
    [ODIN3_AST_UNARY] = {PC_LEAD_SUB, 0, NULL, NULL},
    [ODIN3_AST_CALL] = {PC_LEAD_NAME, 0, NULL, NULL},
};

/* --- spelling at the loc -------------------------------------------------------------------- */

static bool pc_name_spelled(const pc_oracle *orc, odin3_ast_id node, odin3_bytes tok) {
    odin3_bytes name = odin3_bytes_cstr(odin3_ast_name_str(orc->read->ast, node));
    const uint8_t *text = tok.ptr;
    size_t len = tok.len;
    if (len > 0 && text[0] == '\\') { /* an escaped identifier: the name is unescaped */
        text++;
        len--;
    }
    return len == name.len && memcmp(text, name.ptr, name.len) == 0;
}

/* The decimal digits of text ('_' allowed) as a value; false for another byte or one past
 * PC_DECIMAL_DIGITS digits. */
static bool pc_decimal(odin3_bytes text, uint64_t *value) {
    const uint8_t *bytes = text.ptr;
    uint32_t digits = 0;
    *value = 0;
    for (size_t i = 0; i < text.len; i++) {
        if (bytes[i] == '_') {
            continue;
        }
        if (bytes[i] < '0' || bytes[i] > '9' || ++digits > PC_DECIMAL_DIGITS) {
            return false;
        }
        *value = *value * PC_DECIMAL + (uint64_t)(bytes[i] - '0');
    }
    return digits > 0;
}

/* The stored bits of num as a value; false with an x/z bit or a 1 past bit 63. */
static bool pc_bits_value(const odin3_ast_number *num, uint64_t *value) {
    *value = 0;
    for (uint32_t i = 0; i < num->nbits; i++) {
        if (num->bits[i] > 1 || (num->bits[i] == 1 && i >= PC_VALUE_BITS)) {
            return false;
        }
        *value |= (uint64_t)num->bits[i] << (i < PC_VALUE_BITS ? i : 0);
    }
    return true;
}

/* A NUMBER's first piece: its size (the width in decimal) when it has one, else a base (`'h`)
 * or a plain decimal's digits, which spell its value. */
static bool pc_number_spelled(const pc_oracle *orc, odin3_ast_id node, odin3_bytes tok) {
    const odin3_ast_number *num = odin3_ast_number_of(orc->read->ast, node);
    if (num == NULL || tok.len == 0) {
        return false;
    }
    uint64_t written = 0;
    uint64_t stored = 0;
    if (num->width != 0) {
        return pc_decimal(tok, &written) && written == num->width;
    }
    if (((const uint8_t *)tok.ptr)[0] == '\'') {
        return true;
    }
    if (!pc_decimal(tok, &written)) { /* longer than PC_DECIMAL_DIGITS: digits only */
        return pc_text_starts_any(tok, "0|1|2|3|4|5|6|7|8|9");
    }
    return pc_bits_value(num, &stored) && stored == written;
}

/* Kinds whose name rule has exceptions: a hierarchical call is named by its path, a dotted or
 * unnamed port reference, an unnamed instance and a named connection start at punctuation. */
static bool pc_special_lead(const pc_oracle *orc, odin3_ast_id node, odin3_bytes tok, bool *ok) {
    const odin3_ast *ast = orc->read->ast;
    uint32_t flags = odin3_ast_flags(ast, node);
    bool named = odin3_ast_name(ast, node) != 0;
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_CALL:
        *ok = (flags & ODIN3_AST_F_CALL_HIER) != 0 || pc_name_spelled(orc, node, tok);
        return true;
    case ODIN3_AST_TASK_CALL:
        *ok = (flags & ODIN3_AST_F_TASK_CALL_HIER) != 0 || pc_name_spelled(orc, node, tok);
        return true;
    case ODIN3_AST_PORT_REF:
        if ((flags & ODIN3_AST_F_PORT_REF_DOTTED) != 0) {
            *ok = pc_text_is(tok, ".");
        } else {
            *ok = !named || pc_name_spelled(orc, node, tok);
        }
        return true;
    case ODIN3_AST_INSTANCE:
        *ok = named ? pc_name_spelled(orc, node, tok) : pc_text_is(tok, "(");
        return true;
    case ODIN3_AST_CONNECTION:
        *ok = !named || pc_text_is(tok, ".");
        return true;
    default:
        return false;
    }
}

static bool pc_binary_not(const pc_oracle *orc, odin3_ast_id node);

static bool pc_sub_spelled(const pc_oracle *orc, odin3_ast_id node, odin3_bytes tok) {
    const odin3_ast *ast = orc->read->ast;
    if (pc_binary_not(orc, node)) {
        return true; /* checked as its operand */
    }
    if (odin3_ast_kind_of(ast, node) == ODIN3_AST_EVENT_EXPR &&
        odin3_ast_sub(ast, node) == ODIN3_AST_EVENT_EXPR_NONE) {
        return true; /* a plain expression event */
    }
    if (odin3_ast_kind_of(ast, node) == ODIN3_AST_UNARY &&
        odin3_ast_sub(ast, node) == ODIN3_AST_UNARY_XNOR && pc_text_is(tok, "^~")) {
        return true;
    }
    return pc_text_is(tok, odin3_ast_sub_name(ast, node));
}

typedef struct pc_kids pc_kids;
static bool pc_starts_at_child(const pc_oracle *orc, odin3_ast_id node);

/* The node's first token spells it (the kind's lead rule). */
static bool pc_spelled(const pc_oracle *orc, odin3_ast_id node, odin3_bytes tok) {
    bool ok = true;
    if (pc_special_lead(orc, node, tok, &ok)) {
        return ok;
    }
    const pc_rule *rule = &k_rules[odin3_ast_kind_of(orc->read->ast, node)];
    switch (rule->lead) {
    case PC_LEAD_WORD:
        return pc_text_in(tok, rule->words);
    case PC_LEAD_PREFIX:
        return pc_text_starts_any(tok, rule->words);
    case PC_LEAD_SUB:
        return pc_sub_spelled(orc, node, tok);
    case PC_LEAD_NAME:
        return pc_name_spelled(orc, node, tok);
    case PC_LEAD_NUMBER:
        return pc_number_spelled(orc, node, tok);
    case PC_LEAD_CHILD:
        return pc_starts_at_child(orc, node);
    case PC_LEAD_DIRECTIVE:
        return pc_text_starts(tok, "`") && tok.len > 1 &&
               pc_text_starts((odin3_bytes){(const uint8_t *)tok.ptr + 1, tok.len - 1},
                              odin3_ast_sub_name(orc->read->ast, node));
    default:
        return true;
    }
}

/* --- the per-node checks -------------------------------------------------------------------- */

/* A node's place in the stream: 2i for "at token i", 2i + 1 for "just after token i". */
typedef struct pc_span {
    uint64_t from, to;
    uint32_t first, last; /* the first and last token (valid when the node is not empty) */
    bool known, empty;
} pc_span;

static pc_span pc_span_of(const pc_oracle *orc, odin3_ast_id node) {
    const odin3_ast *ast = orc->read->ast;
    odin3_loc loc = odin3_ast_loc(ast, node);
    odin3_loc end = odin3_ast_end(ast, node);
    pc_span span = {0};
    uint32_t i = 0;
    if (loc.v != 0 && loc.v == end.v) {
        span.empty = true;
        if (pc_tok_at(orc->ends, loc, &i)) {
            span.from = span.to = 2ULL * i + 1;
            span.known = true;
        } else if (pc_tok_at(orc->starts, loc, &i)) {
            span.from = span.to = 2ULL * i;
            span.known = true;
        }
        return span;
    }
    span.known = pc_tok_at(orc->starts, loc, &span.first) && pc_tok_at(orc->ends, end, &span.last);
    span.from = 2ULL * span.first;
    span.to = 2ULL * span.last + 1;
    return span;
}

/* The parser's reading of a binary `~&` / `~|` (2C Task 10): UNARY[~] at the same loc as its
 * operand, a BINARY[&] or BINARY[|], whose operator token (the first after its left operand and
 * any closing parentheses) is `~&` or `~|`; its first token is the BINARY's. A written
 * `~(a & b)` has `&` there, so its UNARY moved onto its operand still fails. */
static bool pc_binary_not(const pc_oracle *orc, odin3_ast_id node) {
    const odin3_ast *ast = orc->read->ast;
    if (odin3_ast_kind_of(ast, node) != ODIN3_AST_UNARY ||
        odin3_ast_sub(ast, node) != ODIN3_AST_UNARY_NOT || odin3_ast_nchild(ast, node) != 1) {
        return false;
    }
    odin3_ast_id operand = odin3_ast_child(ast, node, 0);
    uint32_t sub = operand.v != 0 ? odin3_ast_sub(ast, operand) : 0;
    if (operand.v == 0 || odin3_ast_kind_of(ast, operand) != ODIN3_AST_BINARY ||
        (sub != ODIN3_AST_BINARY_AND && sub != ODIN3_AST_BINARY_OR) ||
        odin3_ast_loc(ast, operand).v != odin3_ast_loc(ast, node).v) {
        return false;
    }
    pc_span left = pc_span_of(orc, odin3_ast_child(ast, operand, 0));
    if (!left.known || left.empty) {
        return false;
    }
    uint32_t i = left.last + 1;
    while (i < orc->ntok && pc_text_is(pc_tok_text(orc, i), ")")) {
        i++;
    }
    return i < orc->ntok &&
           pc_text_is(pc_tok_text(orc, i), sub == ODIN3_AST_BINARY_AND ? "~&" : "~|");
}

static odin3_ast_kind pc_parent_kind(const pc_oracle *orc, odin3_ast_id node) {
    return odin3_ast_kind_of(orc->read->ast, odin3_ast_parent(&orc->parents, node));
}

/* The kind's closing token applies to this node. */
static bool pc_close_applies(const pc_oracle *orc, odin3_ast_id node, const pc_rule *rule,
                             odin3_bytes first) {
    odin3_ast_kind parent = pc_parent_kind(orc, node);
    switch (rule->when) {
    case PC_CLOSE_ITEM:
        return parent != ODIN3_AST_LIST;
    case PC_CLOSE_STMT:
        return parent != ODIN3_AST_FOR && parent != ODIN3_AST_GEN_FOR;
    case PC_CLOSE_OPENED:
        return pc_text_in(first, rule->words);
    case PC_CLOSE_NAMED:
        return odin3_ast_name(orc->read->ast, node) != 0;
    default:
        return true;
    }
}

/* Where the node's children lie in the stream: from the earliest child's start to just after
 * the latest child's end, empty children (at a token boundary) included; any = some child. */
struct pc_kids {
    uint64_t from, to;
    bool any;
};

static pc_kids pc_kids_of(const pc_oracle *orc, odin3_ast_id node) {
    odin3_ast_span kids = odin3_ast_children(orc->read->ast, node);
    pc_kids out = {UINT64_MAX, 0, false};
    for (uint32_t i = 0; i < kids.n; i++) {
        pc_span span = kids.ids[i].v != 0 ? pc_span_of(orc, kids.ids[i]) : (pc_span){0};
        if (span.known) {
            out.from = span.from < out.from ? span.from : out.from;
            out.to = span.to > out.to ? span.to : out.to;
            out.any = true;
        }
    }
    return out;
}

static bool pc_starts_at_child(const pc_oracle *orc, odin3_ast_id node) {
    pc_kids kids = pc_kids_of(orc, node);
    return kids.any && kids.from == pc_span_of(orc, node).from;
}

/* The token the node must end at: NULL for no rule, "" for "exactly where its last child
 * ends", else the closing word (a LIST opened by '(' or '#' closes at ')'). */
static const char *pc_close_for(const pc_oracle *orc, odin3_ast_id node, odin3_bytes first) {
    const pc_rule *rule = &k_rules[odin3_ast_kind_of(orc->read->ast, node)];
    if (odin3_ast_kind_of(orc->read->ast, node) == ODIN3_AST_LIST) {
        return pc_text_in(first, rule->words) ? rule->close : "";
    }
    return rule->close != NULL && pc_close_applies(orc, node, rule, first) ? rule->close : NULL;
}

static bool pc_text_ends(odin3_bytes text, const char *suffix) {
    size_t len = strlen(suffix);
    return text.len >= len && memcmp((const uint8_t *)text.ptr + text.len - len, suffix, len) == 0;
}

/* The node's end: the kind's closer, and only closers after its last child. */
static void pc_check_end(pc_oracle *orc, odin3_ast_id node, const pc_span *span) {
    odin3_bytes last = pc_tok_text(orc, span->last);
    const char *close = pc_close_for(orc, node, pc_tok_text(orc, span->first));
    pc_kids kids = pc_kids_of(orc, node);
    if (close != NULL && close[0] == '\0' && kids.any && kids.to != span->to) {
        pc_fail(orc, node, "ends at '%.*s', not where its last child ends", (int)last.len,
                (const char *)last.ptr);
    } else if (close != NULL && close[0] != '\0' && !pc_text_is(last, close) &&
               !(span->first == span->last && pc_text_ends(last, close))) {
        pc_fail(orc, node, "ends at '%.*s', not at '%s'", (int)last.len, (const char *)last.ptr,
                close);
    }
    if (!kids.any || kids.to == 0) {
        return;
    }
    for (uint64_t i = (kids.to - 1) / 2 + 1; i <= span->last && i < orc->ntok; i++) {
        odin3_bytes text = pc_tok_text(orc, (uint32_t)i);
        if (!pc_text_in(text, k_closers)) {
            pc_fail(orc, node, "ends past its last child at '%.*s' (token %u)", (int)text.len,
                    (const char *)text.ptr, (unsigned)i);
            return;
        }
    }
}

/* The tokens that may precede a node's first child inside it (kinds without a leading word);
 * NULL when the kind has no left-edge rule. */
static const char *pc_openers(const pc_oracle *orc, odin3_ast_id node) {
    const odin3_ast *ast = orc->read->ast;
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_BINARY:
    case ODIN3_AST_SELECT:
    case ODIN3_AST_TERNARY:
    case ODIN3_AST_MINTYPMAX:
    case ODIN3_AST_HIER_NAME:
    case ODIN3_AST_NET_ASSIGN:
    case ODIN3_AST_BLOCKING_ASSIGN:
    case ODIN3_AST_NONBLOCKING_ASSIGN:
        return "(";
    case ODIN3_AST_CASE_ITEM:
        return "(|default|:";
    case ODIN3_AST_LIST:
        return "(|#";
    case ODIN3_AST_CONNECTION:
    case ODIN3_AST_PORT_REF:
        return odin3_ast_name(ast, node) == 0 ? "(" : NULL;
    default:
        return NULL;
    }
}

/* The node's start: only openers (and the first child's attributes) before its first child (a
 * node never takes a leading token of its parent's, such as '=' or 'assign'). */
static void pc_check_start(pc_oracle *orc, odin3_ast_id node, const pc_span *span) {
    const char *openers = pc_openers(orc, node);
    pc_kids kids = pc_kids_of(orc, node);
    if (openers == NULL || !kids.any) {
        return;
    }
    uint64_t stop = (kids.from + 1) / 2; /* the first child's first token */
    bool in_attr = false; /* the first child's own attributes, `(* … *)`, precede it (point (g)) */
    for (uint64_t i = span->first; i < stop && i < orc->ntok; i++) {
        odin3_bytes text = pc_tok_text(orc, (uint32_t)i);
        if (in_attr || pc_text_is(text, "(*")) {
            in_attr = !pc_text_is(text, "*)");
        } else if (!pc_text_in(text, openers)) {
            pc_fail(orc, node, "starts before its first child at '%.*s' (token %u)", (int)text.len,
                    (const char *)text.ptr, (unsigned)i);
            return;
        }
    }
}

/* Single-token kinds end at their first token's end. */
static bool pc_single_token(odin3_ast_kind kind) {
    return kind == ODIN3_AST_IDENT || kind == ODIN3_AST_STRING || kind == ODIN3_AST_REAL;
}

/* A metacomment's ATTR (§2.6) lies at its comment: the comment's loc and end. */
static void pc_check_metacomment(pc_oracle *orc, odin3_ast_id node) {
    odin3_range range = {odin3_ast_loc(orc->read->ast, node), odin3_ast_end(orc->read->ast, node)};
    odin3_ast_comment_span comments =
        odin3_ast_comments_of(orc->read->ast, odin3_srcman_buffer_of(orc->sm, range.loc));
    for (uint32_t i = 0; i < comments.n; i++) {
        const odin3_ast_comment *comment = &comments.items[i];
        if (comment->loc.v == range.loc.v && comment->end.v == range.end.v &&
            (comment->flags & ODIN3_AST_COMMENT_METACOMMENT) != 0) {
            return;
        }
    }
    pc_fail(orc, node, "is not at a metacomment");
}

static void pc_check_node(pc_oracle *orc, odin3_ast_id node) {
    const odin3_ast *ast = orc->read->ast;
    odin3_ast_kind kind = odin3_ast_kind_of(ast, node);
    if (kind == ODIN3_AST_UNIT) {
        return;
    }
    if (kind == ODIN3_AST_ATTR && (odin3_ast_flags(ast, node) & ODIN3_AST_F_ATTR_METACOMMENT)) {
        pc_check_metacomment(orc, node);
        return;
    }
    pc_span span = pc_span_of(orc, node);
    uint32_t i = 0;
    if (span.empty) {
        if (!span.known) {
            pc_fail(orc, node, "is empty but not at a token boundary");
        }
        return;
    }
    if (!pc_tok_at(orc->starts, odin3_ast_loc(ast, node), &i)) {
        pc_fail(orc, node, "does not start at a token");
        return;
    }
    odin3_bytes tok = pc_tok_text(orc, i);
    if (kind != ODIN3_AST_NULL_STMT && pc_text_in(tok, k_closers)) {
        pc_fail(orc, node, "starts at the closer '%.*s'", (int)tok.len, (const char *)tok.ptr);
    } else if (tok.len == 0 || !pc_spelled(orc, node, tok)) {
        pc_fail(orc, node, "starts at '%.*s', which does not spell it", (int)tok.len,
                (const char *)tok.ptr);
    }
    if (!span.known) {
        pc_fail(orc, node, "does not end at a token's end");
        return;
    }
    if (span.to < span.from) {
        pc_fail(orc, node, "ends before it starts");
        return;
    }
    if (pc_single_token(kind) && span.last != span.first) {
        pc_fail(orc, node, "spans %u tokens, not 1", (unsigned)(span.last - span.first + 1));
    }
    pc_check_start(orc, node, &span);
    pc_check_end(orc, node, &span);
}

/* --- containment ---------------------------------------------------------------------------- */

static bool pc_one_buffer(const pc_oracle *orc, odin3_range outer, odin3_range inner) {
    odin3_srcbuf_id buf = odin3_srcman_buffer_of(orc->sm, outer.loc);
    return buf.v != 0 && odin3_srcman_buffer_of(orc->sm, outer.end).v == buf.v &&
           odin3_srcman_buffer_of(orc->sm, inner.loc).v == buf.v &&
           odin3_srcman_buffer_of(orc->sm, inner.end).v == buf.v;
}

static odin3_range pc_range(const odin3_ast *ast, odin3_ast_id node) {
    return (odin3_range){odin3_ast_loc(ast, node), odin3_ast_end(ast, node)};
}

/* Every child's range lies inside its parent's: by loc in one buffer, by stream position. */
static void pc_check_children(pc_oracle *orc, odin3_ast_id node) {
    const odin3_ast *ast = orc->read->ast;
    odin3_range outer = pc_range(ast, node);
    bool unit = odin3_ast_kind_of(ast, node) == ODIN3_AST_UNIT;
    pc_span pspan = pc_span_of(orc, node);
    odin3_ast_span kids = odin3_ast_children(ast, node);
    for (uint32_t i = 0; i < kids.n; i++) {
        if (kids.ids[i].v == 0) {
            continue;
        }
        odin3_range inner = pc_range(ast, kids.ids[i]);
        if (pc_one_buffer(orc, outer, inner) &&
            (inner.loc.v < outer.loc.v || inner.end.v > outer.end.v)) {
            pc_fail(orc, kids.ids[i], "lies outside its parent (node %u) in one buffer",
                    (unsigned)node.v);
            continue;
        }
        pc_span cspan = pc_span_of(orc, kids.ids[i]);
        if (!unit && pspan.known && cspan.known &&
            (cspan.from < pspan.from || cspan.to > pspan.to)) {
            pc_fail(orc, kids.ids[i], "lies outside its parent (node %u) in the stream",
                    (unsigned)node.v);
        }
    }
}

odin3_status pc_loc_oracle(const pc_read *read, uint32_t *failures) {
    *failures = 0;
    pc_oracle orc;
    odin3_status st = pc_oracle_open(&orc, read);
    if (st == ODIN3_OK) {
        odin3_ast_id end = odin3_ast_node_end(read->ast);
        for (odin3_ast_id node = {1}; node.v < end.v; node.v++) {
            pc_check_node(&orc, node);
            pc_check_children(&orc, node);
        }
        *failures = orc.failures;
    }
    pc_oracle_close(&orc);
    return st;
}
