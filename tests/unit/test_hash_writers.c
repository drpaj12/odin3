/*
 * test_hash_writers.c — `--name-style hash` in write_blif (VTR), write_verilog and write_json, and
 * the `names` pass (spec 5.4 section 13: H-1, H-2, H-4, H-5; T-hash-6). Every case works in its own
 * mkdtemp directory under the build tree. The design is a BLIF read with --hdl-locs, so its nets
 * and cells carry HDL locations.
 */
#include "frontends/blif/reader.h"
#include "ir/design.h"
#include "ir/module.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ODIN3_HASHW_ROOT
#error "ODIN3_HASHW_ROOT must name the build tree"
#endif

enum { PATH_BUF = 512, DIR_BUF = 256, CHUNK = 4096, LOG_MAX = 4096, ARGS_BUF = 1100, SMALL = 64 };

static char dir[DIR_BUF];
static odin3_design *design;
static char log_text[LOG_MAX];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level <= ODIN3_LOG_WARN) {
        size_t used = strlen(log_text);
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

static void path_of(const char *name, char *out) {
    (void)snprintf(out, PATH_BUF, "%s/%.240s", dir, name);
}

void setUp(void) {
    (void)snprintf(dir, sizeof dir, "%s/hashw.XXXXXX", ODIN3_HASHW_ROOT);
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    log_text[0] = '\0';
    odin3_log_set_sink(sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    DIR *handle = opendir(dir);
    for (struct dirent *entry = handle != NULL ? readdir(handle) : NULL; entry != NULL;
         entry = readdir(handle)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char path[PATH_BUF];
            path_of(entry->d_name, path);
            (void)unlink(path);
        }
    }
    if (handle != NULL) {
        (void)closedir(handle);
    }
    (void)rmdir(dir);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

static bool exists(const char *name) {
    char path[PATH_BUF];
    struct stat info;
    path_of(name, path);
    return stat(path, &info) == 0;
}

static char *slurp(const char *name) {
    char path[PATH_BUF];
    path_of(name, path);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    char *text = odin3_util_calloc(1);
    size_t len = 0;
    char chunk[CHUNK];
    for (size_t got = fread(chunk, 1, sizeof chunk, file); got > 0;
         got = fread(chunk, 1, sizeof chunk, file)) {
        text = odin3_util_realloc(text, len + got + 1);
        memcpy(text + len, chunk, got);
        len += got;
        text[len] = '\0';
    }
    (void)fclose(file);
    return text;
}

static void put_file(const char *name, odin3_bytes text) {
    char path[PATH_BUF];
    path_of(name, path);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(text.len, fwrite(text.ptr, 1, text.len, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* A top with a located net `w` and a located cell on it, a hierarchy in a second located net. */
static const char *const INPUT = ".model top\n.inputs a b\n.outputs y\n"
                                 ".names a b top/u1/w@d.v:3\n11 1\n.cname g1\n"
                                 ".names top/u1/w@d.v:3 v@d.v:4\n1 1\n"
                                 ".names v@d.v:4 y\n1 1\n.end\n";

static void load_design(void) {
    char path[PATH_BUF];
    put_file("in.blif", odin3_bytes_cstr(INPUT));
    path_of("in.blif", path);
    odin3_blif_read_stats stats = {0};
    odin3_blif_read_opts opts = {.hdl_locs = true, .stats = &stats};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read_ex(design, NULL, path, &opts),
                                  log_text);
}

/* The formatted arguments of a pass call (one buffer: use the result at once). */
static odin3_bytes fmt_args(const char *format, ...) __attribute__((format(printf, 1, 2)));
static odin3_bytes fmt_args(const char *format, ...) {
    static char args[ARGS_BUF];
    va_list ap;
    va_start(ap, format);
    int len = vsnprintf(args, sizeof args, format, ap);
    va_end(ap);
    TEST_ASSERT_TRUE(len >= 0 && (size_t)len < sizeof args);
    return (odin3_bytes){args, (size_t)len};
}

static odin3_status run(const char *pass, odin3_bytes args) {
    return odin3_pass_run(design, pass, args);
}

static void run_ok(const char *pass, odin3_bytes args) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(pass, args), log_text);
}

/* --- the writers ------------------------------------------------------------------------------ */

typedef struct writer_case {
    const char *pass;
    const char *flags; /* before the path */
    const char *ext;
    uint32_t entries; /* the renamed objects of the test design */
} writer_case;

static const writer_case WRITERS[] = {
    {"write_blif", "--vtr", ".blif", 2},
    {"write_verilog", "", ".v", 2},
    {"write_json", "", ".json", 4},
};
enum { N_WRITERS = 3, HASH_CHARS = 11 };

static odin3_status write_hash_extra(const writer_case *wc, const char *base, odin3_bytes extra) {
    return run(wc->pass, fmt_args("%s --name-style hash %.*s %s/%s%s", wc->flags, (int)extra.len,
                                  (const char *)extra.ptr, dir, base, wc->ext));
}

static odin3_status write_hash(const writer_case *wc, const char *base) {
    return write_hash_extra(wc, base, odin3_bytes_cstr(""));
}

/* The data lines of a map: not blank, not `#`. */
static uint32_t map_lines(const char *map) {
    uint32_t count = 0;
    for (const char *at = map; *at != '\0';) {
        const char *end = strchr(at, '\n');
        end = end != NULL ? end : at + strlen(at);
        count += end > at && *at != '#' ? 1U : 0U;
        at = *end != '\0' ? end + 1 : end;
    }
    return count;
}

/* The third tab-separated field of line (the short name) into out. */
static void short_field(const char *line, char *out) {
    const char *tab = strchr(line, '\t');
    tab = tab != NULL ? strchr(tab + 1, '\t') : NULL;
    const char *end = tab != NULL ? strchr(tab + 1, '\t') : NULL;
    TEST_ASSERT_NOT_NULL_MESSAGE(end, line);
    memcpy(out, tab + 1, (size_t)(end - tab - 1));
    out[end - tab - 1] = '\0';
}

/* One data line of a map and its short name. */
typedef struct entry_view {
    const char *line;
    const char *short_name;
} entry_view;

/* Calls check(entry, user) for every data line of map. */
typedef void (*short_check)(const entry_view *entry, void *user);
static void each_entry(const char *map, short_check check, void *user) {
    for (const char *at = map; *at != '\0';) {
        const char *end = strchr(at, '\n');
        end = end != NULL ? end : at + strlen(at);
        char line[LOG_MAX];
        char shorts[ARGS_BUF];
        TEST_ASSERT_TRUE((size_t)(end - at) < sizeof line);
        memcpy(line, at, (size_t)(end - at));
        line[end - at] = '\0';
        if (end > at && *at != '#') {
            short_field(line, shorts);
            check(&(entry_view){line, shorts}, user);
        }
        at = *end != '\0' ? end + 1 : end;
    }
}

static bool base32_char(char chr) {
    return (chr >= 'a' && chr <= 'z') || (chr >= '2' && chr <= '7');
}

/* `h` and 10 base32 characters. */
static void expect_hash_text(const char *short_name) {
    TEST_ASSERT_EQUAL_size_t_MESSAGE(HASH_CHARS, strlen(short_name), short_name);
    TEST_ASSERT_EQUAL_CHAR('h', short_name[0]);
    for (size_t i = 1; i < HASH_CHARS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(base32_char(short_name[i]), short_name);
    }
}

static void expect_hash_shape(const entry_view *entry, void *user) {
    /* every renamed object has an HDL location, so no port is in the map */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(entry->line, "@d.v:"), entry->line);
    expect_hash_text(entry->short_name);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr((const char *)user, entry->short_name), entry->short_name);
}

