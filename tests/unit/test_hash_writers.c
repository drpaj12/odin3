/*
 * test_hash_writers.c — `--name-style hash` in write_blif (VTR), write_verilog and write_json, and
 * the `names` pass (spec 5.4 section 13: H-1, H-2, H-4, H-5; T-hash-6). Every case works in its own
 * mkdtemp directory under the build tree. The design is a BLIF read with --hdl-locs, so its nets
 * and cells carry HDL locations.
 */
#include "backends/common/hash_rows.h"
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

static void load_text(const char *text) {
    char path[PATH_BUF];
    put_file("in.blif", odin3_bytes_cstr(text));
    path_of("in.blif", path);
    odin3_blif_read_stats stats = {0};
    odin3_blif_read_opts opts = {.hdl_locs = true, .stats = &stats};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read_ex(design, NULL, path, &opts),
                                  log_text);
}

static void load_design(void) {
    load_text(INPUT);
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

/* Two nets whose provenance text is equal (`u1.w` under hierarchy top/u1 is `w` there): both get
 * a hash row, the second from the full name with the `$N` suffix (H-2), and the result does not
 * depend on which net was named first (H-3). */
static void test_blif_equal_provenance(void) {
    load_text(".model top\n.inputs a b\n.outputs y\n"
              ".names a b top/u1/w@d.v:3\n11 1\n"
              ".names top/u1/w@d.v:3 top/u1/u1.w@d.v:3\n1 1\n"
              ".names top/u1/u1.w@d.v:3 y\n1 1\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, write_hash(&WRITERS[0], "eq"), log_text);
    written one = read_written(&WRITERS[0], "eq");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, map_lines(one.map), one.map);
    TEST_ASSERT_NULL_MESSAGE(strstr(one.net, "$n"), one.net);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(one.map, "w@d.v:3\t"), one.map);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(one.map, "w@d.v:3$1\t"), one.map);
    free_written(&one);
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
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, st); /* absent: not a usage error */
    TEST_ASSERT_EQUAL_STRING("", text);
    odin3_util_free(text);
    text = names_out(&st, "--map %s/m.blif.names --reverse nosuch@x.v:1", dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, st);
    odin3_util_free(text);
}

/* A full name listed twice in a map prints its short name once. */
static void test_names_reverse_once(void) {
    entry_fields fields = written_map();
    char *map = slurp("m.blif.names");
    size_t len = strlen(map);
    char *twice = odin3_util_malloc(2 * len + 1);
    (void)snprintf(twice, 2 * len + 1, "%s%s", map, map);
    put_file("dup.names", (odin3_bytes){twice, 2 * len});
    odin3_util_free(twice);
    odin3_util_free(map);
    odin3_status st = ODIN3_OK;
    char *text = names_out(&st, "--map %s/dup.names --reverse %s", dir, fields.full);
    char expect[ARGS_BUF + 2];
    (void)snprintf(expect, sizeof expect, "%s\n", fields.short_name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, log_text);
    TEST_ASSERT_EQUAL_STRING(expect, text);
    odin3_util_free(text);
}

/* --- more modules, one namespace per module, the width hook, the JSON passes --------------------
 */

enum { KEY_MAX = 128, SEEN_MAX = 512, CHAIN = 40 };

/* The `module<TAB>short` pairs of a map, and the number of repeats and of long names. */
typedef struct seen_pairs {
    char pair[SEEN_MAX][KEY_MAX];
    uint32_t count;
    uint32_t repeats;
    uint32_t longer; /* hash names longer than the starting width (a collision extended them) */
    uint32_t start;  /* 1 + the starting width */
} seen_pairs;

static void note_pair(const entry_view *entry, void *user) {
    seen_pairs *seen = user;
    const char *tab = strchr(entry->line, '\t');
    TEST_ASSERT_NOT_NULL(tab);
    TEST_ASSERT_TRUE(seen->count < SEEN_MAX);
    (void)snprintf(seen->pair[seen->count], KEY_MAX, "%.*s\t%s", (int)(tab - entry->line),
                   entry->line, entry->short_name);
    for (uint32_t i = 0; i < seen->count; i++) {
        seen->repeats += strcmp(seen->pair[i], seen->pair[seen->count]) == 0 ? 1U : 0U;
    }
    seen->longer += strlen(entry->short_name) > seen->start ? 1U : 0U;
    seen->count++;
}

