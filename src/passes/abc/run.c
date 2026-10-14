/*
 * run.c — the abc pass: one invocation, its start-up checks, the module and domain loop (3D spec
 * §7.1, §7.8, AB-19, AB-24; plan Task 14 with Tasks 15 and 16's pieces).
 */
#include "passes/abc/run.h"

#include "passes/abc/abc.h"
#include "passes/abc/engine.h"
#include "passes/abc/exchange.h"
#include "passes/abc/region.h"
#include "passes/abc/result.h"
#include "passes/abc/script.h"
#include "passes/abc/splice.h"
#include "passes/budget.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    ARG_ARENA_BYTES = 4096,
    FILE_NAME_BYTES = 32,
    DIR_PERMISSIONS = 0777,
    DOMAIN_TEXT_BYTES = 160,
    SHA_SHOWN = 16,
    COMMIT_SHOWN = 12,
    AIG_OBJ_WORD_BYTES = 8 /* the exchange arrays: one 8-byte entry per AIG object (point (r)) */
};

static const char *const k_file_suffix[] = {"in.blif", "cmd", "log", "out.blif"};

/* --- the run ------------------------------------------------------------------------------- */

odin3_status odin3_abc_run_create(odin3_pass_ctx *ctx, const odin3_abc_opts *opts,
                                  odin3_abc_run **out) {
    if (ctx == NULL || opts == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_abc_run_create: invalid argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_abc_run *run = odin3_util_calloc(sizeof *run);
    if (run == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    run->ctx = ctx;
    run->design = ctx->design;
    run->opts = *opts;
    run->arena = odin3_arena_create(0);
    run->gate_index = odin3_u64map_create(0);
    odin3_vec_init(&run->gates, sizeof(odin3_abc_gate));
    odin3_strbuf_init(&run->lines);
    if (run->arena == NULL || run->gate_index == NULL) {
        odin3_abc_run_destroy(run);
        return ODIN3_ERR_NO_MEMORY;
    }
    *out = run;
    return ODIN3_OK;
}

odin3_status odin3_abc_run_path(const odin3_abc_run *run, uint32_t file_kind, char *path,
                                size_t cap) {
    if (file_kind > ODIN3_ABC_FILE_OUT) {
        return ODIN3_ERR_INVALID_ARG;
    }
    int len = snprintf(path, cap, "%s/abc%" PRIu32 ".%s", run->dir, run->run_no,
                       k_file_suffix[file_kind]);
    return len > 0 && (size_t)len < cap ? ODIN3_OK : ODIN3_ERR_INVALID_ARG;
}

/* Removes the files a mkdtemp directory holds, then the directory (the pass made them all). */
static void remove_temp_dir(const odin3_abc_run *run) {
    for (uint32_t r = 0; r < run->run_no; r++) {
        for (uint32_t kind = ODIN3_ABC_FILE_IN; kind <= ODIN3_ABC_FILE_OUT; kind++) {
            char path[ODIN3_ABC_DIR_BYTES + FILE_NAME_BYTES];
            (void)snprintf(path, sizeof path, "%s/abc%" PRIu32 ".%s", run->dir, r,
                           k_file_suffix[kind]);
            (void)unlink(path);
        }
    }
    (void)rmdir(run->dir);
}

void odin3_abc_run_destroy(odin3_abc_run *run) {
    if (run == NULL) {
        return;
    }
    if (run->log != NULL) {
        (void)fclose(run->log);
    }
    if (run->runlog != NULL) {
        (void)fclose(run->runlog);
    }
    if (run->dir_is_temp && run->dir_made) {
        remove_temp_dir(run);
    }
    odin3_abc_script_free(&run->script);
    odin3_vec_free(&run->gates);
    odin3_u64map_destroy(run->gate_index);
    odin3_strbuf_free(&run->lines);
    odin3_arena_destroy(run->arena);
    odin3_util_free(run);
}

/* --- views --------------------------------------------------------------------------------- */

odin3_status odin3_abc_check_views(const odin3_design *design) {
    odin3_design *mut = (odin3_design *)design;
    for (uint32_t m = 1; m < odin3_design_module_end(design); m++) {
        const odin3_module *module = odin3_module_get(mut, (odin3_module_id){m});
        for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
            odin3_node_id node = {id};
            const odin3_celltype_def *def =
                odin3_node_live(module, node)
                    ? odin3_celltype_get(design, odin3_node_type(module, node))
                    : NULL;
            if (def != NULL && def->gran == ODIN3_GRAN_WORD) {
                char cell[192];
                odin3_abc_node_text(module, node, cell, sizeof cell);
                odin3_log(ODIN3_LOG_ERROR,
                          "abc: module '%s' is not in the netlist view: %s '%s'; run lower first",
                          odin3_strtab_get(odin3_design_strtab(design), odin3_module_name(module)),
                          def->name, cell);
                return ODIN3_ERR_CHECK;
            }
        }
    }
    return ODIN3_OK;
}

/* --- the exchange directory ---------------------------------------------------------------- */

/* True when path holds a byte the BLIF exchange cannot carry (I12). */
static bool dir_has_bad_byte(const char *path) {
    for (const unsigned char *c = (const unsigned char *)path; *c != '\0'; c++) {
        if (*c < 0x20U || *c == 0x7FU || *c == '"' || *c == '\'' || *c == ';' || *c == '#') {
            return true;
        }
    }
    return false;
}

static odin3_status refuse_dir(const char *dir) {
    odin3_log(ODIN3_LOG_ERROR,
              "abc: --via-blif directory '%s' contains a control character, a quote, ';' or '#'; "
              "ABC cannot read it",
              dir);
    return ODIN3_ERR_INVALID_ARG;
}

/* The path of the directory this invocation uses (absolute) into run->dir, bytes checked. */
static odin3_status choose_dir(odin3_abc_run *run) {
    char path[ODIN3_ABC_DIR_BYTES];
    if (run->opts.via_blif != NULL) {
        const char *abs = odin3_abc_resolve_cwd(run->arena, run->opts.via_blif);
        if (abs == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        (void)snprintf(path, sizeof path, "%s", abs);
    } else {
        const char *tmp = getenv("TMPDIR");
        (void)snprintf(path, sizeof path, "%s/odin3-abc-XXXXXX",
                       tmp != NULL && tmp[0] != '\0' ? tmp : "/tmp");
        run->dir_is_temp = true;
    }
    if (dir_has_bad_byte(path) || strlen(path) + FILE_NAME_BYTES >= sizeof path) {
        return refuse_dir(path);
    }
    (void)snprintf(run->dir, sizeof run->dir, "%s", path);
    return ODIN3_OK;
}

odin3_status odin3_abc_run_ensure_dir(odin3_abc_run *run) {
    if (run->dir_made) {
        return ODIN3_OK;
    }
    odin3_status st = choose_dir(run);
    if (st != ODIN3_OK) {
        return st;
    }
    bool made = run->dir_is_temp ? mkdtemp(run->dir) != NULL
                                 : (mkdir(run->dir, DIR_PERMISSIONS) == 0 || errno == EEXIST);
    if (!made) {
        odin3_log(ODIN3_LOG_ERROR, "abc: %s: cannot create the directory: %s", run->dir,
                  strerror(errno));
        run->dir_is_temp = false; /* nothing of ours to remove */
        return ODIN3_ERR_IO;
    }
    run->dir_made = true;
    return ODIN3_OK;
}

/* --- start-up ------------------------------------------------------------------------------ */

/* The checked script of the choice: the file, or the embedded default's bytes. */
static odin3_status load_script(odin3_abc_run *run) {
    odin3_abc_script_src src = {run->choice.given, run->choice.resolved, {NULL, 0}};
    if (run->choice.source == ODIN3_ABC_SRC_BUILTIN) {
        src.text = (odin3_bytes){odin3_abc_default_script, odin3_abc_default_script_len};
        src.resolved = NULL;
    }
    odin3_status st = odin3_abc_script_parse(&src, run->opts.lut_k, run->exchange, &run->script);
    if (st == ODIN3_OK && !odin3_abc_hex_digest(run->script.sha256_hex, run->digest)) {
        st = ODIN3_ERR_INVALID_ARG;
    }
    return st;
}

static odin3_status open_log(odin3_abc_run *run) {
    if (run->opts.log == NULL) {
        return ODIN3_OK;
    }
    run->log = fopen(run->opts.log, "w");
    if (run->log == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "abc: %s: cannot open the log: %s", run->opts.log,
                  strerror(errno));
        return ODIN3_ERR_IO;
    }
    return ODIN3_OK;
}

/* The --via-blif directory is created at start-up (so an IO error comes before any IR change);
 * the mkdtemp one only when the first run needs it. */
static odin3_status make_given_dir(odin3_abc_run *run) {
    return run->opts.via_blif != NULL ? odin3_abc_run_ensure_dir(run) : ODIN3_OK;
}

static void log_start(const odin3_abc_run *run) {
    char k_text[16];
    if (run->opts.lut_k != 0) {
        (void)snprintf(k_text, sizeof k_text, "%u", (unsigned)run->opts.lut_k);
    } else {
        (void)snprintf(k_text, sizeof k_text, "none");
    }
    odin3_log(ODIN3_LOG_INFO,
              "abc: script %s (%s), sha256 %.*s\xE2\x80\xA6, K=%s, exchange %s, ABC %.*s",
              run->choice.given, run->choice.resolved, SHA_SHOWN, run->script.sha256_hex, k_text,
              run->exchange == ODIN3_ABC_EXCHANGE_AIG ? "aig" : "blif", COMMIT_SHOWN,
              run->engine->abc_commit);
}

/* Steps 3 to 7 of the start-up order; step 1 and 2 are in odin3_abc_pass_run. */
static odin3_status startup_checks(odin3_abc_run *run) {
    odin3_status st = odin3_abc_exchange_choice(&run->opts, &run->exchange);
    st = st == ODIN3_OK ? odin3_abc_choose_script(run->design, &run->opts, run->arena, &run->choice)
                        : st;
    st = st == ODIN3_OK ? load_script(run) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    run->engine = odin3_abc_engine_get();
    if (run->engine == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "abc: linked ABC is not available (no engine registered; build "
                                   "with ODIN3_WITH_ABC=ON)");
        return ODIN3_ERR_ENGINE;
    }
    run->route = odin3_abc_route(run->digest);
    if (run->route == ODIN3_ABC_ROUTE_CHILD) {
        run->exchange = ODIN3_ABC_EXCHANGE_BLIF; /* the subprocess always uses the BLIF exchange */
    }
    if (run->exchange == ODIN3_ABC_EXCHANGE_AIG) {
        odin3_log(ODIN3_LOG_ERROR, "abc: the in-memory exchange is not available yet; use "
                                   "--via-blif or unset ODIN3_ABC_EXCHANGE");
        return ODIN3_ERR_INVALID_ARG;
    }
    st = odin3_abc_check_views(run->design);
    st = st == ODIN3_OK ? open_log(run) : st;
    st = st == ODIN3_OK ? make_given_dir(run) : st;
    if (st == ODIN3_OK) {
        log_start(run);
    }
    return st;
}

