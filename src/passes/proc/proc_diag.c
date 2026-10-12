/*
 * proc_diag.c — proc's diagnostics (2E spec §15, PR-21; plan point (k)): errors and warnings
 * located at the first SOURCE or IMPORTED leaf of a provenance record, the error cap (E7), and
 * the names of signals.
 */
#include "passes/proc/proc_internal.h"

#include "ast/srcman.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/proc/proc.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* --- located messages ---------------------------------------------------------------------- */

/* odin3_prov_sources visitor: keeps the first leaf (the record that names the object). */
static void first_leaf(void *user, odin3_prov_id leaf) {
    odin3_prov_id *found = user;
    if (found->v == 0) {
        *found = leaf;
    }
}

/* The location of record at: its first leaf's first location, or none (all zero). */
static odin3_srcloc locate(const odin3_design *design, odin3_prov_id at) {
    odin3_srcloc none = {0, 0, 0, 0, 0, 0};
    odin3_prov_id leaf = {0};
    if (at.v == 0 || odin3_prov_get(design, at) == NULL ||
        odin3_prov_sources(design, at, first_leaf, &leaf) != ODIN3_OK) {
        return none; /* no record, or out of memory: unlocated */
    }
    const odin3_prov_record *rec = odin3_prov_get(design, leaf);
    return rec != NULL && rec->n_locs > 0 ? rec->locs[0] : none;
}

static const char *level_word(odin3_log_level level) {
    if (level == ODIN3_LOG_ERROR) {
        return "error";
    }
    return level == ODIN3_LOG_WARN ? "warning" : "info";
}

/* Logs msg at record at (point (k)): a source-manager location, else the record's file, line and
 * column, else unlocated with the module's name. */
static void log_at(const odin3_proc_mctx *mctx, odin3_log_level level, odin3_prov_id at,
                   const char *msg) {
    odin3_srcloc loc = locate(mctx->design, at);
    const odin3_strtab *tab = odin3_design_strtab(mctx->design);
    if (loc.loc != 0) {
        odin3_diag(mctx->design, level, (odin3_loc){loc.loc}, "%s", msg);
    } else if (loc.file != 0) {
        odin3_log(level, "%s:%" PRIu32 ":%" PRIu32 ": %s: %s", odin3_strtab_get(tab, loc.file),
                  loc.line, loc.col, level_word(level), msg);
    } else {
        odin3_log(level, "proc: module '%s': %s",
                  odin3_strtab_get(tab, odin3_module_name(mctx->module)), msg);
    }
}

static void vlog_at(const odin3_proc_mctx *mctx, odin3_log_level level, odin3_prov_id at,
                    const char *fmt, va_list args) {
    char msg[ODIN3_LOG_BUF];
    if (vsnprintf(msg, sizeof msg, fmt, args) < 0) {
        msg[0] = '\0';
    }
    log_at(mctx, level, at, msg);
}

void odin3_proc_error(odin3_proc_mctx *mctx, odin3_prov_id at, const char *fmt, ...) {
    if (mctx->stopped) {
        return;
    }
    mctx->errors++;
    uint32_t count = mctx->errors;
    if (mctx->stats != NULL) {
        count = ++mctx->stats->errors;
    }
    uint32_t cap = odin3_proc_limits_now().max_errors;
    if (count >= cap) {
        odin3_log(ODIN3_LOG_ERROR, "too many errors; proc stopped after %" PRIu32, cap);
        mctx->stopped = true;
        return;
    }
    va_list args;
    va_start(args, fmt);
    vlog_at(mctx, ODIN3_LOG_ERROR, at, fmt, args);
    va_end(args);
}

void odin3_proc_warn(odin3_proc_mctx *mctx, odin3_proc_warning kind, odin3_prov_id at,
                     const char *fmt, ...) {
    if (mctx->stats != NULL && kind < ODIN3_PROC_N_WARNINGS) {
        mctx->stats->warnings[kind]++;
    }
    va_list args;
    va_start(args, fmt);
    vlog_at(mctx, ODIN3_LOG_WARN, at, fmt, args);
    va_end(args);
}

/* --- names (§15) --------------------------------------------------------------------------- */

