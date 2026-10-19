/*
 * exchange.c — the BLIF exchange of one run: files, ABC, read-back (3D spec §6.2, §7.6, §7.9, §11;
 * plan Task 16 with the 3D-5a carry-overs: a child that exits 0 may still have failed, so the
 * output file and the log decide, never the exit status).
 */
#include "passes/abc/exchange.h"

#include "passes/abc/abc.h"
#include "passes/abc/aig.h"
#include "passes/abc/aligned.h"
#include "passes/abc/child.h"
#include "passes/abc/region.h"
#include "passes/abc/result.h"
#include "passes/abc/run.h"
#include "passes/abc/script.h"
#include "passes/abc/xblif.h"
#include "passes/builtin.h"
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

enum {
    PATH_BYTES = ODIN3_ABC_DIR_BYTES + 64,
    CMD_WORD_BYTES = 64,
    DOMAIN_TEXT_BYTES = 160,
    FIRST_OBJ_NUMBER = 2 /* result and MiniLUT objects 0 and 1 are the constants */
};

/* --- ABC's output lines -------------------------------------------------------------------- */

/* True when the command is a printing command: its output is shown at INFO. */
static bool is_print_command(const char *line) {
    char word[CMD_WORD_BYTES];
    size_t len = strcspn(line, " \t");
    if (len == 0 || len >= sizeof word) {
        return false;
    }
    memcpy(word, line, len);
    word[len] = '\0';
    const odin3_abc_command_info *info = odin3_abc_command_find(word);
    return info != NULL && info->cls == ODIN3_ABC_CLASS_PRINT;
}

/* One output line to the run's log file, --log and the pass log. */
static void emit_line(odin3_abc_run *run, const char *line) {
    if (line[0] != '\0') {
        (void)snprintf(run->last_line, sizeof run->last_line, "%.*s",
                       (int)sizeof run->last_line - 1, line);
    }
    if (run->runlog != NULL) {
        (void)fprintf(run->runlog, "%s\n", line);
    }
    if (run->log != NULL) {
        (void)fprintf(run->log, "%s\n", line);
    }
    odin3_log(run->cmd_class == ODIN3_ABC_CLASS_PRINT ? ODIN3_LOG_INFO : ODIN3_LOG_DEBUG, "abc| %s",
              line);
}

/* Logs the buffered lines (the linked engine's call has returned) and clears the buffer. */
static void flush_lines(odin3_abc_run *run) {
    const char *at = run->lines.data;
    const char *end = at == NULL ? NULL : at + run->lines.len;
    while (at != NULL && at < end) {
        const char *nl = memchr(at, '\n', (size_t)(end - at));
        size_t len = nl == NULL ? (size_t)(end - at) : (size_t)(nl - at);
        char line[sizeof run->last_line * 4];
        size_t cut = len < sizeof line - 1U ? len : sizeof line - 1U;
        memcpy(line, at, cut);
        line[cut] = '\0';
        emit_line(run, line);
        at += len + 1U;
    }
    odin3_strbuf_clear(&run->lines);
    if (run->runlog != NULL) {
        (void)fflush(run->runlog);
    }
    if (run->log != NULL) {
        (void)fflush(run->log);
    }
}

/* The engine's and the child's text callback: the linked engine's lines wait for flush_lines (the
 * core never logs during an engine call); the child's are handled as they arrive. */
static void text_callback(void *user, const char *line) {
    odin3_abc_run *run = user;
    if (run->streaming) {
        emit_line(run, line);
        return;
    }
    if (odin3_strbuf_append(&run->lines, odin3_bytes_cstr(line)) != ODIN3_OK ||
        odin3_strbuf_append(&run->lines, odin3_bytes_cstr("\n")) != ODIN3_OK) {
        run->lines_overflow = true;
    }
}

/* --- failures ------------------------------------------------------------------------------ */

