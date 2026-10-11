/*
 * keywords.c — the Verilog keyword table (2C spec §2.5, VP-6): the 1364-2005 Annex B words, each
 * with its token and the keyword sets it belongs to, sorted for bsearch; and the
 * `begin_keywords version strings. The three opaque-body openers (specify, primitive, config)
 * map to their OPAQUE_* tokens (§2.8).
 */
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "util/hash.h"
#include "verilog_parser.h" /* generated (build tree): TOK_* */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A word's sets: in every set (1364-1995 on); new in 1364-2001 (not the config words, so also
 * in 1364-2001-noconfig); the 1364-2001 config words; new in 1364-2005 (uwire). */
enum {
    VL_1995 = ODIN3_VL_KW_1995 | ODIN3_VL_KW_2001_NOCONFIG | ODIN3_VL_KW_2001 | ODIN3_VL_KW_2005,
    VL_2001 = ODIN3_VL_KW_2001_NOCONFIG | ODIN3_VL_KW_2001 | ODIN3_VL_KW_2005,
    VL_CONFIG = ODIN3_VL_KW_2001 | ODIN3_VL_KW_2005,
    VL_2005 = ODIN3_VL_KW_2005
};

/* X(word, token, sets), sorted bytewise by word (a unit test checks the order). */
#define ODIN3_VL_KEYWORDS(X)                                                                       \
    X("always", TOK_ALWAYS, VL_1995)                                                               \
    X("and", TOK_AND, VL_1995)                                                                     \
    X("assign", TOK_ASSIGN, VL_1995)                                                               \
    X("automatic", TOK_AUTOMATIC, VL_2001)                                                         \
    X("begin", TOK_BEGIN, VL_1995)                                                                 \
    X("buf", TOK_BUF, VL_1995)                                                                     \
    X("bufif0", TOK_BUFIF0, VL_1995)                                                               \
    X("bufif1", TOK_BUFIF1, VL_1995)                                                               \
    X("case", TOK_CASE, VL_1995)                                                                   \
    X("casex", TOK_CASEX, VL_1995)                                                                 \
    X("casez", TOK_CASEZ, VL_1995)                                                                 \
    X("cell", TOK_CELL, VL_CONFIG)                                                                 \
    X("cmos", TOK_CMOS, VL_1995)                                                                   \
    X("config", TOK_OPAQUE_CONFIG, VL_CONFIG)                                                      \
    X("deassign", TOK_DEASSIGN, VL_1995)                                                           \
    X("default", TOK_DEFAULT, VL_1995)                                                             \
    X("defparam", TOK_DEFPARAM, VL_1995)                                                           \
    X("design", TOK_DESIGN, VL_CONFIG)                                                             \
    X("disable", TOK_DISABLE, VL_1995)                                                             \
    X("edge", TOK_EDGE, VL_1995)                                                                   \
    X("else", TOK_ELSE, VL_1995)                                                                   \
    X("end", TOK_END, VL_1995)                                                                     \
    X("endcase", TOK_ENDCASE, VL_1995)                                                             \
    X("endconfig", TOK_ENDCONFIG, VL_CONFIG)                                                       \
    X("endfunction", TOK_ENDFUNCTION, VL_1995)                                                     \
    X("endgenerate", TOK_ENDGENERATE, VL_2001)                                                     \
    X("endmodule", TOK_ENDMODULE, VL_1995)                                                         \
    X("endprimitive", TOK_ENDPRIMITIVE, VL_1995)                                                   \
    X("endspecify", TOK_ENDSPECIFY, VL_1995)                                                       \
    X("endtable", TOK_ENDTABLE, VL_1995)                                                           \
    X("endtask", TOK_ENDTASK, VL_1995)                                                             \
    X("event", TOK_EVENT, VL_1995)                                                                 \
    X("for", TOK_FOR, VL_1995)                                                                     \
    X("force", TOK_FORCE, VL_1995)                                                                 \
    X("forever", TOK_FOREVER, VL_1995)                                                             \
    X("fork", TOK_FORK, VL_1995)                                                                   \
    X("function", TOK_FUNCTION, VL_1995)                                                           \
    X("generate", TOK_GENERATE, VL_2001)                                                           \
    X("genvar", TOK_GENVAR, VL_2001)                                                               \
    X("highz0", TOK_HIGHZ0, VL_1995)                                                               \
    X("highz1", TOK_HIGHZ1, VL_1995)                                                               \
    X("if", TOK_IF, VL_1995)                                                                       \
    X("ifnone", TOK_IFNONE, VL_1995)                                                               \
    X("incdir", TOK_INCDIR, VL_CONFIG)                                                             \
    X("include", TOK_INCLUDE, VL_CONFIG)                                                           \
    X("initial", TOK_INITIAL, VL_1995)                                                             \
    X("inout", TOK_INOUT, VL_1995)                                                                 \
    X("input", TOK_INPUT, VL_1995)                                                                 \
    X("instance", TOK_INSTANCE, VL_CONFIG)                                                         \
    X("integer", TOK_INTEGER, VL_1995)                                                             \
    X("join", TOK_JOIN, VL_1995)                                                                   \
    X("large", TOK_LARGE, VL_1995)                                                                 \
    X("liblist", TOK_LIBLIST, VL_CONFIG)                                                           \
    X("library", TOK_LIBRARY, VL_CONFIG)                                                           \
    X("localparam", TOK_LOCALPARAM, VL_2001)                                                       \
    X("macromodule", TOK_MACROMODULE, VL_1995)                                                     \
    X("medium", TOK_MEDIUM, VL_1995)                                                               \
    X("module", TOK_MODULE, VL_1995)                                                               \
    X("nand", TOK_KW_NAND, VL_1995)                                                                \
    X("negedge", TOK_NEGEDGE, VL_1995)                                                             \
    X("nmos", TOK_NMOS, VL_1995)                                                                   \
    X("nor", TOK_KW_NOR, VL_1995)                                                                  \
    X("noshowcancelled", TOK_NOSHOWCANCELLED, VL_2001)                                             \
    X("not", TOK_NOT, VL_1995)                                                                     \
    X("notif0", TOK_NOTIF0, VL_1995)                                                               \
    X("notif1", TOK_NOTIF1, VL_1995)                                                               \
    X("or", TOK_OR, VL_1995)                                                                       \
    X("output", TOK_OUTPUT, VL_1995)                                                               \
    X("parameter", TOK_PARAMETER, VL_1995)                                                         \
    X("pmos", TOK_PMOS, VL_1995)                                                                   \
    X("posedge", TOK_POSEDGE, VL_1995)                                                             \
    X("primitive", TOK_OPAQUE_PRIMITIVE, VL_1995)                                                  \
    X("pull0", TOK_PULL0, VL_1995)                                                                 \
    X("pull1", TOK_PULL1, VL_1995)                                                                 \
    X("pulldown", TOK_PULLDOWN, VL_1995)                                                           \
    X("pullup", TOK_PULLUP, VL_1995)                                                               \
    X("pulsestyle_ondetect", TOK_PULSESTYLE_ONDETECT, VL_2001)                                     \
    X("pulsestyle_onevent", TOK_PULSESTYLE_ONEVENT, VL_2001)                                       \
    X("rcmos", TOK_RCMOS, VL_1995)                                                                 \
    X("real", TOK_REAL, VL_1995)                                                                   \
    X("realtime", TOK_REALTIME, VL_1995)                                                           \
    X("reg", TOK_REG, VL_1995)                                                                     \
    X("release", TOK_RELEASE, VL_1995)                                                             \
    X("repeat", TOK_REPEAT, VL_1995)                                                               \
    X("rnmos", TOK_RNMOS, VL_1995)                                                                 \
    X("rpmos", TOK_RPMOS, VL_1995)                                                                 \
    X("rtran", TOK_RTRAN, VL_1995)                                                                 \
    X("rtranif0", TOK_RTRANIF0, VL_1995)                                                           \
    X("rtranif1", TOK_RTRANIF1, VL_1995)                                                           \
    X("scalared", TOK_SCALARED, VL_1995)                                                           \
    X("showcancelled", TOK_SHOWCANCELLED, VL_2001)                                                 \
    X("signed", TOK_SIGNED, VL_2001)                                                               \
    X("small", TOK_SMALL, VL_1995)                                                                 \
    X("specify", TOK_OPAQUE_SPECIFY, VL_1995)                                                      \
    X("specparam", TOK_SPECPARAM, VL_1995)                                                         \
    X("strong0", TOK_STRONG0, VL_1995)                                                             \
    X("strong1", TOK_STRONG1, VL_1995)                                                             \
    X("supply0", TOK_SUPPLY0, VL_1995)                                                             \
    X("supply1", TOK_SUPPLY1, VL_1995)                                                             \
    X("table", TOK_TABLE, VL_1995)                                                                 \
    X("task", TOK_TASK, VL_1995)                                                                   \
    X("time", TOK_TIME, VL_1995)                                                                   \
    X("tran", TOK_TRAN, VL_1995)                                                                   \
    X("tranif0", TOK_TRANIF0, VL_1995)                                                             \
    X("tranif1", TOK_TRANIF1, VL_1995)                                                             \
    X("tri", TOK_TRI, VL_1995)                                                                     \
    X("tri0", TOK_TRI0, VL_1995)                                                                   \
    X("tri1", TOK_TRI1, VL_1995)                                                                   \
    X("triand", TOK_TRIAND, VL_1995)                                                               \
    X("trior", TOK_TRIOR, VL_1995)                                                                 \
    X("trireg", TOK_TRIREG, VL_1995)                                                               \
    X("unsigned", TOK_UNSIGNED, VL_2001)                                                           \
    X("use", TOK_USE, VL_CONFIG)                                                                   \
    X("uwire", TOK_UWIRE, VL_2005)                                                                 \
    X("vectored", TOK_VECTORED, VL_1995)                                                           \
    X("wait", TOK_WAIT, VL_1995)                                                                   \
    X("wand", TOK_WAND, VL_1995)                                                                   \
    X("weak0", TOK_WEAK0, VL_1995)                                                                 \
    X("weak1", TOK_WEAK1, VL_1995)                                                                 \
    X("while", TOK_WHILE, VL_1995)                                                                 \
    X("wire", TOK_WIRE, VL_1995)                                                                   \
    X("wor", TOK_WOR, VL_1995)                                                                     \
    X("xnor", TOK_KW_XNOR, VL_1995)                                                                \
    X("xor", TOK_XOR, VL_1995)