/* --- the loop ------------------------------------------------------------------------------ */

static const char *module_text(const odin3_abc_run *run) {
    return odin3_strtab_get(odin3_design_strtab(run->design), odin3_module_name(run->module));
}

/* The once-per-module INFO lines (§11): cut points and x/z/unconn reads. */
static void log_module_info(const odin3_abc_run *run, const odin3_abc_region *region) {
    const uint32_t *cut = region->cut_counts;
    if (cut[0] + cut[1] + cut[2] + cut[3] + cut[4] != 0) {
        odin3_log(ODIN3_LOG_INFO,
                  "abc: module '%s': cut points: %u hard or black-box cells, %u instances, %u "
                  "level latches, %u tri-state or bus cells, %u other cells",
                  module_text(run), (unsigned)cut[ODIN3_ABC_CUT_HARD],
                  (unsigned)cut[ODIN3_ABC_CUT_INSTANCE], (unsigned)cut[ODIN3_ABC_CUT_LATCH],
                  (unsigned)cut[ODIN3_ABC_CUT_TRISTATE], (unsigned)cut[ODIN3_ABC_CUT_OTHER]);
    }
    if (region->zero_pins != 0) {
        odin3_log(ODIN3_LOG_INFO, "abc: module '%s': %u soft-logic pins read x, z or unconn as 0",
                  module_text(run), (unsigned)region->zero_pins);
    }
}