/* Logs the §11 text of a failed engine call; always ENGINE. src is the script command or NULL. */
static odin3_status report_fail(const odin3_abc_run *run, const odin3_abc_fail *fail,
                                const char *cmd, const odin3_abc_cmd *src) {
    static const char k_disabled[] = "linked ABC is disabled until odin3 restarts";
    switch (fail->kind) {
    case ODIN3_ABC_FAIL_EXIT:
        odin3_log(ODIN3_LOG_ERROR, "abc: ABC called exit(%d) during '%s'; %s", (int)fail->code, cmd,
                  k_disabled);
        break;
    case ODIN3_ABC_FAIL_ABORT:
        odin3_log(ODIN3_LOG_ERROR, "abc: ABC aborted during '%s'; %s", cmd, k_disabled);
        break;
    case ODIN3_ABC_FAIL_ASSERT:
        odin3_log(ODIN3_LOG_ERROR, "abc: ABC assertion failed during '%s': %s; %s", cmd, fail->text,
                  k_disabled);
        break;
    case ODIN3_ABC_FAIL_DISABLED:
        odin3_log(ODIN3_LOG_ERROR, "abc: linked ABC is disabled by an earlier failure in this "
                                   "process");
        break;
    case ODIN3_ABC_FAIL_BUSY:
        odin3_log(ODIN3_LOG_ERROR, "abc: linked ABC is busy (another session is open)");
        break;
    case ODIN3_ABC_FAIL_RESULT:
        odin3_log(ODIN3_LOG_ERROR, "abc: ABC returned a malformed network: %s", fail->text);
        break;
    default:
        if (src != NULL) {
            odin3_log(ODIN3_LOG_ERROR, "abc: %s:%u: command '%s' failed%s%s%s", run->choice.given,
                      (unsigned)src->src_line, cmd, src->alias != NULL ? " (from alias '" : "",
                      src->alias != NULL ? src->alias : "", src->alias != NULL ? "')" : "");
        } else {
            odin3_log(ODIN3_LOG_ERROR, "abc: internal command '%s' failed", cmd);
        }
        break;
    }
    return ODIN3_ERR_ENGINE;
}

/* --- files --------------------------------------------------------------------------------- */

/* The three file names of the current run. */
typedef struct xfiles {
    char in[PATH_BYTES];
    char cmd[PATH_BYTES];
    char out[PATH_BYTES];
} xfiles;

static odin3_status run_paths(const odin3_abc_run *run, xfiles *files) {
    odin3_status st = odin3_abc_run_path(run, ODIN3_ABC_FILE_IN, files->in, PATH_BYTES);
    st = st == ODIN3_OK ? odin3_abc_run_path(run, ODIN3_ABC_FILE_CMD, files->cmd, PATH_BYTES) : st;
    return st == ODIN3_OK ? odin3_abc_run_path(run, ODIN3_ABC_FILE_OUT, files->out, PATH_BYTES)
                          : st;
}

/* Writes in.blif and the command file. */
static odin3_status write_inputs(odin3_abc_run *run, const xfiles *files) {
    odin3_status st = odin3_abc_xblif_write(run, run->region, files->in);
    odin3_strbuf cmds;
    odin3_strbuf_init(&cmds);
    st = st == ODIN3_OK
             ? odin3_abc_xblif_commands(run, &(odin3_abc_paths){files->in, files->out}, &cmds)
             : st;
    st = st == ODIN3_OK ? odin3_pass_write_text(files->cmd, &cmds) : st;
    odin3_strbuf_free(&cmds);
    return st;
}

static odin3_status open_run_log(odin3_abc_run *run) {
    char path[PATH_BYTES];
    odin3_status st = odin3_abc_run_path(run, ODIN3_ABC_FILE_LOG, path, sizeof path);
    if (st != ODIN3_OK) {
        return st;
    }
    run->runlog = fopen(path, "w");
    if (run->runlog == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "abc: %s: cannot create the file: %s", path, strerror(errno));
        return ODIN3_ERR_IO;
    }
    return ODIN3_OK;
}

