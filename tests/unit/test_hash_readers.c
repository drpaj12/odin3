/*
 * test_hash_readers.c — `--name-map` in read_blif, read_verilog -netlist and read_json (spec 5.4
 * section 13: H-6; T-hash-2, T-hash-4). The design is a BLIF read with --hdl-locs, so its nets
 * and cells carry HDL locations and hierarchy. Every case works in its own mkdtemp directory under
 * the build tree.
 */
#include "frontends/blif/reader.h"
#include "ir/design.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <dirent.h>
#include <locale.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ODIN3_HASHR_ROOT
#error "ODIN3_HASHR_ROOT must name the build tree"
#endif

enum { PATH_BUF = 512, DIR_BUF = 256, CHUNK = 4096, LOG_MAX = 8192, ARGS_BUF = 1100 };

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

typedef struct file_pair {
    const char *from, *to;
} file_pair;

static void path_of(const char *name, char *out) {
    (void)snprintf(out, PATH_BUF, "%s/%.240s", dir, name);
}

void setUp(void) {
    (void)snprintf(dir, sizeof dir, "%s/hashr.XXXXXX", ODIN3_HASHR_ROOT);
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

/* A top with a located net `w` under a hierarchy, a located cell, and a second located net. */
static const char *const INPUT = ".model top\n.inputs a b\n.outputs y\n"
                                 ".names a b top/u1/w@d.v:3\n11 1\n.cname g1\n"
                                 ".names top/u1/w@d.v:3 v@d.v:4\n1 1\n"
                                 ".names v@d.v:4 y\n1 1\n.end\n";
/* The same design, its nets met in another order. */
static const char *const INPUT_SWAPPED = ".model top\n.inputs a b\n.outputs y\n"
                                         ".names v@d.v:4 y\n1 1\n"
                                         ".names a b top/u1/w@d.v:3\n11 1\n.cname g1\n"
                                         ".names top/u1/w@d.v:3 v@d.v:4\n1 1\n.end\n";

static void load_text(const char *text) {
    char path[PATH_BUF];
    put_file("in.blif", odin3_bytes_cstr(text));
    path_of("in.blif", path);
    odin3_blif_read_stats stats = {0};
    odin3_blif_read_opts opts = {.hdl_locs = true, .stats = &stats};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read_ex(design, NULL, path, &opts),
                                  log_text);
}

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

/* Throws the design away for an empty one, as a second read needs. */
static void fresh_design(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

/* --- the three formats ------------------------------------------------------------------------ */

typedef struct format_case {
    const char *write_pass;
    const char *write_flags; /* before the options */
    const char *read_pass;
    const char *read_flags; /* before the map option */
    const char *map_flag;
    const char *ext;
} format_case;

static const format_case FORMATS[] = {
    {"write_blif", "--vtr", "read_blif", "--hdl-locs", "--name-map", ".blif"},
    {"write_verilog", "", "read_verilog", "-netlist", "-name-map", ".v"},
    {"write_json", "", "read_json", "", "--name-map", ".json"},
};
enum { N_FORMATS = 3 };

static void write_style(const format_case *fc, const char *style, const char *base) {
    run_ok(fc->write_pass,
           fmt_args("%s --name-style %s %s/%s%s", fc->write_flags, style, dir, base, fc->ext));
}

static odin3_status read_with_map(const format_case *fc, const char *base, const char *map) {
    return run(fc->read_pass, fmt_args("%s %s %s/%s %s/%s%s", fc->read_flags, fc->map_flag, dir,
                                       map, dir, base, fc->ext));
}

static void expect_same_files(const char *left, const char *right) {
    char *one = slurp(left);
    char *two = slurp(right);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(one, two, log_text);
    odin3_util_free(one);
    odin3_util_free(two);
}

/* Reads base (a provenance write) with no map and writes it again under the name `again`. */
static void reread_provenance(const format_case *fc, const file_pair *names) {
    fresh_design();
    run_ok(fc->read_pass, fmt_args("%s %s/%s%s", fc->read_flags, dir, names->from, fc->ext));
    write_style(fc, "provenance", names->to);
}

/* JSON: the design after one write and read of the format (R-1: the first read reorders some
 * parameters, which is no matter of names; the JSON reader keeps no hierarchy of a name). */
static void settle_json(void) {
    run_ok("write_json", fmt_args("%s/settle.json", dir));
    fresh_design();
    run_ok("read_json", fmt_args("%s/settle.json", dir));
}

/*
 * T-hash-4: write hash, read with the map, write provenance == the direct provenance write. BLIF
 * compares with the direct write itself. Verilog does not read back to the writer's own text for
 * reasons that have nothing to do with names (assign shapes), so it compares with the write of the
 * read of a provenance write (H-6: the rest of the read is exactly that). JSON compares with the
 * direct write of a design that went through the reader once (settle_json).
 */
static void hash_roundtrip(const format_case *fc) {
    load_text(INPUT);
    if (fc == &FORMATS[2]) {
        settle_json();
    }
    write_style(fc, "provenance", "direct");
    write_style(fc, "hash", "hashed");
    fresh_design();
    char map[PATH_BUF];
    (void)snprintf(map, sizeof map, "hashed%s.names", fc->ext);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, read_with_map(fc, "hashed", map), log_text);
    write_style(fc, "provenance", "back");
    char back[PATH_BUF];
    char want[PATH_BUF];
    (void)snprintf(back, sizeof back, "back%s", fc->ext);
    if (fc == &FORMATS[1]) {
        reread_provenance(fc, &(file_pair){"direct", "plain"});
        (void)snprintf(want, sizeof want, "plain%s", fc->ext);
    } else {
        (void)snprintf(want, sizeof want, "direct%s", fc->ext);
    }
    expect_same_files(want, back);
}