/* A chain of CHAIN located nets, each driving the next, and a named located cell on every one. */
static void load_chain(const char *module_head) {
    char *text = odin3_util_calloc(CHAIN * 160 + 256);
    size_t len = (size_t)sprintf(text, "%s.inputs a\n.outputs y\n", module_head);
    const char *prev = "a";
    char prev_buf[SMALL];
    for (int i = 0; i < CHAIN; i++) {
        char net[SMALL];
        (void)snprintf(net, sizeof net, "top/u1/n%d@d.v:%d", i, i + 1);
        len += (size_t)sprintf(text + len, ".names %s %s\n1 1\n.cname g%d\n", prev, net, i);
        (void)snprintf(prev_buf, sizeof prev_buf, "%s", net);
        prev = prev_buf;
    }
    (void)sprintf(text + len, ".names %s y\n1 1\n.end\n", prev);
    load_text(text);
    odin3_util_free(text);
}

/* The map of a narrow write: header width 1, unique pairs, some name extended by a collision. */
static void expect_narrow_map(const char *map) {
    seen_pairs *seen = odin3_util_calloc(sizeof *seen);
    seen->start = 2; /* `h` and one character */
    each_entry(map, note_pair, seen);
    uint32_t count = seen->count;
    uint32_t repeats = seen->repeats;
    uint32_t longer = seen->longer;
    odin3_util_free(seen);
    TEST_ASSERT_TRUE_MESSAGE(count >= CHAIN, map);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, repeats, map);
    TEST_ASSERT_TRUE_MESSAGE(longer > 0, "no collision extended a name");
}

/* T-hash-3 through a writer: a start width of 1 forces collisions; the names still come out
 * unique per module (all kinds together in Verilog and JSON) and the map still checks. */
static void collision_case(const writer_case *wc) {
    load_chain(".model top\n");
    odin3_hash_rows_test_start(1);
    odin3_status st = write_hash(wc, "narrow");
    odin3_hash_rows_test_start(0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, log_text);
    written one = read_written(wc, "narrow");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(one.map, "width=1\n"), one.map);
    expect_narrow_map(one.map);
    odin3_status check = ODIN3_OK;
    char *text = names_out(&check, "--map %s/narrow%s.names --check", dir, wc->ext);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, check, text);
    TEST_ASSERT_NOT_NULL(strstr(text, "bad_hash=0 duplicates=0 ignored=0"));
    odin3_util_free(text);
    free_written(&one);
}

static void test_collision_width(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        collision_case(&WRITERS[i]);
        odin3_design_destroy(design);
        design = odin3_design_create();
    }
}

/* The hash name of the first map line that starts with `prefix` (HASH_CHARS bytes, not a copy). */
static const char *hash_after(const char *map, const char *prefix) {
    const char *at = strstr(map, prefix);
    TEST_ASSERT_NOT_NULL_MESSAGE(at, map);
    return at + strlen(prefix);
}

static void two_modules_case(const writer_case *wc) {
    load_text(".model sub\n.inputs a\n.outputs y\n"
              ".names a top/u1/w@d.v:3\n1 1\n.cname g1\n.names top/u1/w@d.v:3 y\n1 1\n.end\n"
              ".model top\n.inputs a\n.outputs y\n"
              ".names a top/u1/w@d.v:3\n1 1\n.cname g1\n"
              ".subckt sub a=top/u1/w@d.v:3 y=y\n.end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, write_hash(wc, "two"), log_text);
    written one = read_written(wc, "two");
    TEST_ASSERT_NULL_MESSAGE(strstr(one.net, "@d.v"), one.net);
    const char *sub = hash_after(one.map, "\nsub\tnet\t");
    const char *top = hash_after(one.map, "\ntop\tnet\t");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(sub, top, HASH_CHARS), one.map);
    free_written(&one);
}

/* More than one module: Verilog and JSON write every module, each with its own map lines, and the
 * same full name in two modules gives the same hash name (H-2). */
static void test_two_modules(void) {
    for (int i = 1; i < N_WRITERS; i++) {
        two_modules_case(&WRITERS[i]);
        odin3_design_destroy(design);
        design = odin3_design_create();
    }
}

/* The two JSON passes agree when a plain key equals another object's provenance name: the user net
 * keeps its key (bumped past the provenance name as in the first pass), the located one gets its
 * hash name, and the second pass finds the rows of the first in step. */