/* The netlist and the name map of one write named base. */
typedef struct written {
    char *net;
    char *map;
} written;

static written read_written(const writer_case *wc, const char *base) {
    char name[SMALL];
    char map[SMALL + 8];
    (void)snprintf(name, sizeof name, "%s%s", base, wc->ext);
    (void)snprintf(map, sizeof map, "%s.names", name);
    return (written){slurp(name), slurp(map)};
}

static void free_written(written *files) {
    odin3_util_free(files->net);
    odin3_util_free(files->map);
}

static void test_writer_hash(const writer_case *wc) {
    load_design();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, write_hash(wc, "one"), log_text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, write_hash(wc, "two"), log_text);
    written one = read_written(wc, "one");
    written two = read_written(wc, "two");
    /* deterministic: byte-equal netlist and map (the map holds no path) */
    TEST_ASSERT_EQUAL_STRING(one.net, two.net);
    TEST_ASSERT_EQUAL_STRING(one.map, two.map);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(one.map, "# odin3-names 1 hash=sha256 width=10\n", 37),
                                  one.map);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(wc->entries, map_lines(one.map), one.map);
    each_entry(one.map, expect_hash_shape, one.net);
    /* the provenance form is gone from the netlist; the ports are not renamed */
    TEST_ASSERT_NULL_MESSAGE(strstr(one.net, "@d.v"), one.net);
    TEST_ASSERT_NOT_NULL(strstr(one.map, "top/u1/w@d.v:3"));
    TEST_ASSERT_NOT_NULL(strstr(one.map, "\tnet\t"));
    free_written(&one);
    free_written(&two);
}

