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
#include <stdarg.h>
#include <fcntl.h>
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
    (void)snprintf(out, PATH_BUF, "%s/%s", dir, name);
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

static void put_file(const char *name, const char *text) {
    char path[PATH_BUF];
    path_of(name, path);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* A top with a located net `w` and a located cell on it, a hierarchy in a second located net. */
static const char *const INPUT = ".model top\n.inputs a b\n.outputs y\n"
                                 ".names a b top/u1/w@d.v:3\n11 1\n.cname g1\n"
                                 ".names top/u1/w@d.v:3 v@d.v:4\n1 1\n"
                                 ".names v@d.v:4 y\n1 1\n.end\n";

static void load_design(void) {
    char path[PATH_BUF];
    put_file("in.blif", INPUT);
    path_of("in.blif", path);
    odin3_blif_read_stats stats = {0};
    odin3_blif_read_opts opts = {.hdl_locs = true, .stats = &stats};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read_ex(design, NULL, path, &opts),
                                  log_text);
}

/* Runs `pass <args with %s = dir>`. */
static odin3_status run(const char *pass, const char *format, ...) __attribute__((format(printf, 2, 3)));
static odin3_status run(const char *pass, const char *format, ...) {
    char args[ARGS_BUF];
    va_list ap;
    va_start(ap, format);
    (void)vsnprintf(args, sizeof args, format, ap);
    va_end(ap);
    return odin3_pass_run(design, pass, odin3_bytes_cstr(args));
}

/* --- the writers ------------------------------------------------------------------------------ */

typedef struct writer_case {
    const char *pass;
    const char *flags; /* before the path */
    const char *ext;
} writer_case;

static const writer_case WRITERS[] = {
    {"write_blif", "--vtr", ".blif"},
    {"write_verilog", "", ".v"},
    {"write_json", "", ".json"},
};
enum { N_WRITERS = 3, HASH_CHARS = 11 };

static odin3_status write_hash(const writer_case *wc, const char *base, const char *extra) {
    char dirs[PATH_BUF];
    (void)snprintf(dirs, sizeof dirs, "%s", dir);
    return run(wc->pass, "%s --name-style hash %s %s/%s%s", wc->flags, extra, dirs, base, wc->ext);
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

/* Calls check(short_name) for the third field of every data line. */
typedef void (*short_check)(const char *short_name, const char *line, void *user);
static void each_entry(const char *map, short_check check, void *user) {
    for (const char *at = map; *at != '\0';) {
        const char *end = strchr(at, '\n');
        end = end != NULL ? end : at + strlen(at);
        if (end > at && *at != '#') {
            char line[LOG_MAX];
            TEST_ASSERT_TRUE((size_t)(end - at) < sizeof line);
            memcpy(line, at, (size_t)(end - at));
            line[end - at] = '\0';
            const char *tab1 = strchr(line, '\t');
            const char *tab2 = tab1 != NULL ? strchr(tab1 + 1, '\t') : NULL;
            const char *tab3 = tab2 != NULL ? strchr(tab2 + 1, '\t') : NULL;
            TEST_ASSERT_NOT_NULL_MESSAGE(tab3, line);
            char shorts[ARGS_BUF];
            memcpy(shorts, tab2 + 1, (size_t)(tab3 - tab2 - 1));
            shorts[tab3 - tab2 - 1] = '\0';
            check(shorts, line, user);
        }
        at = *end != '\0' ? end + 1 : end;
    }
}

static void expect_hash_shape(const char *short_name, const char *line, void *user) {
    (void)line;
    const char *netlist = user;
    TEST_ASSERT_EQUAL_size_t_MESSAGE(HASH_CHARS, strlen(short_name), short_name);
    TEST_ASSERT_EQUAL_CHAR('h', short_name[0]);
    for (size_t i = 1; i < HASH_CHARS; i++) {
        TEST_ASSERT_TRUE_MESSAGE((short_name[i] >= 'a' && short_name[i] <= 'z') ||
                                     (short_name[i] >= '2' && short_name[i] <= '7'),
                                 short_name);
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(netlist, short_name), short_name);
}

static void test_writer_hash(const writer_case *wc) {
    load_design();
    char name1[SMALL];
    char name2[SMALL];
    (void)snprintf(name1, sizeof name1, "one%s", wc->ext);
    (void)snprintf(name2, sizeof name2, "two%s", wc->ext);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, write_hash(wc, "one", ""), log_text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, write_hash(wc, "two", ""), log_text);
    char map1[SMALL + 8];
    char map2[SMALL + 8];
    (void)snprintf(map1, sizeof map1, "%s.names", name1);
    (void)snprintf(map2, sizeof map2, "%s.names", name2);
    char *net1 = slurp(name1);
    char *net2 = slurp(name2);
    char *mapped1 = slurp(map1);
    char *mapped2 = slurp(map2);
    /* deterministic: byte-equal netlist and map (the map holds no path) */
    TEST_ASSERT_EQUAL_STRING(net1, net2);
    TEST_ASSERT_EQUAL_STRING(mapped1, mapped2);
    TEST_ASSERT_TRUE_MESSAGE(strncmp(mapped1, "# odin3-names 1 hash=sha256 width=10\n", 37) == 0,
                             mapped1);
    TEST_ASSERT_TRUE_MESSAGE(map_lines(mapped1) >= 2, mapped1);
    each_entry(mapped1, expect_hash_shape, net1);
    /* the provenance form is gone from the netlist; the ports are not renamed */
    TEST_ASSERT_NULL_MESSAGE(strstr(net1, "@d.v"), net1);
    TEST_ASSERT_NOT_NULL(strstr(mapped1, "top/u1/w@d.v:3"));
    TEST_ASSERT_NOT_NULL(strstr(mapped1, "\tnet\t"));
    odin3_util_free(net1);
    odin3_util_free(net2);
    odin3_util_free(mapped1);
    odin3_util_free(mapped2);
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
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, write_hash(wc, "o", extra), log_text);
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
        TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, write_hash(wc, "x", extra));
        char outname[PATH_BUF];
        (void)snprintf(outname, sizeof outname, "x%s", wc->ext);
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
        odin3_status st = run(wc->pass, "%s --name-style hash -", wc->flags);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_INVALID_ARG, st, log_text);
        TEST_ASSERT_NOT_NULL(strstr(log_text, "--name-map"));
        odin3_design_destroy(design);
        design = odin3_design_create();
    }
}