static void test_json_plain_key_equals_provenance(void) {
    load_design();
    odin3_module *top = odin3_module_get(design, odin3_design_top(design));
    TEST_ASSERT_NOT_NULL(top);
    uint32_t name = 0;
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(design, odin3_bytes_cstr("top/u1/w@d.v:3"), &name));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(top, name, (odin3_prov_id){0}, &net));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, write_hash(&WRITERS[2], "plain"), log_text);
    written one = read_written(&WRITERS[2], "plain");
    TEST_ASSERT_NULL_MESSAGE(strstr(log_text, "out of step"), log_text);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(WRITERS[2].entries, map_lines(one.map), one.map);
    free_written(&one);
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

/* --- --prov-map: the version 2 map (V1.2.0 3.5) ---------------------------------------------- */

/* Parses the map text into map (fresh) and expects a clean version 2 map. */
static void parse_v2(odin3_name_map *map, const char *text) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_init(map));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_name_map_parse(map, odin3_bytes_cstr(text), "test.names"), log_text);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, map->ignored, text);
    TEST_ASSERT_EQUAL_UINT32(2, map->version);
}

/* Every O line of the version 2 map carries the record of its object. */
static void expect_records_on_o_lines(const odin3_name_map *map, const char *pass) {
    for (size_t k = 0; k < map->lines.len; k++) {
        const odin3_nm_line *line = odin3_vec_cat(&map->lines, k);
        if (line->type == 'O') {
            TEST_ASSERT_TRUE_MESSAGE(line->f[7].len > 1, pass);
            TEST_ASSERT_EQUAL_CHAR('r', ((const char *)line->f[7].ptr)[0]);
        }
    }
}

/* The O lines of the version 2 map are the version 1 lines: same short and full names, in order. */
static void expect_same_names(const odin3_name_map *first, const odin3_name_map *second) {
    TEST_ASSERT_EQUAL_UINT(first->entries.len, second->entries.len);
    for (size_t k = 0; k < first->entries.len; k++) {
        const odin3_name_entry *want = odin3_vec_cat(&first->entries, k);
        const odin3_name_entry *got = odin3_vec_cat(&second->entries, k);
        TEST_ASSERT_EQUAL_size_t(want->short_name.len, got->short_name.len);
        TEST_ASSERT_EQUAL_MEMORY(want->short_name.ptr, got->short_name.ptr, want->short_name.len);
        TEST_ASSERT_EQUAL_size_t(want->full.len, got->full.len);
        TEST_ASSERT_EQUAL_MEMORY(want->full.ptr, got->full.ptr, want->full.len);
    }
}

static void check_version_2_pair(const writer_case *wc, const written *one, const written *two) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(one->net, two->net, wc->pass); /* the netlist is unchanged */
    TEST_ASSERT_EQUAL_INT(0, strncmp(one->map, "# odin3-names 1 ", 16));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, strncmp(two->map, "# odin3-names 2 hash=sha256 width=10 prov=1 ast=none\n", 53),
        two->map);
    odin3_name_map map;
    parse_v2(&map, two->map);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(wc->entries, map.entries.len, two->map);
    odin3_name_check_v2 check = odin3_name_map_check_v2(&map);
    TEST_ASSERT_TRUE_MESSAGE(odin3_name_check_v2_ok(&check), two->map);
    TEST_ASSERT_TRUE(check.records > 0);
    expect_records_on_o_lines(&map, wc->pass);
    odin3_name_map first;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_name_map_init(&first));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_name_map_parse(&first, odin3_bytes_cstr(one->map), "v1.names"));
    expect_same_names(&first, &map);
    odin3_name_map_free(&first);
    odin3_name_map_free(&map);
}

static void reset_design(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
}

static void test_prov_map_writes_version_2_beside_an_identical_netlist(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        const writer_case *wc = &WRITERS[i];
        load_design();
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, write_hash(wc, "v1"), log_text);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            ODIN3_OK, write_hash_extra(wc, "v2", odin3_bytes_cstr("--prov-map")), log_text);
        written one = read_written(wc, "v1");
        written two = read_written(wc, "v2");
        check_version_2_pair(wc, &one, &two);
        free_written(&one);
        free_written(&two);
        reset_design();
    }
}

static void expect_full_in(const odin3_name_entry *entry, const char *prov) {
    char full[SMALL * 4];
    TEST_ASSERT_TRUE(entry->full.len < sizeof full);
    memcpy(full, entry->full.ptr, entry->full.len);
    full[entry->full.len] = '\0';
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(prov, full), full);
}