/* The hierarchy of a hash name survives the JSON read (the key is the full name again). */
static void test_json_hier(void) {
    const format_case *fc = &FORMATS[2];
    load_text(INPUT);
    write_style(fc, "hash", "hashed");
    fresh_design();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, read_with_map(fc, "hashed", "hashed.json.names"),
                                  log_text);
    write_style(fc, "provenance", "back");
    char *back = slurp("back.json");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(back, "\"top/u1/w@d.v:3\""), back);
    odin3_util_free(back);
}

static void test_blif_roundtrip(void) {
    hash_roundtrip(&FORMATS[0]);
}

static void test_verilog_roundtrip(void) {
    hash_roundtrip(&FORMATS[1]);
}

static void test_json_roundtrip(void) {
    hash_roundtrip(&FORMATS[2]);
}

/* Without the option a hash name is an ordinary name (T-5). */
static void test_no_option_unchanged(void) {
    const format_case *fc = &FORMATS[0];
    load_text(INPUT);
    write_style(fc, "hash", "hashed");
    fresh_design();
    run_ok("read_blif", fmt_args("--hdl-locs %s/hashed.blif", dir));
    write_style(fc, "provenance", "plain");
    char *plain = slurp("plain.blif");
    char *hashed = slurp("hashed.blif");
    TEST_ASSERT_NULL_MESSAGE(strstr(plain, "@d.v"), plain);
    /* the first line is the writer's comment, which names the style */
    TEST_ASSERT_EQUAL_STRING(strchr(hashed, '\n'), strchr(plain, '\n'));
    TEST_ASSERT_EQUAL_STRING("", log_text);
    odin3_util_free(plain);
    odin3_util_free(hashed);
}

/* The first data line of a map, without it, into a new map file. */
static void drop_first_entry(const file_pair *files) {
    char *text = slurp(files->from);
    char *header_end = strchr(text, '\n');
    TEST_ASSERT_NOT_NULL(header_end);
    char *entry_end = strchr(header_end + 1, '\n');
    TEST_ASSERT_NOT_NULL(entry_end);
    *header_end = '\0';
    char kept[LOG_MAX];
    (void)snprintf(kept, sizeof kept, "%s\n%s", text, entry_end + 1);
    put_file(files->to, odin3_bytes_cstr(kept));
    odin3_util_free(text);
}

/* A hash name the map lacks stays and is counted: one WARNING with the count and the first. */
static void expect_log_has(const char *text) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, text), log_text);
}

static void expect_miss_warning(const format_case *fc) {
    char file[PATH_BUF];
    expect_log_has("hash-shaped name");
    expect_log_has("not in the name map");
    expect_log_has("first: 'h");
    (void)snprintf(file, sizeof file, "hashed%s:", fc->ext);
    expect_log_has(file);
}

static void missing_case(const format_case *fc) {
    char map[PATH_BUF];
    load_text(INPUT);
    write_style(fc, "hash", "hashed");
    (void)snprintf(map, sizeof map, "hashed%s.names", fc->ext);
    drop_first_entry(&(file_pair){map, "short.names"});
    fresh_design();
    log_text[0] = '\0';
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, read_with_map(fc, "hashed", "short.names"), log_text);
    expect_miss_warning(fc);
}