/* The root of bit's class without compressing the map (diagnostics read a const context). */
static odin3_pw_bit root_of(const odin3_proc_mctx *mctx, odin3_pw_bit bit) {
    const odin3_pw_bit *map = mctx->netmap.data;
    while ((bit & ODIN3_PW_CONST) == 0 && bit < mctx->netmap.len && map[bit] != 0) {
        bit = map[bit];
    }
    return bit;
}

static const char *str_of(const odin3_proc_mctx *mctx, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(mctx->design), str);
}

/* "w" for a one-bit wire, else "w[index]" in declared indices. */
static odin3_status put_wire_bit(const odin3_proc_mctx *mctx, odin3_wirebit wb, odin3_strbuf *out) {
    const char *name = str_of(mctx, odin3_wire_name(mctx->module, wb.wire));
    if (odin3_wire_width(mctx->module, wb.wire) == 1) {
        return odin3_strbuf_append(out, odin3_bytes_cstr(name));
    }
    return odin3_strbuf_appendf(out, "%s[%" PRId32 "]", name,
                                odin3_wire_index(mctx->module, wb.wire, wb.bit));
}

/* A net that is no named wire's primary bit: its name, else its first alias, else $n<ID>. */
static odin3_status put_net(const odin3_proc_mctx *mctx, odin3_net_id net, odin3_strbuf *out) {
    uint32_t name = odin3_net_name(mctx->module, net);
    if (name != 0) {
        return odin3_strbuf_append(out, odin3_bytes_cstr(str_of(mctx, name)));
    }
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (odin3_net_alias_next(mctx->module, net, &cursor, &alias)) {
        if (odin3_wire_valid(alias.wb.wire) && odin3_wire_name(mctx->module, alias.wb.wire) != 0) {
            return put_wire_bit(mctx, alias.wb, out);
        }
        if (!odin3_wire_valid(alias.wb.wire) && alias.name != 0) {
            return odin3_strbuf_append(out, odin3_bytes_cstr(str_of(mctx, alias.name)));
        }
    }
    return odin3_strbuf_appendf(out, "$n%" PRIu32, net.v);
}

/* The number of bits from bits[0] on that are consecutive bits of wb's wire, from wb.bit. */
static uint32_t run_on_wire(const odin3_proc_mctx *mctx, const odin3_pw_bit *bits, uint32_t n,
                            odin3_wirebit wb) {
    uint32_t width = odin3_wire_width(mctx->module, wb.wire);
    uint32_t len = 1;
    while (len < n && wb.bit + len < width &&
           root_of(mctx, bits[len]) == odin3_wire_net(mctx->module, wb.wire, wb.bit + len).v) {
        len++;
    }
    return len;
}

odin3_status odin3_proc_name_bits(const odin3_proc_mctx *mctx, const odin3_pw_bit *bits, uint32_t n,
                                  odin3_strbuf *out) {
    static const char *const k_consts[] = {"1'0", "1'1", "1'x", "1'z", "1'-", "1'm"};
    if (n == 0) {
        return ODIN3_OK;
    }
    odin3_pw_bit first = root_of(mctx, bits[0]);
    if ((first & ODIN3_PW_CONST) != 0) {
        uint32_t state = first & ~ODIN3_PW_CONST;
        return odin3_strbuf_append(
            out, odin3_bytes_cstr(state < sizeof k_consts / sizeof k_consts[0] ? k_consts[state]
                                                                               : "1'x"));
    }
    odin3_wirebit wb = odin3_net_primary(mctx->module, (odin3_net_id){first});
    if (!odin3_wire_valid(wb.wire) || odin3_wire_name(mctx->module, wb.wire) == 0) {
        return put_net(mctx, (odin3_net_id){first}, out);
    }
    uint32_t width = odin3_wire_width(mctx->module, wb.wire);
    uint32_t len = run_on_wire(mctx, bits, n, wb);
    const char *name = str_of(mctx, odin3_wire_name(mctx->module, wb.wire));
    if (len == n && wb.bit == 0 && n == width) {
        return odin3_strbuf_append(out, odin3_bytes_cstr(name));
    }
    if (len == n && n > 1) {
        return odin3_strbuf_appendf(out, "%s[%" PRId32 ":%" PRId32 "]", name,
                                    odin3_wire_index(mctx->module, wb.wire, wb.bit + n - 1),
                                    odin3_wire_index(mctx->module, wb.wire, wb.bit));
    }
    return put_wire_bit(mctx, wb, out);
}