/* Every `full` of the map is in the provenance-style netlist text. */
static void expect_fulls_in(const odin3_name_map *map, const char *prov) {
    TEST_ASSERT_TRUE(map->entries.len > 0);
    for (size_t k = 0; k < map->entries.len; k++) {
        expect_full_in(odin3_vec_cat(&map->entries, k), prov);
    }
}

/* 3.5: the `full` of every O line is the name --name-style provenance writes for the net. */
static void test_prov_map_full_equals_the_provenance_name(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        const writer_case *wc = &WRITERS[i];
        load_design();
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            ODIN3_OK, write_hash_extra(wc, "h", odin3_bytes_cstr("--prov-map")), log_text);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            ODIN3_OK,
            run(wc->pass, fmt_args("%s --name-style provenance %s/p%s", wc->flags, dir, wc->ext)),
            log_text);
        written hashed = read_written(wc, "h");
        char pname[SMALL];
        (void)snprintf(pname, sizeof pname, "p%s", wc->ext);
        char *prov = slurp(pname);
        odin3_name_map map;
        parse_v2(&map, hashed.map);
        expect_fulls_in(&map, prov);
        odin3_name_map_free(&map);
        odin3_util_free(prov);
        free_written(&hashed);
        reset_design();
    }
}

static void test_prov_map_needs_hash_names(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        const writer_case *wc = &WRITERS[i];
        load_design();
        log_text[0] = '\0';
        TEST_ASSERT_EQUAL_INT(
            ODIN3_ERR_INVALID_ARG,
            run(wc->pass, fmt_args("%s --prov-map %s/np%s", wc->flags, dir, wc->ext)));
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "--prov-map needs --name-style hash"),
                                     log_text);
        TEST_ASSERT_EQUAL_INT(
            ODIN3_ERR_INVALID_ARG,
            run(wc->pass, fmt_args("%s --name-style provenance --prov-map full %s/np%s", wc->flags,
                                   dir, wc->ext)));
        char name[SMALL];
        (void)snprintf(name, sizeof name, "np%s", wc->ext);
        TEST_ASSERT_FALSE(exists(name));
        odin3_design_destroy(design);
        design = odin3_design_create();
    }
}

/* No file named base + ext (+ .names) exists. */
static void expect_nothing_written(const writer_case *wc, const char *base) {
    char name[SMALL];
    (void)snprintf(name, sizeof name, "%s%s", base, wc->ext);
    TEST_ASSERT_FALSE(exists(name));
    (void)snprintf(name, sizeof name, "%s%s.names", base, wc->ext);
    TEST_ASSERT_FALSE(exists(name));
}

static void test_prov_map_above_what_the_read_kept_is_refused(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        const writer_case *wc = &WRITERS[i];
        load_design(); /* a BLIF read: no construct table was kept */
        log_text[0] = '\0';
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                              write_hash_extra(wc, "kept", odin3_bytes_cstr("--prov-map full")));
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "read_verilog --prov-ast full"), log_text);
        expect_nothing_written(wc, "kept"); /* refused before anything is written */
        reset_design();
    }
}

/* A mode word after the option is the mode; a path after it is not. */
static void test_prov_map_option_word_is_optional(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        const writer_case *wc = &WRITERS[i];
        load_design();
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            ODIN3_OK,
            run(wc->pass,
                fmt_args("--prov-map %s --name-style hash %s/w%s", wc->flags, dir, wc->ext)),
            log_text);
        char name[SMALL];
        (void)snprintf(name, sizeof name, "w%s.names", wc->ext);
        char *map = slurp(name);
        TEST_ASSERT_NOT_NULL(strstr(map, "# odin3-names 2 "));
        odin3_util_free(map);
        odin3_design_destroy(design);
        design = odin3_design_create();
    }
}

/* --prov-map given twice is refused by all three writers; nothing is written. */
static void test_prov_map_twice_is_refused(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        const writer_case *wc = &WRITERS[i];
        load_design();
        log_text[0] = '\0';
        TEST_ASSERT_EQUAL_INT(
            ODIN3_ERR_INVALID_ARG,
            write_hash_extra(wc, "twice", odin3_bytes_cstr("--prov-map refs --prov-map")));
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "given twice"), log_text);
        expect_nothing_written(wc, "twice");
        reset_design();
    }
}

/* An output in a directory that does not exist: the write fails and leaves no netlist and no map.
 */