static void test_missing_entry_warns(void) {
    for (int i = 0; i < N_FORMATS; i++) {
        missing_case(&FORMATS[i]);
        fresh_design();
    }
    /* BLIF: the dropped net is used twice */
    load_text(INPUT);
    write_style(&FORMATS[0], "hash", "again");
    drop_first_entry(&(file_pair){"again.blif.names", "short2.names"});
    fresh_design();
    log_text[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_with_map(&FORMATS[0], "again", "short2.names"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "2 use(s) of hash-shaped names"), log_text);
}

/* A map of an unknown version is an error, a missing map file too. */
static void test_bad_map(void) {
    const format_case *fc = &FORMATS[0];
    load_text(INPUT);
    write_style(fc, "hash", "hashed");
    put_file("v9.names", odin3_bytes_cstr("# odin3-names 9 hash=sha256 width=10\n"));
    fresh_design();
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, read_with_map(fc, "hashed", "v9.names"));
    fresh_design();
    log_text[0] = '\0';
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, read_with_map(fc, "hashed", "absent.names"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, "cannot read the name map"), log_text);
}

/* -name-map needs -netlist; every reader wants a file after the option. */
static void test_option_errors(void) {
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, run("read_verilog", fmt_args("-name-map x.names y.v")));
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, run("read_blif", fmt_args("%s/x.blif --name-map", dir)));
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, run("read_json", fmt_args("%s/x.json --name-map", dir)));
}

/* --- T-hash-2: determinism ---------------------------------------------------------------------
 */

typedef struct pair {
    char *net, *map;
} pair;

static pair write_pair(const format_case *fc, const char *base) {
    write_style(fc, "hash", base);
    char net[PATH_BUF];
    char map[PATH_BUF];
    (void)snprintf(net, sizeof net, "%s%s", base, fc->ext);
    (void)snprintf(map, sizeof map, "%s%s.names", base, fc->ext);
    return (pair){slurp(net), slurp(map)};
}

static void free_pair(pair *files) {
    odin3_util_free(files->net);
    odin3_util_free(files->map);
}

/* Two orders of construction give one map (the names depend on the set of full names only). */
static void test_construction_order(void) {
    for (int i = 0; i < N_FORMATS; i++) {
        load_text(INPUT);
        pair one = write_pair(&FORMATS[i], "one");
        fresh_design();
        load_text(INPUT_SWAPPED);
        pair two = write_pair(&FORMATS[i], "two");
        TEST_ASSERT_EQUAL_STRING_MESSAGE(one.map, two.map, FORMATS[i].write_pass);
        free_pair(&one);
        free_pair(&two);
        fresh_design();
    }
}

static const char *const LOCALES[] = {"C", "C.utf8", "de_DE.UTF-8"};
enum { N_LOCALES = 3 };

/* Writes the design under every installed locale: the first write is kept, the others must equal
 * it. Returns how many locales were installed. */
static uint32_t write_under_locales(const format_case *fc) {
    pair first = {NULL, NULL};
    uint32_t used = 0;
    for (size_t k = 0; k < N_LOCALES; k++) {
        if (setlocale(LC_ALL, LOCALES[k]) == NULL) {
            continue; /* not installed here */
        }
        used++;
        load_text(INPUT);
        pair now = write_pair(fc, "loc");
        if (first.net == NULL) {
            first = now;
        } else {
            TEST_ASSERT_EQUAL_STRING(first.net, now.net);
            TEST_ASSERT_EQUAL_STRING(first.map, now.map);
            free_pair(&now);
        }
        fresh_design();
    }
    free_pair(&first);
    return used;
}

/* The same design under three locales: byte-equal netlist and map. */
static void test_locales(void) {
    char *saved = strdup(setlocale(LC_ALL, NULL));
    TEST_ASSERT_NOT_NULL(saved);
    uint32_t used = 0;
    for (int i = 0; i < N_FORMATS; i++) {
        used += write_under_locales(&FORMATS[i]);
    }
    (void)setlocale(LC_ALL, saved);
    free(saved);
    TEST_ASSERT_TRUE(used >= N_FORMATS);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_blif_roundtrip);
    RUN_TEST(test_verilog_roundtrip);
    RUN_TEST(test_json_roundtrip);
    RUN_TEST(test_json_hier);
    RUN_TEST(test_no_option_unchanged);
    RUN_TEST(test_missing_entry_warns);
    RUN_TEST(test_bad_map);
    RUN_TEST(test_option_errors);
    RUN_TEST(test_construction_order);
    RUN_TEST(test_locales);
    return UNITY_END();
}
