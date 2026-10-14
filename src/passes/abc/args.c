/*
 * args.c — the abc pass's arguments, script precedence and exchange choice (3D spec §7.1, §7.2,
 * AB-19, AB-20; plan Task 14 and point (z)).
 */
#include "frontends/project/paths.h"
#include "frontends/project/project.h"
#include "ir/lut.h"
#include "passes/abc/abc.h"
#include "passes/abc/run.h"
#include "passes/abc/script.h"
#include "passes/manager.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { LUT_DECIMAL_BASE = 10, CWD_BYTES = 4096 };

/* --- arguments ------------------------------------------------------------------------------ */

/* A NUL-terminated copy of word in arena; NULL on out of memory. */
static const char *copy_word(odin3_arena *arena, odin3_bytes word) {
    return odin3_arena_strndup(arena, word.ptr == NULL ? "" : (const char *)word.ptr, word.len);
}

/* Which option a word names, as an index into k_options, or -1. */
typedef enum abc_option { OPT_SCRIPT, OPT_LUT, OPT_VIA_BLIF, OPT_LOG, OPT_PROV, OPT_COUNT } abc_option;

static const char *const k_option_names[OPT_COUNT] = {"--script", "--lut", "--via-blif", "--log",
                                                      "--prov"};

static int option_of(odin3_bytes word) {
    for (int i = 0; i < OPT_COUNT; i++) {
        if (odin3_pass_arg_is(word, k_option_names[i])) {
            return i;
        }
    }
    return -1;
}

/* `--lut`'s value: digits only, 2 .. ODIN3_LUT_MAX_WIDTH. */
static bool parse_lut(const char *text, uint32_t *k) {
    if (text[0] == '\0' || strlen(text) > 3) {
        return false;
    }
    uint32_t value = 0;
    for (const char *c = text; *c != '\0'; c++) {
        if (*c < '0' || *c > '9') {
            return false;
        }
        value = value * LUT_DECIMAL_BASE + (uint32_t)(*c - '0');
    }
    *k = value;
    return value >= ODIN3_ABC_LUT_MIN && value <= ODIN3_LUT_MAX_WIDTH;
}