static void test_prov_map_write_failure_leaves_nothing(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        const writer_case *wc = &WRITERS[i];
        load_design();
        TEST_ASSERT_NOT_EQUAL_INT(
            ODIN3_OK, run(wc->pass, fmt_args("%s --name-style hash --prov-map %s/nodir/x%s",
                                             wc->flags, dir, wc->ext)));
        char name[SMALL];
        (void)snprintf(name, sizeof name, "nodir/x%s", wc->ext);
        TEST_ASSERT_FALSE(exists(name));
        (void)snprintf(name, sizeof name, "nodir/x%s.names", wc->ext);
        TEST_ASSERT_FALSE(exists(name));
        reset_design();
    }
}

/* A map that cannot be renamed into place fails the write; the existing netlist stays and no
 * version 2 map appears (the same rule as version 1, with --prov-map). */
static void test_prov_map_commit_failure_keeps_the_old_netlist(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        const writer_case *wc = &WRITERS[i];
        load_design();
        char outname[SMALL];
        char path[PATH_BUF];
        char extra[PATH_BUF];
        (void)snprintf(outname, sizeof outname, "oldv2%s", wc->ext);
        put_file(outname, odin3_bytes_cstr("OLD\n"));
        path_of("mapdir2", path);
        TEST_ASSERT_EQUAL_INT(0, mkdir(path, 0700));
        (void)snprintf(extra, sizeof extra, "--prov-map --name-map %.400s", path);
        TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, write_hash_extra(wc, "oldv2", odin3_bytes_cstr(extra)));
        char *net = slurp(outname);
        TEST_ASSERT_EQUAL_STRING("OLD\n", net);
        odin3_util_free(net);
        TEST_ASSERT_EQUAL_INT(0, rmdir(path));
        reset_design();
    }
}

static uint32_t count_type(const odin3_name_map *map, char type) {
    uint32_t count = 0;
    for (size_t k = 0; k < map->lines.len; k++) {
        count += ((const odin3_nm_line *)odin3_vec_cat(&map->lines, k))->type == type ? 1U : 0U;
    }
    return count;
}

/* Each writer's version 2 map has N lines for the names it keeps (BLIF: nets only). */
static void test_prov_map_has_n_lines_from_every_writer(void) {
    for (int i = 0; i < N_WRITERS; i++) {
        const writer_case *wc = &WRITERS[i];
        load_design();
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            ODIN3_OK, write_hash_extra(wc, "nl", odin3_bytes_cstr("--prov-map")), log_text);
        written files = read_written(wc, "nl");
        odin3_name_map map;
        parse_v2(&map, files.map);
        TEST_ASSERT_TRUE_MESSAGE(count_type(&map, 'N') > 0, wc->pass);
        odin3_name_check_v2 check = odin3_name_map_check_v2(&map);
        TEST_ASSERT_TRUE_MESSAGE(odin3_name_check_v2_report(&check, wc->pass), files.map);
        odin3_name_map_free(&map);
        free_written(&files);
        reset_design();
    }
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
    RUN_TEST(test_blif_equal_provenance);
    RUN_TEST(test_refused_stdout_without_map);
    RUN_TEST(test_map_needs_hash);
    RUN_TEST(test_no_map_without_hash);
    RUN_TEST(test_json_default_unchanged);
    RUN_TEST(test_names_lookup_reverse);
    RUN_TEST(test_names_absent_fails);
    RUN_TEST(test_names_reverse_once);
    RUN_TEST(test_collision_width);
    RUN_TEST(test_two_modules);
    RUN_TEST(test_json_plain_key_equals_provenance);
    RUN_TEST(test_names_check);
    RUN_TEST(test_names_arguments);
    RUN_TEST(test_prov_map_writes_version_2_beside_an_identical_netlist);
    RUN_TEST(test_prov_map_full_equals_the_provenance_name);
    RUN_TEST(test_prov_map_needs_hash_names);
    RUN_TEST(test_prov_map_above_what_the_read_kept_is_refused);
    RUN_TEST(test_prov_map_option_word_is_optional);
    RUN_TEST(test_prov_map_twice_is_refused);
    RUN_TEST(test_prov_map_write_failure_leaves_nothing);
    RUN_TEST(test_prov_map_commit_failure_keeps_the_old_netlist);
    RUN_TEST(test_prov_map_has_n_lines_from_every_writer);
    return UNITY_END();
}