static void close_run_log(odin3_abc_run *run) {
    if (run->runlog != NULL) {
        (void)fclose(run->runlog);
        run->runlog = NULL;
    }
}

/* --- the linked engine --------------------------------------------------------------------- */

/* One command line through exec; a failure is reported. src is the script command, or NULL. */
static odin3_status exec_line(odin3_abc_run *run, const char *line, const odin3_abc_cmd *src) {
    const odin3_abc_engine *eng = run->engine;
    odin3_abc_fail fail = {0};
    run->cmd_class =
        src != NULL && is_print_command(src->line) ? ODIN3_ABC_CLASS_PRINT : ODIN3_ABC_CLASS_OTHER;
    odin3_status st = eng->exec(eng->self, line, &fail);
    flush_lines(run);
    return st == ODIN3_ERR_ENGINE ? report_fail(run, &fail, line, src) : st;
}

/* Every command of the run through exec; the first failure is reported. */
static odin3_status exec_commands(odin3_abc_run *run, const xfiles *files) {
    const odin3_abc_paths paths = {files->in, files->out};
    odin3_strbuf line;
    odin3_strbuf_init(&line);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < odin3_abc_xblif_command_count(run); i++) {
        const odin3_abc_cmd *src = NULL;
        odin3_strbuf_clear(&line);
        st = odin3_abc_xblif_command(run, i, &paths, &line, &src);
        st = st == ODIN3_OK ? exec_line(run, line.data, src) : st;
    }
    odin3_strbuf_free(&line);
    return st;
}