/* The byte budget and the AIG cap, before the engine opens (point (r), §7.8). */
static odin3_status check_caps(odin3_abc_run *run, const odin3_abc_region *region) {
    uint64_t per_obj = ODIN3_ABC_BYTES_PER_AIG_OBJ + AIG_OBJ_WORD_BYTES;
    uint64_t extra = region->objs_est > UINT64_MAX / per_obj ? UINT64_MAX : region->objs_est * per_obj;
    uint32_t cap = odin3_abc_run_limits_get()->aig_objs;
    if (region->objs_est > cap) {
        char domain[DOMAIN_TEXT_BYTES];
        odin3_abc_domain_text(run->module, run->domain, domain, sizeof domain);
        odin3_log(ODIN3_LOG_ERROR, "abc: module '%s', %s: the AIG needs more than %u objects",
                  module_text(run), domain, (unsigned)cap);
        return ODIN3_ERR_PARSE;
    }
    odin3_status st = odin3_budget_check(run->ctx, run->design, extra);
    if (st == ODIN3_ERR_PARSE) {
        odin3_strbuf msg;
        odin3_strbuf_init(&msg);
        if (odin3_budget_message(run->ctx, &msg) == ODIN3_OK) {
            odin3_log(ODIN3_LOG_ERROR, "%s", msg.data);
        }
        odin3_strbuf_free(&msg);
    }
    return st;
}

