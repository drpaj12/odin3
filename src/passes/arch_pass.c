/*
 * arch_pass.c — the `read_arch` and `write_techlib` passes (Phase 4.1, spec C-4, C-5).
 */
#include "frontends/arch/arch.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "techlib/reader.h"
#include "util/log.h"
#include "util/str.h"

#include <stdio.h>
#include <string.h>

/* The single word of args, NUL-terminated in buf. INVALID_ARG (logged) otherwise. */
static odin3_status one_word(const char *pass, odin3_bytes args, odin3_strbuf *buf) {
    odin3_bytes word = odin3_pass_arg_next(&args);
    if (word.len == 0 || odin3_pass_arg_next(&args).ptr != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "%s: expects one path", pass);
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_strbuf_append(buf, word);
}

static bool has_suffix(const char *path, const char *suffix) {
    size_t len = strlen(path);
    size_t n = strlen(suffix);
    return len >= n && strcmp(path + len - n, suffix) == 0;
}

static odin3_status read_arch_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                  void *user) {
    (void)ctx;
    (void)user;
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = one_word("read_arch", args, &path);
    if (st == ODIN3_OK && has_suffix(path.data, ".xml")) {
        st = odin3_arch_read(design, path.data);
    } else if (st == ODIN3_OK && has_suffix(path.data, ".o3lib")) {
        st = odin3_techlib_read(design, path.data);
    } else if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "read_arch: expected a .xml or .o3lib file, not '%s'",
                  path.data);
        st = ODIN3_ERR_INVALID_ARG;
    }
    odin3_strbuf_free(&path);
    return st;
}

static odin3_status write_techlib_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                      void *user) {
    (void)ctx;
    (void)user;
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = one_word("write_techlib", args, &path);
    const odin3_arch *arch = odin3_design_arch(design);
    if (st == ODIN3_OK && arch == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "write_techlib: no architecture has been read (read_arch first)");
        st = ODIN3_ERR_INVALID_ARG;
    }
    if (st == ODIN3_OK && strcmp(path.data, "-") == 0) {
        const char *text = odin3_arch_library_text(arch);
        st = fputs(text, stdout) >= 0 && fflush(stdout) == 0 ? ODIN3_OK : ODIN3_ERR_IO;
    } else if (st == ODIN3_OK) {
        st = odin3_pass_write_text(path.data, &arch->text);
    }
    odin3_strbuf_free(&path);
    return st;
}

const odin3_pass_def odin3_pass_read_arch = {
    "read_arch",
    "read_arch <file.xml|file.o3lib>: import a VPR architecture's models and pb_types (the models "
    "become hard cell types), or read a tech library",
    read_arch_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};

const odin3_pass_def odin3_pass_write_techlib = {
    "write_techlib",
    "write_techlib <file|->: write the .o3lib library that read_arch generated",
    write_techlib_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
