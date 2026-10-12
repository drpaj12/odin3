/*
 * elab_test.h — static helpers shared by the 2D elaboration unit tests (plan Task 10, review I5):
 * a request of source files (written into a fresh directory under the build tree, which becomes
 * the working directory, so diagnostics name them as given), an optional project file written
 * next to them, a pre-script run on the fresh design (read_techlib, read_project, read_blif …),
 * then odin3_read_verilog_files and odin3_elab_stage1, with every message captured.
 *
 * The Verilog files read are the project record's when the pre-script read one, else the
 * request's files ending in ".v", in order (library "work"); other files are only written.
 * A test using these helpers calls et_free in its body and resets the log sink in tearDown
 * (odin3_log_set_sink(NULL, NULL)), so a failed assertion never leaves the sink pointing at a
 * dead result. Not to be included together with elab_test_tree.h (both define et_free).
 */
#ifndef ODIN3_TESTS_UNIT_ELAB_TEST_H
#define ODIN3_TESTS_UNIT_ELAB_TEST_H

#include "ast/ast.h"
#include "ast/check.h"
#include "ast/print.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "frontends/project/project.h"
#include "frontends/verilog/elab1.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/read_verilog.h"
#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ODIN3_ELAB_TMP
#error "ODIN3_ELAB_TMP must name a directory under the build tree"
#endif

enum { ET_PATH = 1024, ET_MAX_FILES = 16, ET_MAX_SOURCES = 16 };

/* One file of a request: its path relative to the fresh directory (one subdirectory level at
 * most, e.g. "lib/cells.v") and its text. */
typedef struct et_src {
    const char *path;
    const char *text;
} et_src;

/* A request: the files, the passes run on the fresh design first ("read_techlib …; read_project
 * p.o3proj"; NULL: none), a project file written next to the sources (NULL: none) and the
 * options handed to odin3_elab1_options (NULL: the defaults). */
typedef struct et_req {
    const et_src *srcs;
    uint32_t n;
    const char *pre_script;
    const et_src *project;
    const odin3_elab_opts *opts;
} et_req;

/* One captured message: its level and where its text starts in et_result.text. */
typedef struct et_msg {
    odin3_log_level level;
    size_t at;
} et_msg;

/* One stage-1 run: everything is owned until et_free. */
typedef struct et_result {
    odin3_design *design;
    odin3_pass_ctx ctx;                /* the read's run */
    odin3_ast *parsed;                 /* the parsed store (owned by the result, not the design) */
    odin3_ast *elab;                   /* the elaborated store; NULL unless stage 1 succeeded */
    odin3_elab_opts opts;              /* the options after odin3_elab1_options */
    odin3_status status;               /* odin3_elab_stage1's */
    odin3_strbuf dump;                 /* odin3_ast_dump of the elaborated UNIT */
    odin3_strbuf text;                 /* every message, NUL-separated */
    odin3_vec msgs;                    /* et_msg, in order */
    uint32_t errors;                   /* messages at ODIN3_LOG_ERROR */
    uint32_t warnings;                 /* messages at ODIN3_LOG_WARN */
    char dir[ET_PATH];                 /* the fresh directory */
    char cwd[ET_PATH];                 /* the working directory before, restored by et_free */
    char files[ET_MAX_FILES][ET_PATH]; /* written files and directories, removed by et_free */
    uint32_t n_files;
} et_result;

/* The capturing sink: user is the et_result. */
static inline void et_sink(odin3_log_level level, const char *msg, void *user) {
    et_result *res = user;
    et_msg *slot = odin3_vec_push(&res->msgs);
    if (slot == NULL || odin3_strbuf_append(&res->text, odin3_bytes_cstr(msg)) != ODIN3_OK ||
        odin3_strbuf_append(&res->text, (odin3_bytes){"", 1}) != ODIN3_OK) {
        return; /* out of memory (an OOM sweep): the message is lost */
    }
    *slot = (et_msg){level, res->text.len - strlen(msg) - 1};
    res->errors += level == ODIN3_LOG_ERROR ? 1 : 0;
    res->warnings += level == ODIN3_LOG_WARN ? 1 : 0;
}

/* Message i's text and level. */
static inline const char *et_text(const et_result *res, uint32_t idx) {
    const et_msg *msg = odin3_vec_cat(&res->msgs, idx);
    return res->text.data + msg->at;
}

static inline odin3_log_level et_level(const et_result *res, uint32_t idx) {
    return ((const et_msg *)odin3_vec_cat(&res->msgs, idx))->level;
}

static inline uint32_t et_count(const et_result *res) {
    return (uint32_t)res->msgs.len;
}