/* One run: the region, the caps, the exchange, folding and the splice. */
static odin3_status run_domain(odin3_abc_run *run, const odin3_abc_domain *domain, bool first) {
    odin3_abc_region region;
    run->domain = domain;
    odin3_status st = odin3_abc_region_build(run, domain, &region);
    if (st != ODIN3_OK) {
        return st;
    }
    run->region = &region;
    if (first) {
        log_module_info(run, &region);
    }
    if (region.soft.len == 0 && region.regs.len == 0) {
        odin3_log(ODIN3_LOG_DEBUG, "abc: module '%s': nothing to map", module_text(run));
        run->region = NULL;
        odin3_abc_region_free(&region);
        return ODIN3_OK;
    }
    odin3_abc_result result;
    odin3_abc_result_init(&result);
    st = check_caps(run, &region);
    st = st == ODIN3_OK ? odin3_abc_exchange_blif(run, &result) : st;
    st = st == ODIN3_OK ? odin3_abc_result_fold(&result) : st;
    st = st == ODIN3_OK ? odin3_abc_splice(run, &region, &result) : st;
    odin3_abc_result_free(&result);
    run->region = NULL;
    odin3_abc_region_free(&region);
    return st;
}

static odin3_status run_module(odin3_abc_run *run, uint32_t id) {
    run->module_id = (odin3_module_id){id};
    run->module = odin3_module_get(run->design, run->module_id);
    odin3_vec domains;
    odin3_status st = odin3_abc_domains(run, run->module, &domains);
    for (size_t d = 0; st == ODIN3_OK && d < domains.len; d++) {
        st = run_domain(run, odin3_vec_at(&domains, d), d == 0);
    }
    odin3_abc_domains_free(&domains);
    return st;
}

static odin3_status run_all(odin3_abc_run *run) {
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_design_module_end(run->design); id++) {
        st = run_module(run, id);
    }
    return st;
}

/* --- the pass ------------------------------------------------------------------------------ */

odin3_status odin3_abc_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                void *user) {
    (void)user;
    (void)design;
    odin3_arena *arg_arena = odin3_arena_create(ARG_ARENA_BYTES);
    if (arg_arena == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_abc_opts opts;
    odin3_abc_run *run = NULL;
    odin3_status st = odin3_abc_parse_args(args, arg_arena, &opts);
    st = st == ODIN3_OK ? odin3_abc_run_create(ctx, &opts, &run) : st;
    st = st == ODIN3_OK ? startup_checks(run) : st;
    st = st == ODIN3_OK ? run_all(run) : st;
    if (st == ODIN3_OK) {
        st = odin3_design_compact(ctx, ctx->design);
    }
    odin3_abc_run_destroy(run);
    odin3_arena_destroy(arg_arena);
    return st;
}

const odin3_pass_def odin3_pass_abc = {
    "abc",
    "abc [--script FILE] [--lut K] [--via-blif DIR] [--log FILE] [--prov fine|cone]: map each "
    "module's soft logic to K-input LUTs with linked ABC (one run per clock domain)",
    odin3_abc_pass_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