static odin3_status run_linked(odin3_abc_run *run, const xfiles *files) {
    const odin3_abc_engine *eng = run->engine;
    odin3_abc_fail fail = {0};
    run->streaming = false;
    odin3_status st = eng->open(eng->self, text_callback, run, &fail);
    flush_lines(run);
    if (st == ODIN3_ERR_ENGINE) {
        return report_fail(run, &fail, "open", NULL);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    st = exec_commands(run, files);
    eng->close(eng->self);
    flush_lines(run);
    return st;
}

/* --- the child ----------------------------------------------------------------------------- */

static odin3_status run_child(odin3_abc_run *run, const char *cmd_file) {
    char program[ODIN3_ABC_CHILD_PATH_BYTES];
    odin3_abc_child_res res;
    memset(&res, 0, sizeof res);
    odin3_status st = odin3_abc_child_program(program, sizeof program);
    res.run = run->run_no;
    if (st == ODIN3_OK) {
        st = odin3_abc_child_check_pin(program, &res);
    }
    if (st == ODIN3_OK) {
        odin3_abc_child_req req = {program, "-f",        cmd_file,      run->dir, 0,
                                   0,       run->run_no, text_callback, run};
        run->streaming = true;
        run->cmd_class = ODIN3_ABC_CLASS_OTHER;
        st = odin3_abc_child_run(&req, &res);
        run->streaming = false;
    }
    if (st == ODIN3_ERR_ENGINE) {
        char text[ODIN3_ABC_CHILD_MESSAGE_BYTES];
        if (odin3_abc_child_message(&res, text, sizeof text) == ODIN3_OK) {
            odin3_log(ODIN3_LOG_ERROR, "%s", text);
        }
    } else if (st == ODIN3_ERR_IO) {
        odin3_log(ODIN3_LOG_ERROR, "abc: cannot find the ABC subprocess program");
    }
    return st;
}

/* --- the exchange -------------------------------------------------------------------------- */

static void log_header(odin3_abc_run *run) {
    if (run->log == NULL) {
        return;
    }
    char domain[DOMAIN_TEXT_BYTES];
    odin3_abc_domain_text(run->module, run->domain, domain, sizeof domain);
    (void)fprintf(
        run->log, "# abc run %u: module '%s', %s\n", (unsigned)run->run_no,
        odin3_strtab_get(odin3_design_strtab(run->design), odin3_module_name(run->module)), domain);
}

odin3_status odin3_abc_exchange_blif(odin3_abc_run *run, odin3_abc_result *out) {
    xfiles files;
    odin3_abc_result_init(out);
    odin3_status st = odin3_abc_run_ensure_dir(run);
    st = st == ODIN3_OK ? run_paths(run, &files) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    run->last_line[0] = '\0';
    odin3_strbuf_clear(&run->lines);
    st = open_run_log(run);
    log_header(run);
    st = st == ODIN3_OK ? write_inputs(run, &files) : st;
    if (st == ODIN3_OK) {
        st = run->route == ODIN3_ABC_ROUTE_CHILD ? run_child(run, files.cmd)
                                                 : run_linked(run, &files);
    }
    close_run_log(run);
    run->run_no++;
    if (st != ODIN3_OK) {
        return st;
    }
    return odin3_abc_xblif_read(run, files.out, out);
}

/* --- the in-memory exchange ---------------------------------------------------------------- */

enum { REASON_TEXT_BYTES = ODIN3_ABC_REASON_BYTES - 64 };

/* load_aig, `&put`, the script and (when it ends in the network space) `&get -m -n` (AB-22). */
static odin3_status aig_commands(odin3_abc_run *run) {
    const odin3_abc_engine *eng = run->engine;
    odin3_abc_fail fail = {0};
    odin3_abc_aig view = odin3_abc_aig_view(&run->aig);
    odin3_status st = eng->load_aig(eng->self, &view, &fail);
    flush_lines(run);
    if (st == ODIN3_ERR_ENGINE) {
        return report_fail(run, &fail, "load_aig", NULL);
    }
    st = st == ODIN3_OK ? exec_line(run, "&put", NULL) : st;
    for (uint32_t i = 0; st == ODIN3_OK && i < run->script.n_cmds; i++) {
        const odin3_abc_cmd *src = odin3_abc_script_cmd(&run->script, i);
        st = exec_line(run, src->line, src);
    }
    if (st == ODIN3_OK && run->script.final_space == ODIN3_ABC_SPACE_NTK) {
        st = exec_line(run, "&get -m -n", NULL);
    }
    return st;
}

/* The name map's fallback: only a RESULT or COMMAND failure of name_map falls back to cones. */
static odin3_status name_map_failed(odin3_abc_run *run, const odin3_abc_fail *fail) {
    if (fail->kind != ODIN3_ABC_FAIL_RESULT && fail->kind != ODIN3_ABC_FAIL_COMMAND) {
        return report_fail(run, fail, "name_map", NULL);
    }
    char text[REASON_TEXT_BYTES];
    (void)snprintf(text, sizeof text, "%.*s", (int)sizeof text - 1,
                   fail->text[0] != '\0' ? fail->text : "the name map failed");
    (void)snprintf(run->nomap_reason, sizeof run->nomap_reason, "ABC: %s", text);
    return ODIN3_OK;
}

static odin3_status push_u32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

/* One MiniAIG literal (or -1) per aligned object, as the result's old_lit per LUT object. */
static odin3_status store_old_lit(odin3_abc_result *res, const int32_t *lits, uint32_t lut_base) {
    uint32_t total = odin3_abc_result_first_lut(res) + res->n_luts;
    odin3_vec_clear(&res->old_lit);
    odin3_status st = odin3_vec_reserve(&res->old_lit, total);
    for (uint32_t obj = 0; st == ODIN3_OK && obj < total; obj++) {
        uint32_t first = odin3_abc_result_first_lut(res);
        st = push_u32(&res->old_lit,
                      obj >= first ? (uint32_t)lits[lut_base + obj - first] : UINT32_MAX);
    }
    return st;
}

/* The name map (§6.3) on the aligned MiniLUT; skipped for --prov cone and above the cap. */
static odin3_status aig_name_map(odin3_abc_run *run, const odin3_abc_luts *luts,
                                 odin3_abc_result *res) {
    const odin3_abc_engine *eng = run->engine;
    uint32_t cap = odin3_abc_run_limits_get()->namemap_objs;
    run->nomap_reason[0] = '\0';
    if (run->opts.prov == ODIN3_ABC_PROV_CONE) {
        (void)snprintf(run->nomap_reason, sizeof run->nomap_reason, "--prov cone");
        return ODIN3_OK;
    }
    if (run->aig.objs.len / 2U > cap) {
        (void)snprintf(run->nomap_reason, sizeof run->nomap_reason,
                       "the AIG has more than %u objects", (unsigned)cap);
        return ODIN3_OK;
    }
    odin3_arena *arena = odin3_arena_create(0);
    odin3_abc_luts aligned = {0};
    odin3_abc_fail fail = {0};
    const int32_t *lits = NULL;
    odin3_status st =
        arena != NULL ? odin3_abc_aligned_build(run, luts, arena, &aligned) : ODIN3_ERR_NO_MEMORY;
    st = st == ODIN3_OK ? eng->name_map(eng->self, &aligned, &lits, &fail) : st;
    flush_lines(run);
    if (st ==
        ODIN3_ERR_INVALID_ARG) { /* not reached after the result's checks: no map, no failure */
        (void)snprintf(run->nomap_reason, sizeof run->nomap_reason,
                       "ABC: the aligned network cannot be built");
        st = ODIN3_OK;
    } else if (st == ODIN3_ERR_ENGINE) {
        st = name_map_failed(run, &fail);
    } else if (st == ODIN3_OK) {
        uint32_t lut_base = FIRST_OBJ_NUMBER + res->n_cis + (uint32_t)run->region->regs.len;
        st = store_old_lit(res, lits, lut_base);
    }
    odin3_arena_destroy(arena);
    return st;
}

/* get_luts, the result form and the name map, all before close (the session owns the arrays). */
static odin3_status aig_collect(odin3_abc_run *run, odin3_abc_result *out) {
    const odin3_abc_engine *eng = run->engine;
    odin3_abc_luts luts = {0};
    odin3_abc_fail fail = {0};
    odin3_status st = eng->get_luts(eng->self, &luts, &fail);
    flush_lines(run);
    if (st == ODIN3_ERR_ENGINE) {
        return report_fail(run, &fail, "get_luts", NULL);
    }
    odin3_arena *arena = st == ODIN3_OK ? odin3_arena_create(0) : NULL;
    const odin3_abc_result_expect expect = {(uint32_t)run->region->cis.len,
                                            (uint32_t)run->region->cos.len,
                                            (uint32_t)run->region->regs.len, run->opts.lut_k};
    st = arena != NULL ? odin3_abc_result_from_luts(&luts, &expect, arena, out)
                       : ODIN3_ERR_NO_MEMORY;
    odin3_arena_destroy(arena);
    return st == ODIN3_OK ? aig_name_map(run, &luts, out) : st;
}

static odin3_status aig_session(odin3_abc_run *run, odin3_abc_result *out) {
    const odin3_abc_engine *eng = run->engine;
    odin3_abc_fail fail = {0};
    run->streaming = false;
    odin3_status st = eng->open(eng->self, text_callback, run, &fail);
    flush_lines(run);
    if (st == ODIN3_ERR_ENGINE) {
        return report_fail(run, &fail, "open", NULL);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    st = aig_commands(run);
    st = st == ODIN3_OK ? aig_collect(run, out) : st;
    eng->close(eng->self);
    flush_lines(run);
    return st;
}

odin3_status odin3_abc_exchange_aig(odin3_abc_run *run, odin3_abc_result *out) {
    odin3_abc_result_init(out);
    odin3_abc_aig_free(&run->aig);
    run->last_line[0] = '\0';
    run->nomap_reason[0] = '\0';
    odin3_strbuf_clear(&run->lines);
    log_header(run);
    odin3_status st = odin3_abc_aig_build(run, run->region, &run->aig);
    st = st == ODIN3_OK ? aig_session(run, out) : st;
    run->run_no++;
    return st;
}