static void test_blif_hash(void) {
    test_writer_hash(&WRITERS[0]);
    char *net = slurp("one.blif");
    TEST_ASSERT_NOT_NULL(strstr(net, ".inputs a b\n"));
    TEST_ASSERT_NOT_NULL(strstr(net, ".outputs y\n"));
    odin3_util_free(net);
}

static void test_verilog_hash(void) {
    test_writer_hash(&WRITERS[1]);
    char *net = slurp("one.v");
    TEST_ASSERT_NOT_NULL(strstr(net, "input a"));
    TEST_ASSERT_NOT_NULL(strstr(net, "output y"));
    odin3_util_free(net);
}

static void test_json_hash(void) {
    test_writer_hash(&WRITERS[2]);
    char *net = slurp("one.json");
    TEST_ASSERT_NOT_NULL(strstr(net, "\"a\":"));
    TEST_ASSERT_NOT_NULL(strstr(net, "\"y\":"));
    odin3_util_free(net);
}

/* The map goes where --name-map says; the default `<out>.names` is then not written. */
static void test_explicit_map_path(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        const writer_case *wc = &WRITERS[i];
        load_design();
        char extra[PATH_BUF];
        char sidecar[PATH_BUF];
        (void)snprintf(extra, sizeof extra, "--name-map %s/m%d.map", dir, i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, write_hash_extra(wc, "o", odin3_bytes_cstr(extra)),
                                      log_text);
        (void)snprintf(sidecar, sizeof sidecar, "o%s.names", wc->ext);
        char mapname[PATH_BUF];
        (void)snprintf(mapname, sizeof mapname, "m%d.map", i);
        TEST_ASSERT_TRUE(exists(mapname));
        TEST_ASSERT_FALSE(exists(sidecar));
        odin3_design_destroy(design);
        design = odin3_design_create();
    }
}

/* H-1: no writable map, no write: the netlist is not written either. */
static void test_refused_without_map(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        const writer_case *wc = &WRITERS[i];
        load_design();
        char extra[PATH_BUF];
        (void)snprintf(extra, sizeof extra, "--name-map %s/no/such/dir/m.map", dir);
        log_text[0] = '\0';
        TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, write_hash_extra(wc, "x", odin3_bytes_cstr(extra)));
        char outname[PATH_BUF];
        (void)snprintf(outname, sizeof outname, "x%s", wc->ext);
        TEST_ASSERT_FALSE(exists(outname));
        odin3_design_destroy(design);
        design = odin3_design_create();
    }
}

/* A map that cannot be renamed into place (the path is a directory) fails the write and leaves an
 * existing netlist as it was: never a new netlist beside a stale map. */
static void test_map_commit_failure(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        const writer_case *wc = &WRITERS[i];
        load_design();
        char outname[SMALL];
        char path[PATH_BUF];
        char extra[PATH_BUF];
        (void)snprintf(outname, sizeof outname, "old%s", wc->ext);
        put_file(outname, odin3_bytes_cstr("OLD\n"));
        path_of("mapdir", path);
        TEST_ASSERT_EQUAL_INT(0, mkdir(path, 0700));
        (void)snprintf(extra, sizeof extra, "--name-map %.400s", path);
        TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, write_hash_extra(wc, "old", odin3_bytes_cstr(extra)));
        char *net = slurp(outname);
        TEST_ASSERT_EQUAL_STRING("OLD\n", net);
        odin3_util_free(net);
        TEST_ASSERT_EQUAL_INT(0, rmdir(path));
        odin3_design_destroy(design);
        design = odin3_design_create();
    }
}

/* --name-map equal to the output path (also spelled with `./`) is refused; nothing is written. */
static void test_map_is_output_path(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        const writer_case *wc = &WRITERS[i];
        load_design();
        char outname[SMALL];
        char extra[PATH_BUF];
        (void)snprintf(outname, sizeof outname, "same%s", wc->ext);
        (void)snprintf(extra, sizeof extra, "--name-map %s/./%s", dir, outname);
        log_text[0] = '\0';
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                              write_hash_extra(wc, "same", odin3_bytes_cstr(extra)));
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "--name-map is the output path"), log_text);
        TEST_ASSERT_FALSE(exists(outname));
        odin3_design_destroy(design);
        design = odin3_design_create();
    }
}