/* Every captured message, one per line with its level, for assertion messages. */
static inline const char *et_log(const et_result *res) {
    static char buf[1 << 14];
    size_t len = 0;
    buf[0] = '\0';
    for (uint32_t i = 0; i < et_count(res) && len < sizeof buf; i++) {
        int got = snprintf(buf + len, sizeof buf - len, "[%d] %s\n", (int)et_level(res, i),
                           et_text(res, i));
        if (got < 0) {
            break;
        }
        len += (size_t)got;
    }
    return buf;
}

/* The index of the message text at level, -1 when none. */
static inline int et_find(const et_result *res, odin3_log_level level, const char *text) {
    for (uint32_t i = 0; i < et_count(res); i++) {
        if (et_level(res, i) == level && strcmp(et_text(res, i), text) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Asserts the message text at level was logged. */
static inline void et_expect(const et_result *res, odin3_log_level level, const char *text) {
    if (et_find(res, level, text) < 0) {
        static char why[1 << 15];
        int got =
            snprintf(why, sizeof why, "missing [%d] %s\nlog:\n%s", (int)level, text, et_log(res));
        TEST_FAIL_MESSAGE(got > 0 ? why : text);
    }
}

/* The messages at level whose text contains needle. */
static inline uint32_t et_count_with(const et_result *res, odin3_log_level level,
                                     const char *needle) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < et_count(res); i++) {
        count += et_level(res, i) == level && strstr(et_text(res, i), needle) != NULL ? 1 : 0;
    }
    return count;
}

/* The errors that are not point (l)'s pending lines. */
static inline uint32_t et_real_errors(const et_result *res) {
    return res->errors -
           et_count_with(res, ODIN3_LOG_ERROR, "elaboration is not available yet (2D)");
}

/* Records a path (file or directory) et_free removes, deepest first. */
static inline void et_note_path(et_result *out, const char *path) {
    TEST_ASSERT_TRUE(out->n_files < ET_MAX_FILES);
    int got = snprintf(out->files[out->n_files], ET_PATH, "%s", path);
    TEST_ASSERT_TRUE(got > 0 && got < ET_PATH);
    out->n_files++;
}

/* Writes src under the current (fresh) directory, making its one parent directory if needed. */
static inline void et_write(et_result *out, const et_src *src) {
    const char *slash = strchr(src->path, '/');
    if (slash != NULL) {
        char dir[ET_PATH];
        int got = snprintf(dir, sizeof dir, "%.*s", (int)(slash - src->path), src->path);
        TEST_ASSERT_TRUE(got > 0 && got < ET_PATH);
        struct stat info;
        if (stat(dir, &info) != 0) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, mkdir(dir, 0700), dir);
            et_note_path(out, dir);
        }
    }
    FILE *file = fopen(src->path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, src->path);
    size_t len = strlen(src->text);
    TEST_ASSERT_EQUAL_size_t(len, fwrite(src->text, 1, len, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    et_note_path(out, src->path);
}

/* The fresh directory under ODIN3_ELAB_TMP, made the working directory (the old one kept). */
static inline void et_enter_fresh_dir(et_result *out) {
    TEST_ASSERT_NOT_NULL(getcwd(out->cwd, sizeof out->cwd));
    int got = snprintf(out->dir, sizeof out->dir, "%s/et.XXXXXX", ODIN3_ELAB_TMP);
    TEST_ASSERT_TRUE(got > 0 && got < ET_PATH);
    TEST_ASSERT_NOT_NULL_MESSAGE(mkdtemp(out->dir), out->dir);
    TEST_ASSERT_EQUAL_INT(0, chdir(out->dir));
}

/* The pre-script on the design (it must succeed). */
static inline void et_pre_script(et_result *out, const char *script) {
    odin3_status st = odin3_pass_run_script(out->design, odin3_bytes_cstr(script),
                                            (odin3_script_src){"pre", ODIN3_SCRIPT_BY_COMMAND});
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, et_log(out));
}

/*
 * The fresh directory (made the working directory) with the request's files, a fresh design with
 * the capturing sink installed, and the pre-script run on it. Use et_stage1 for the whole run, or
 * this alone to run passes on the design (et_free cleans up either way).
 */
static inline void et_setup(const et_req *req, et_result *out) {
    memset(out, 0, sizeof *out);
    odin3_strbuf_init(&out->dump);
    odin3_strbuf_init(&out->text);
    odin3_vec_init(&out->msgs, sizeof(et_msg));
    et_enter_fresh_dir(out);
    for (uint32_t i = 0; i < req->n; i++) {
        et_write(out, &req->srcs[i]);
    }
    if (req->project != NULL) {
        et_write(out, req->project);
    }
    out->design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(out->design);
    odin3_log_set_sink(et_sink, out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_INFO));
    if (req->pre_script != NULL) {
        et_pre_script(out, req->pre_script);
    }
}