/* --name-map without hash is refused; provenance and short styles write no map. */
static void test_map_only_with_hash(void) {
    load_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          run("write_verilog", "%s/p.v --name-map %s/p.map", dir, dir));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_verilog", "%s/p.v --name-style provenance", dir));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_json", "%s/p.json --name-style provenance", dir));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_json", "%s/s.json --name-style short", dir));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          run("write_blif", "--vtr --name-style provenance %s/p.blif", dir));
    TEST_ASSERT_FALSE(exists("p.v.names"));
    TEST_ASSERT_FALSE(exists("p.json.names"));
    TEST_ASSERT_FALSE(exists("p.blif.names"));
    char *prov = slurp("p.json");
    TEST_ASSERT_NOT_NULL(strstr(prov, "top/u1/w@d.v:3"));
    odin3_util_free(prov);
}

/* JSON without the option is as before: the IR names, no provenance names. */
static void test_json_default_unchanged(void) {
    load_design();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_json", "%s/a.json", dir));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("write_json", "%s/b.json --name-style short", dir));
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

static void first_entry(const char *short_name, const char *line, void *user) {
    entry_fields *fields = user;
    if (fields->short_name[0] != '\0') {
        return;
    }
    (void)snprintf(fields->short_name, sizeof fields->short_name, "%s", short_name);
    const char *tab = line;
    for (int i = 0; i < 3; i++) {
        tab = strchr(tab, '\t') + 1;
    }
    const char *end = strchr(tab, '\t');
    (void)snprintf(fields->full, sizeof fields->full, "%.*s", (int)(end - tab), tab);
}

static entry_fields written_map(void) {
    load_design();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, run("write_blif", "--vtr --name-style hash %s/m.blif", dir), log_text);
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

static void test_names_check(void) {
    (void)written_map();
    odin3_status st = ODIN3_OK;
    char *text = names_out(&st, "--map %s/m.blif.names --check", dir);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, log_text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "bad_hash=0 duplicates=0 ignored=0"), text);
    TEST_ASSERT_NOT_NULL(strstr(text, "entries=2 "));
    odin3_util_free(text);
    /* a corrupted entry: the check reports it and fails */
    char *map = slurp("m.blif.names");
    char *at = strstr(map, "\th");
    TEST_ASSERT_NOT_NULL(at);
    at[2] = at[2] == 'a' ? 'b' : 'a'; /* the short name is no longer the hash of the full name */
    put_file("bad.names", map);
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
    put_file("one.names", "# odin3-names 1 hash=sha256 width=10\n");
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
    RUN_TEST(test_refused_stdout_without_map);
    RUN_TEST(test_map_only_with_hash);
    RUN_TEST(test_json_default_unchanged);
    RUN_TEST(test_names_lookup_reverse);
    RUN_TEST(test_names_absent_fails);
    RUN_TEST(test_names_check);
    RUN_TEST(test_names_arguments);
    return UNITY_END();
}