/* Output "-" (stdout) has no default map path: refused with --name-style hash. */
static void test_refused_stdout_without_map(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        const writer_case *wc = &WRITERS[i];
        load_design();
        log_text[0] = '\0';
        odin3_status st = run(wc->pass, fmt_args("%s --name-style hash -", wc->flags));
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, st, log_text);
        TEST_ASSERT_NOT_NULL(strstr(log_text, "--name-map"));
        odin3_design_destroy(design);
        design = odin3_design_create();
    }
}

/* --name-map without hash is refused. */
static void test_map_needs_hash(void) {
    load_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          run("write_verilog", fmt_args("%s/p.v --name-map %s/p.map", dir, dir)));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          run("write_json", fmt_args("%s/p.json --name-map %s/p.map", dir, dir)));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_INVALID_ARG,
        run("write_blif",
            fmt_args("--vtr --name-style provenance --name-map %s/p.map %s/p.blif", dir, dir)));
    TEST_ASSERT_FALSE(exists("p.map"));
}

static void expect_no_sidecars(void) {
    TEST_ASSERT_FALSE(exists("p.v.names"));
    TEST_ASSERT_FALSE(exists("p.json.names"));
    TEST_ASSERT_FALSE(exists("p.blif.names"));
}

static void expect_json_provenance(void) {
    char *prov = slurp("p.json");
    TEST_ASSERT_NOT_NULL(strstr(prov, "top/u1/w@d.v:3"));
    odin3_util_free(prov);
}

/* Provenance and short styles write no map; provenance JSON names carry the location. */
static void test_no_map_without_hash(void) {
    load_design();
    run_ok("write_verilog", fmt_args("%s/p.v --name-style provenance", dir));
    run_ok("write_json", fmt_args("%s/p.json --name-style provenance", dir));
    run_ok("write_json", fmt_args("%s/s.json --name-style short", dir));
    run_ok("write_blif", fmt_args("--vtr --name-style provenance %s/p.blif", dir));
    expect_no_sidecars();
    expect_json_provenance();
}

/* JSON without the option is as before: the IR names, no provenance names. */
static void test_json_default_unchanged(void) {
    load_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_json", fmt_args("%s/a.json", dir)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          run("write_json", fmt_args("%s/b.json --name-style short", dir)));
    char *one = slurp("a.json");
    char *two = slurp("b.json");
    TEST_ASSERT_EQUAL_STRING(one, two);
    TEST_ASSERT_NULL(strstr(one, "@d.v"));
    odin3_util_free(one);
    odin3_util_free(two);
}

/* --- the names pass (T-hash-6) --------------------------------------------------------------- */

/* Runs pass names and returns what it printed to stdout. */
static char *names_out(odin3_status *st, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
static char *names_out(odin3_status *st, const char *format, ...) {
    char args[ARGS_BUF];
    char path[PATH_BUF];
    va_list ap;
    va_start(ap, format);
    (void)vsnprintf(args, sizeof args, format, ap);
    va_end(ap);
    path_of("cap.txt", path);
    (void)fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int capture = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    TEST_ASSERT_TRUE(saved >= 0 && capture >= 0);
    TEST_ASSERT_TRUE(dup2(capture, STDOUT_FILENO) >= 0);
    *st = odin3_pass_run(design, "names", odin3_bytes_cstr(args));
    (void)fflush(stdout);
    TEST_ASSERT_TRUE(dup2(saved, STDOUT_FILENO) >= 0);
    (void)close(capture);
    (void)close(saved);
    return slurp("cap.txt");
}

/* A written map of the test design (BLIF), and one entry's fields. */
typedef struct entry_fields {
    char short_name[ARGS_BUF];
    char full[ARGS_BUF];
} entry_fields;

static void first_entry(const entry_view *entry, void *user) {
    entry_fields *fields = user;
    if (fields->short_name[0] != '\0') {
        return;
    }
    (void)snprintf(fields->short_name, sizeof fields->short_name, "%s", entry->short_name);
    const char *tab = entry->line;
    for (int i = 0; i < 3; i++) {
        tab = strchr(tab, '\t') + 1;
    }
    const char *end = strchr(tab, '\t');
    (void)snprintf(fields->full, sizeof fields->full, "%.*s", (int)(end - tab), tab);
}

static entry_fields written_map(void) {
    load_design();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, run("write_blif", fmt_args("--vtr --name-style hash %s/m.blif", dir)), log_text);
    char *map = slurp("m.blif.names");
    entry_fields fields = {{0}, {0}};
    each_entry(map, first_entry, &fields);
    odin3_util_free(map);
    TEST_ASSERT_TRUE(fields.short_name[0] == 'h');
    return fields;
}