static inline uint32_t et_intern(et_result *out, const char *text) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(out->design, odin3_bytes_cstr(text), &id));
    return id;
}

/* The record's Verilog design files (none when no record was read). */
static inline uint32_t et_record_sources(const et_result *out, odin3_pp_source *srcs) {
    uint32_t count = 0;
    const odin3_project *proj = odin3_design_project(out->design);
    for (size_t i = 0; proj != NULL && proj->finished && i < proj->files.len; i++) {
        const odin3_proj_file *file = odin3_vec_cat(&proj->files, i);
        if (file->lang == ODIN3_LANG_VERILOG && count < ET_MAX_SOURCES) {
            srcs[count++] = (odin3_pp_source){file->path.given, file->path.resolved, file->library,
                                              file->path.at};
        }
    }
    return count;
}

/* One request file as read_verilog names a direct file (library work). */
static inline odin3_pp_source et_direct_source(et_result *out, const char *path) {
    char resolved[2 * ET_PATH];
    int got = snprintf(resolved, sizeof resolved, "%s/%s", out->dir, path);
    TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof resolved);
    return (odin3_pp_source){
        et_intern(out, path), et_intern(out, resolved), et_intern(out, "work"), {0}};
}

static inline bool et_is_verilog(const char *path) {
    size_t len = strlen(path);
    return len >= 2 && strcmp(path + len - 2, ".v") == 0;
}

/* The Verilog files to read: the record's (when one was read) or the request's ".v" files. */
static inline uint32_t et_sources(et_result *out, const et_req *req, odin3_pp_source *srcs) {
    uint32_t count = et_record_sources(out, srcs);
    if (count > 0) {
        return count;
    }
    for (uint32_t i = 0; i < req->n && count < ET_MAX_SOURCES; i++) {
        if (et_is_verilog(req->srcs[i].path)) {
            srcs[count++] = et_direct_source(out, req->srcs[i].path);
        }
    }
    return count;
}

/* et_setup, then the read (which must succeed) and odin3_elab_stage1; the dump on success. */
static inline void et_stage1(const et_req *req, et_result *out) {
    et_setup(req, out);
    odin3_pp_source srcs[ET_MAX_SOURCES];
    uint32_t count = et_sources(out, req, srcs);
    TEST_ASSERT_TRUE(count > 0);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_pass_run_begin(out->design, et_intern(out, "read_verilog"), &out->ctx));
    odin3_rv_opts ropts = {srcs, count, out->ctx.run, NULL};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_read_verilog_files(out->design, &ropts, &out->parsed), et_log(out));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab1_options(out->design, req->opts, &out->opts));
    out->status = odin3_elab_stage1(&out->ctx, out->parsed, &out->opts, &out->elab);
    if (out->elab != NULL) {
        TEST_ASSERT_EQUAL_INT(1, odin3_ast_root_count(out->elab));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_ast_dump(out->elab, odin3_ast_root(out->elab, 0), &out->dump));
    }
}

/* The elaborated store binds (EL-5: the seam stage 2 reads) and then passes check FULL, rule 8
 * included. A store binds once: its symbol table stays with it. */
static inline void et_expect_binds(const et_result *res) {
    odin3_elab_binding binding = {0};
    odin3_status st = odin3_elab_bind(res->elab, &binding);
    odin3_elab_binding_free(&binding);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, et_log(res));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(res->elab, ODIN3_CHECK_FULL));
}

/* Frees everything, removes the files and the directory, restores the working directory and the
 * default sink. No Unity assertion here: after a failed test one would abort the clean-up. */
static inline void et_free(et_result *res) {
    odin3_log_set_sink(NULL, NULL);
    odin3_ast_destroy(res->elab);
    odin3_ast_destroy(res->parsed);
    odin3_design_destroy(res->design);
    for (uint32_t i = res->n_files; i-- > 0;) {
        if (remove(res->files[i]) != 0) {
            (void)rmdir(res->files[i]);
        }
    }
    if (res->cwd[0] != '\0' && chdir(res->cwd) != 0) {
        (void)fprintf(stderr, "et_free: cannot return to %s\n", res->cwd);
    }
    if (res->dir[0] != '\0') {
        (void)rmdir(res->dir);
    }
    odin3_strbuf_free(&res->dump);
    odin3_strbuf_free(&res->text);
    odin3_vec_free(&res->msgs);
    memset(res, 0, sizeof *res);
}

#endif
