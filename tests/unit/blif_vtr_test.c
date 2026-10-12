/* blif_vtr_test.c — helpers for test_blif_vtr.c (see blif_vtr_test.h). */
#include "blif_vtr_test.h"

#include "passes/manager.h"
#include "util/hash.h"
#include "util/log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef ODIN3_BVT_ROOT
#error "ODIN3_BVT_ROOT must name the build tree"
#endif
#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must name lib/"
#endif

enum { PATH_BUF = 512, READ_CHUNK = 4096 };

static const char *const LEVEL_NAMES[ODIN3_LOG_LEVEL_COUNT] = {"error", "warn", "info", "debug"};

static void bvt_sink(odin3_log_level level, const char *msg, void *user) {
    bvt *bv = user;
    if (level == ODIN3_LOG_ERROR) {
        bv->errors++;
    } else if (level == ODIN3_LOG_WARN) {
        bv->warnings++;
    }
    (void)odin3_strbuf_appendf(&bv->log, "%s: %s\n", LEVEL_NAMES[level], msg);
}

static void join(const bvt *bv, const char *name, char *path) {
    (void)snprintf(path, PATH_BUF, "%s/%s", bv->dir, name);
}

/* Writes text to bv->dir/t.blif. */
static odin3_status put_input(const bvt *bv, const char *text) {
    char path[PATH_BUF];
    join(bv, "t.blif", path);
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return ODIN3_ERR_IO;
    }
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, file) == len;
    return fclose(file) == 0 && ok ? ODIN3_OK : ODIN3_ERR_IO;
}

/* The design and the directory on the first call; the sink on every call. */
static odin3_status bvt_setup(bvt *bv) {
    if (bv->design == NULL) {
        odin3_strbuf_init(&bv->log);
        bv->design = odin3_design_create();
        if (bv->design == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    if (bv->dir[0] == '\0') {
        (void)snprintf(bv->dir, sizeof bv->dir, "%s/bvt.XXXXXX", ODIN3_BVT_ROOT);
        if (mkdtemp(bv->dir) == NULL) {
            bv->dir[0] = '\0';
            return ODIN3_ERR_IO;
        }
    }
    odin3_log_set_sink(bvt_sink, bv);
    return ODIN3_OK;
}

/* Runs pass with args in bv->dir, then returns to the working directory. */
static odin3_status run_in_dir(bvt *bv, const char *pass, const char *args) {
    char cwd[PATH_BUF];
    if (getcwd(cwd, sizeof cwd) == NULL || chdir(bv->dir) != 0) {
        return ODIN3_ERR_IO;
    }
    odin3_status st = odin3_pass_run(bv->design, pass, odin3_bytes_cstr(args));
    return chdir(cwd) == 0 ? st : ODIN3_ERR_IO;
}

odin3_status bvt_read_lib(bvt *bv, bvt_source src) {
    odin3_status st = bvt_setup(bv);
    st = st == ODIN3_OK ? put_input(bv, src.blif_text) : st;
    if (st == ODIN3_OK && src.lib_file != NULL) {
        char path[PATH_BUF];
        int len = snprintf(path, sizeof path, "%s/%s", ODIN3_LIB_DIR, src.lib_file);
        st = len > 0 && len < PATH_BUF ? run_in_dir(bv, "read_techlib", path) : ODIN3_ERR_IO;
    }
    return st == ODIN3_OK ? run_in_dir(bv, "read_blif", "t.blif") : st;
}

odin3_status bvt_read(bvt *bv, const char *blif_text, bool vtr_lib) {
    return bvt_read_lib(bv, (bvt_source){blif_text, vtr_lib ? "vtr.o3lib" : NULL});
}

static odin3_status slurp(const char *path, odin3_strbuf *out) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ODIN3_OK;
    }
    char chunk[READ_CHUNK];
    odin3_status st = ODIN3_OK;
    for (size_t got = fread(chunk, 1, sizeof chunk, file); st == ODIN3_OK && got > 0;
         got = fread(chunk, 1, sizeof chunk, file)) {
        st = odin3_strbuf_append(out, (odin3_bytes){chunk, got});
    }
    (void)fclose(file);
    return st;
}

odin3_status bvt_write(bvt *bv, const char *pass_args, odin3_strbuf *out) {
    odin3_strbuf_clear(&bv->log);
    bv->errors = 0;
    bv->warnings = 0;
    odin3_strbuf_clear(out);
    odin3_status st = run_in_dir(bv, "write_blif", pass_args);
    char path[PATH_BUF];
    join(bv, "out.blif", path);
    odin3_status read = slurp(path, out);
    return st != ODIN3_OK ? st : read;
}

bool bvt_file_exists(const bvt *bv, const char *name) {
    char path[PATH_BUF];
    join(bv, name, path);
    return access(path, F_OK) == 0;
}

void bvt_free(bvt *bv) {
    DIR *dir = bv->dir[0] != '\0' ? opendir(bv->dir) : NULL;
    for (struct dirent *entry = dir != NULL ? readdir(dir) : NULL; entry != NULL;
         entry = readdir(dir)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char path[PATH_BUF];
            join(bv, entry->d_name, path);
            (void)unlink(path);
        }
    }
    if (dir != NULL) {
        (void)closedir(dir);
        (void)rmdir(bv->dir);
    }
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(bv->design);
    odin3_strbuf_free(&bv->log);
    *bv = (bvt){0};
}