static void test_names_lookup_reverse(void) {
    entry_fields fields = written_map();
    odin3_design_destroy(design); /* no design loaded: the pass reads only the map */
    design = odin3_design_create();
    odin3_status st = ODIN3_OK;
    char *text = names_out(&st, "--map %s/m.blif.names --lookup %s", dir, fields.short_name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, log_text);
    TEST_ASSERT_NOT_NULL(strstr(text, fields.short_name));
    TEST_ASSERT_NOT_NULL(strstr(text, fields.full));
    TEST_ASSERT_NOT_NULL(strstr(text, "\tnet\t"));
    odin3_util_free(text);
    text = names_out(&st, "--map %s/m.blif.names --reverse %s", dir, fields.full);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, log_text);
    char expect[ARGS_BUF + 2];
    (void)snprintf(expect, sizeof expect, "%s\n", fields.short_name);
    TEST_ASSERT_EQUAL_STRING(expect, text);
    odin3_util_free(text);
}

static void test_names_absent_fails(void) {
    (void)written_map();
    odin3_status st = ODIN3_OK;
    log_text[0] = '\0';
    char *text = names_out(&st, "--map %s/m.blif.names --lookup hzzzzzzzzzz", dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, st);
    TEST_ASSERT_EQUAL_STRING("", text);
    odin3_util_free(text);
    text = names_out(&st, "--map %s/m.blif.names --reverse nosuch@x.v:1", dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, st);
    odin3_util_free(text);
}

static void check_corrupted(void);

static void test_names_check(void) {
    (void)written_map();
    odin3_status st = ODIN3_OK;
    char *text = names_out(&st, "--map %s/m.blif.names --check", dir);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, log_text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "bad_hash=0 duplicates=0 ignored=0"), text);
    TEST_ASSERT_NOT_NULL(strstr(text, "entries=2 "));
    odin3_util_free(text);
    check_corrupted();
}

/* A corrupted entry: the check reports it and fails. */
static void check_corrupted(void) {
    odin3_status st = ODIN3_OK;
    char *text = NULL;
    char *map = slurp("m.blif.names");
    char *at = strstr(map, "\th");
    TEST_ASSERT_NOT_NULL(at);
    at[2] = at[2] == 'a' ? 'b' : 'a'; /* the short name is no longer the hash of the full name */
    put_file("bad.names", odin3_bytes_cstr(map));
    odin3_util_free(map);
    text = names_out(&st, "--map %s/bad.names --check", dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, st);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "ignored=1"), text);
    odin3_util_free(text);
}

static void test_names_arguments(void) {
    odin3_status st = ODIN3_OK;
    char *text = names_out(&st, "--lookup x");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, st); /* no --map */
    odin3_util_free(text);
    text = names_out(&st, "--map %s/none.names --check", dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, st);
    odin3_util_free(text);
    put_file("one.names", odin3_bytes_cstr("# odin3-names 1 hash=sha256 width=10\n"));
    text = names_out(&st, "--map %s/one.names --check --lookup x", dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, st); /* one action only */
    odin3_util_free(text);
    text = names_out(&st, "--map %s/one.names", dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, st); /* an action is needed */
    odin3_util_free(text);
    text = names_out(&st, "--map %s/one.names --check", dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_NOT_NULL(strstr(text, "entries=0 "));
    odin3_util_free(text);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_blif_hash);
    RUN_TEST(test_verilog_hash);
    RUN_TEST(test_json_hash);
    RUN_TEST(test_explicit_map_path);
    RUN_TEST(test_refused_without_map);
    RUN_TEST(test_map_commit_failure);
    RUN_TEST(test_map_is_output_path);
    RUN_TEST(test_refused_stdout_without_map);
    RUN_TEST(test_map_needs_hash);
    RUN_TEST(test_no_map_without_hash);
    RUN_TEST(test_json_default_unchanged);
    RUN_TEST(test_names_lookup_reverse);
    RUN_TEST(test_names_absent_fails);
    RUN_TEST(test_names_check);
    RUN_TEST(test_names_arguments);
    return UNITY_END();
}