#define VL_KEYWORD_ENTRY(word, token, mask) {word, token, mask},

static const odin3_vl_kw vl_keywords[] = {ODIN3_VL_KEYWORDS(VL_KEYWORD_ENTRY)};

enum { VL_KEYWORD_COUNT = sizeof vl_keywords / sizeof vl_keywords[0] };

const odin3_vl_kw *odin3_vl_keywords(size_t *count) {
    *count = VL_KEYWORD_COUNT;
    return vl_keywords;
}

/* bsearch order: the key's bytes against an entry's word, then the lengths. */
static int keyword_cmp(const void *lhs, const void *rhs) {
    const odin3_bytes *word = lhs;
    const char *name = ((const odin3_vl_kw *)rhs)->word;
    size_t len = strlen(name);
    size_t common = word->len < len ? word->len : len;
    int order = memcmp(word->ptr, name, common);
    if (order != 0) {
        return order;
    }
    return (word->len > len) - (word->len < len);
}

int odin3_vl_keyword(odin3_bytes word, uint32_t mask) {
    if (word.len == 0) {
        return 0;
    }
    const odin3_vl_kw *hit =
        bsearch(&word, vl_keywords, VL_KEYWORD_COUNT, sizeof vl_keywords[0], keyword_cmp);
    return hit != NULL && (hit->mask & mask) != 0 ? hit->token : 0;
}

bool odin3_vl_keyword_version(odin3_bytes quoted, uint32_t *mask) {
    static const struct {
        const char *text;
        uint32_t mask;
    } versions[] = {{"\"1364-1995\"", ODIN3_VL_KW_1995},
                    {"\"1364-2001\"", ODIN3_VL_KW_2001},
                    {"\"1364-2001-noconfig\"", ODIN3_VL_KW_2001_NOCONFIG},
                    {"\"1364-2005\"", ODIN3_VL_KW_2005}};
    for (size_t i = 0; i < sizeof versions / sizeof versions[0]; i++) {
        size_t len = strlen(versions[i].text);
        if (quoted.len == len && memcmp(quoted.ptr, versions[i].text, len) == 0) {
            *mask = versions[i].mask;
            return true;
        }
    }
    return false;
}