/* One option's value (already copied): the field it sets, or a logged INVALID_ARG. */
static odin3_status set_option(abc_option which, const char *value, odin3_abc_opts *out) {
    switch (which) {
    case OPT_SCRIPT:
        out->script = value;
        return ODIN3_OK;
    case OPT_VIA_BLIF:
        out->via_blif = value;
        return ODIN3_OK;
    case OPT_LOG:
        out->log = value;
        return ODIN3_OK;
    case OPT_LUT:
        if (!parse_lut(value, &out->lut_k)) {
            odin3_log(ODIN3_LOG_ERROR, "abc: --lut must be an integer from 2 to 16, not '%s'",
                      value);
            return ODIN3_ERR_INVALID_ARG;
        }
        return ODIN3_OK;
    default:
        break;
    }
    if (strcmp(value, "fine") == 0) {
        out->prov = ODIN3_ABC_PROV_FINE;
    } else if (strcmp(value, "cone") == 0) {
        out->prov = ODIN3_ABC_PROV_CONE;
    } else {
        odin3_log(ODIN3_LOG_ERROR, "abc: --prov must be fine or cone, not '%s'", value);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

odin3_status odin3_abc_parse_args(odin3_bytes args, odin3_arena *arena, odin3_abc_opts *out) {
    *out = (odin3_abc_opts){0};
    bool seen[OPT_COUNT] = {false};
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        int which = option_of(word);
        if (which < 0) {
            odin3_log(ODIN3_LOG_ERROR, "abc: unknown argument '%.*s'", (int)word.len,
                      (const char *)word.ptr);
            return ODIN3_ERR_INVALID_ARG;
        }
        if (seen[which]) {
            odin3_log(ODIN3_LOG_ERROR, "abc: %s given twice", k_option_names[which]);
            return ODIN3_ERR_INVALID_ARG;
        }
        seen[which] = true;
        odin3_bytes value = odin3_pass_arg_next(&args);
        if (value.ptr == NULL) {
            odin3_log(ODIN3_LOG_ERROR, "abc: %s needs a value", k_option_names[which]);
            return ODIN3_ERR_INVALID_ARG;
        }
        const char *text = copy_word(arena, value);
        if (text == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        odin3_status st = set_option((abc_option)which, text, out);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- script precedence --------------------------------------------------------------------- */

const char *odin3_abc_resolve_cwd(odin3_arena *arena, const char *path) {
    char cwd[CWD_BYTES];
    const char *base = getcwd(cwd, sizeof cwd) != NULL ? cwd : "";
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    odin3_path_join join = {odin3_bytes_cstr(base), odin3_bytes_cstr(path)};
    const char *out = NULL;
    if (odin3_path_resolve(join, &buf) == ODIN3_OK) {
        out = odin3_arena_strndup(arena, buf.data == NULL ? "" : buf.data, buf.len);
    }
    odin3_strbuf_free(&buf);
    return out;
}

static const char *strtab_text(const odin3_design *design, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(design), str);
}

odin3_status odin3_abc_choose_script(const odin3_design *design, const odin3_abc_opts *opts,
                                     odin3_arena *arena, odin3_abc_script_choice *out) {
    *out = (odin3_abc_script_choice){0};
    const char *cli = odin3_pass_get_options().abc_script;
    const odin3_project *proj = odin3_design_project(design);
    if (opts->script != NULL) {
        out->given = opts->script;
        out->source = ODIN3_ABC_SRC_PASS;
        out->resolved = odin3_abc_resolve_cwd(arena, opts->script);
    } else if (proj != NULL && proj->abc_script.given != 0) {
        out->given = strtab_text(design, proj->abc_script.given);
        out->resolved = strtab_text(design, proj->abc_script.resolved);
        out->source = ODIN3_ABC_SRC_PROJECT;
    } else if (cli != NULL && cli[0] != '\0') {
        out->given = cli;
        out->source = ODIN3_ABC_SRC_CLI;
        out->resolved = odin3_abc_resolve_cwd(arena, cli);
    } else {
        out->given = ODIN3_ABC_DEFAULT_SCRIPT_NAME;
        out->resolved = "embedded";
        out->source = ODIN3_ABC_SRC_BUILTIN;
    }
    return out->given != NULL && out->resolved != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* --- exchange and digest ------------------------------------------------------------------- */

odin3_status odin3_abc_exchange_choice(const odin3_abc_opts *opts, uint32_t *exchange) {
    *exchange = ODIN3_ABC_EXCHANGE_BLIF;
    const char *env = getenv("ODIN3_ABC_EXCHANGE");
    if (opts->via_blif != NULL || env == NULL) {
        return ODIN3_OK;
    }
    if (strcmp(env, "aig") == 0) {
        *exchange = ODIN3_ABC_EXCHANGE_AIG;
    } else if (strcmp(env, "blif") != 0) {
        odin3_log(ODIN3_LOG_ERROR, "abc: ODIN3_ABC_EXCHANGE must be aig or blif, not '%s'", env);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

static int hex_value(char chr) {
    if (chr >= '0' && chr <= '9') {
        return chr - '0';
    }
    return chr >= 'a' && chr <= 'f' ? chr - 'a' + LUT_DECIMAL_BASE : -1;
}

bool odin3_abc_hex_digest(const char *hex, unsigned char digest[ODIN3_SHA256_DIGEST_BYTES]) {
    if (hex == NULL || strlen(hex) != 2U * ODIN3_SHA256_DIGEST_BYTES) {
        return false;
    }
    unsigned char out[ODIN3_SHA256_DIGEST_BYTES];
    for (uint32_t i = 0; i < ODIN3_SHA256_DIGEST_BYTES; i++) {
        int hi = hex_value(hex[2U * i]);
        int lo = hex_value(hex[2U * i + 1U]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (unsigned char)(hi * 16 + lo);
    }
    memcpy(digest, out, sizeof out);
    return true;
}
