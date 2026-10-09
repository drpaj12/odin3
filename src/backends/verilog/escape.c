/* escape.c — Verilog identifiers: simple, escaped (`\name `) or unwritable. */
#include "backends/verilog/writer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    FIRST_PRINTABLE = 0x21, /* '!' : escaped identifiers hold 0x21..0x7e */
    LAST_PRINTABLE = 0x7e,  /* '~' */
};

/* Verilog-2005 (IEEE 1364) and SystemVerilog-2017 (IEEE 1800 Annex B) reserved words, sorted
 * (strcmp order) for a binary search. */
static const char *const KEYWORDS[] = {
    "accept_on",
    "alias",
    "always",
    "always_comb",
    "always_ff",
    "always_latch",
    "and",
    "assert",
    "assign",
    "assume",
    "automatic",
    "before",
    "begin",
    "bind",
    "bins",
    "binsof",
    "bit",
    "break",
    "buf",
    "bufif0",
    "bufif1",
    "byte",
    "case",
    "casex",
    "casez",
    "cell",
    "chandle",
    "checker",
    "class",
    "clocking",
    "cmos",
    "config",
    "const",
    "constraint",
    "context",
    "continue",
    "cover",
    "covergroup",
    "coverpoint",
    "cross",
    "deassign",
    "default",
    "defparam",
    "design",
    "disable",
    "dist",
    "do",
    "edge",
    "else",
    "end",
    "endcase",
    "endchecker",
    "endclass",
    "endclocking",
    "endconfig",
    "endfunction",
    "endgenerate",
    "endgroup",
    "endinterface",
    "endmodule",
    "endpackage",
    "endprimitive",
    "endprogram",
    "endproperty",
    "endsequence",
    "endspecify",
    "endtable",
    "endtask",
    "enum",
    "event",
    "eventually",
    "expect",
    "export",
    "extends",
    "extern",
    "final",
    "first_match",
    "for",
    "force",
    "foreach",
    "forever",
    "fork",
    "forkjoin",
    "function",
    "generate",
    "genvar",
    "global",
    "highz0",
    "highz1",
    "if",
    "iff",
    "ifnone",
    "ignore_bins",
    "illegal_bins",
    "implements",
    "implies",
    "import",
    "incdir",
    "include",
    "initial",
    "inout",
    "input",
    "inside",
    "instance",
    "int",
    "integer",
    "interconnect",
    "interface",
    "intersect",
    "join",
    "join_any",
    "join_none",
    "large",
    "let",
    "liblist",
    "library",
    "local",
    "localparam",
    "logic",
    "longint",
    "macromodule",
    "matches",
    "medium",
    "modport",
    "module",
    "nand",
    "negedge",
    "nettype",
    "new",
    "nexttime",
    "nmos",
    "nor",
    "noshowcancelled",
    "not",
    "notif0",
    "notif1",
    "null",
    "or",
    "output",
    "package",
    "packed",
    "parameter",
    "pmos",
    "posedge",
    "primitive",
    "priority",
    "program",
    "property",
    "protected",
    "pull0",
    "pull1",
    "pulldown",
    "pullup",
    "pulsestyle_ondetect",
    "pulsestyle_onevent",
    "pure",
    "rand",
    "randc",
    "randcase",
    "randsequence",
    "rcmos",
    "real",
    "realtime",
    "ref",
    "reg",
    "reject_on",
    "release",
    "repeat",
    "restrict",
    "return",
    "rnmos",
    "rpmos",
    "rtran",
    "rtranif0",
    "rtranif1",
    "s_always",
    "s_eventually",
    "s_nexttime",
    "s_until",
    "s_until_with",
    "scalared",
    "sequence",
    "shortint",
    "shortreal",
    "showcancelled",
    "signed",
    "small",
    "soft",
    "solve",
    "specify",
    "specparam",
    "static",
    "string",
    "strong",
    "strong0",
    "strong1",
    "struct",
    "super",
    "supply0",
    "supply1",
    "sync_accept_on",
    "sync_reject_on",
    "table",
    "tagged",
    "task",
    "this",
    "throughout",
    "time",
    "timeprecision",
    "timeunit",
    "tran",
    "tranif0",
    "tranif1",
    "tri",
    "tri0",
    "tri1",
    "triand",
    "trior",
    "trireg",
    "type",
    "typedef",
    "union",
    "unique",
    "unique0",
    "unsigned",
    "until",
    "until_with",
    "untyped",
    "use",
    "uwire",
    "var",
    "vectored",
    "virtual",
    "void",
    "wait",
    "wait_order",
    "wand",
    "weak",
    "weak0",
    "weak1",
    "while",
    "wildcard",
    "wire",
    "with",
    "within",
    "wor",
    "xnor",
    "xor",
};

static bool is_letter(uint8_t chr) {
    return (chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z') || chr == '_';
}

static bool is_ident_char(uint8_t chr) {
    return is_letter(chr) || (chr >= '0' && chr <= '9') || chr == '$';
}

/* strcmp order of name against a keyword. */
static int compare_keyword(odin3_bytes name, const char *word) {
    size_t word_len = strlen(word);
    size_t common = name.len < word_len ? name.len : word_len;
    int cmp = memcmp(name.ptr, word, common);
    if (cmp != 0) {
        return cmp;
    }
    return (name.len > word_len) - (name.len < word_len);
}

bool odin3_verilog_is_keyword(odin3_bytes name) {
    size_t lo = 0;
    size_t hi = sizeof KEYWORDS / sizeof KEYWORDS[0];
    while (name.len > 0 && lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compare_keyword(name, KEYWORDS[mid]);
        if (cmp == 0) {
            return true;
        }
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return false;
}

odin3_verilog_ident odin3_verilog_ident_kind(odin3_bytes name) {
    const uint8_t *bytes = name.ptr;
    if (name.len == 0) {
        return ODIN3_VERILOG_UNWRITABLE;
    }
    bool simple = is_letter(bytes[0]);
    for (size_t i = 0; i < name.len; i++) {
        if (bytes[i] < FIRST_PRINTABLE || bytes[i] > LAST_PRINTABLE) {
            return ODIN3_VERILOG_UNWRITABLE;
        }
        simple = simple && is_ident_char(bytes[i]);
    }
    return simple && !odin3_verilog_is_keyword(name) ? ODIN3_VERILOG_SIMPLE : ODIN3_VERILOG_ESCAPED;
}

odin3_status odin3_verilog_append_ident(odin3_strbuf *buf, odin3_bytes name) {
    odin3_verilog_ident kind = odin3_verilog_ident_kind(name);
    if (kind == ODIN3_VERILOG_UNWRITABLE) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (kind == ODIN3_VERILOG_SIMPLE) {
        return odin3_strbuf_append(buf, name);
    }
    /* One append, so a failure leaves buf unchanged. */
    return odin3_strbuf_appendf(buf, "\\%.*s ", (int)name.len, (const char *)name.ptr);
}
