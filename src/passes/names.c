/*
 * names.c — the `names` pass (spec 5.4 section 13, H-5): looks hashed names up in a name map.
 *
 *   names --map <file> --lookup <short>   the map line(s) of a hash name (failure when absent)
 *   names --map <file> --reverse <full>   the hash name(s) of a full name (failure when absent)
 *   names --map <file> --check            counts; failure when an entry is bad
 *   names --map <file> --chain <short>    version 2 maps: the HDL constructs of an object
 *
 * Version 2 maps (V1.2.0 3.5): --lookup prints the object's line, its records (indented by their
 * depth in the parent walk), its chain of constructs and the F and T lines they name; --chain
 * prints the chain only; --check adds the version 2 invariants (every reference resolves, parents
 * before children, a class on every record) and a second line of counts. --lookup and --chain
 * match an O line by its short name and an N line by its name. Version 1 output is unchanged.
 *
 * Status: OK; ODIN3_ERR_CHECK when --lookup or --reverse finds nothing; ODIN3_ERR_PARSE when
 * --check finds a bad entry or the map is malformed; ODIN3_ERR_INVALID_ARG for a usage error.
 * Plain text on stdout, for scripts. The pass reads only the map: it needs no design, so
 * `odin3 -c "names --map out.blif.names --lookup h..."` works with nothing loaded.
 */
#include "backends/common/name_map.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "passes/names_v2.h"
#include "util/file.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { MAP_MAX_BYTES = 1 << 30 };

typedef enum names_action { ACTION_NONE, ACTION_LOOKUP, ACTION_REVERSE, ACTION_CHECK, ACTION_CHAIN } names_action;

typedef struct names_args {
    odin3_strbuf map; /* --map's path */
    names_action action;
    odin3_bytes value; /* --lookup's or --reverse's argument, a view into the pass arguments */
} names_args;

/* Sets the action of word; one action only. */
static odin3_status set_action(names_args *out, names_action action, odin3_bytes value) {
    if (out->action != ACTION_NONE) {
        odin3_log(ODIN3_LOG_ERROR, "names: give one of --lookup, --reverse, --chain and --check");
        return ODIN3_ERR_INVALID_ARG;
    }
    out->action = action;
    out->value = value;
    return ODIN3_OK;
}

/* The word after option word (INVALID_ARG, logged, when there is none). */
static odin3_status option_value(odin3_bytes *rest, odin3_bytes word, odin3_bytes *value) {
    *value = odin3_pass_arg_next(rest);
    if (value->ptr == NULL || value->len == 0) {
        odin3_log(ODIN3_LOG_ERROR, "names: %.*s expects a value", (int)word.len,
                  (const char *)word.ptr);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

static odin3_status names_option(odin3_bytes word, odin3_bytes *rest, names_args *out) {
    odin3_bytes value = {NULL, 0};
    if (odin3_pass_arg_is(word, "--check")) {
        return set_action(out, ACTION_CHECK, value);
    }
    bool map = odin3_pass_arg_is(word, "--map");
    bool lookup = odin3_pass_arg_is(word, "--lookup");
    bool reverse = odin3_pass_arg_is(word, "--reverse");
    bool chain = odin3_pass_arg_is(word, "--chain");
    if (!map && !lookup && !reverse && !chain) {
        odin3_log(ODIN3_LOG_ERROR, "names: unknown argument '%.*s'", (int)word.len,
                  (const char *)word.ptr);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = option_value(rest, word, &value);
    if (st != ODIN3_OK) {
        return st;
    }
    if (map) {
        odin3_strbuf_clear(&out->map);
        return odin3_strbuf_append(&out->map, value);
    }
    names_action action = lookup ? ACTION_LOOKUP : (reverse ? ACTION_REVERSE : ACTION_CHAIN);
    return set_action(out, action, value);
}

static odin3_status names_parse(odin3_bytes args, names_args *out) {
    odin3_status st = ODIN3_OK;
    for (odin3_bytes word = odin3_pass_arg_next(&args); st == ODIN3_OK && word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        st = names_option(word, &args, out);
    }
    if (st == ODIN3_OK && (out->map.len == 0 || out->action == ACTION_NONE)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "names: expects --map <file> and one of --lookup <short>, --reverse <full>, "
                  "--chain <short>, --check");
        st = ODIN3_ERR_INVALID_ARG;
    }
    return st;
}

static bool same_bytes(odin3_bytes lhs, odin3_bytes rhs) {
    return lhs.len == rhs.len && (lhs.len == 0 || memcmp(lhs.ptr, rhs.ptr, lhs.len) == 0);
}

/* Reads and parses the map file (logged). */
static odin3_status load_map(const char *path, odin3_name_map *map) {
    odin3_file_bytes file = {0};
    odin3_status st = odin3_file_read_all(path, MAP_MAX_BYTES, &file);
    if (st == ODIN3_ERR_IO) {
        odin3_log(ODIN3_LOG_ERROR, "names: %s: cannot read the name map: %s", path,
                  strerror(file.err));
    } else if (st == ODIN3_ERR_PARSE) {
        odin3_log(ODIN3_LOG_ERROR, "names: %s: the name map is over %d bytes", path, MAP_MAX_BYTES);
    }
    if (st == ODIN3_ERR_NO_MEMORY) {
        odin3_log(ODIN3_LOG_ERROR, "names: %s: out of memory reading the name map", path);
    }
    st = st == ODIN3_OK ? odin3_name_map_init(map) : st;
    st = st == ODIN3_OK ? odin3_name_map_parse(map, (odin3_bytes){file.data, file.len}, path) : st;
    odin3_file_bytes_free(&file);
    return st;
}

static odin3_status write_stdout(const odin3_strbuf *text) {
    if ((text->len > 0 && fwrite(text->data, 1, text->len, stdout) != text->len) ||
        fflush(stdout) != 0) {
        odin3_log(ODIN3_LOG_ERROR, "names: cannot write to stdout");
        return ODIN3_ERR_IO;
    }
    return ODIN3_OK;
}

/* Appends the map line of entry (H-4: escaped fields, no header) to out. */
static odin3_status append_line(const odin3_name_map *map, const odin3_name_entry *entry,
                                odin3_strbuf *out) {
    odin3_strbuf one;
    odin3_strbuf_init(&one);
    odin3_status st = odin3_name_map_write(entry, 1, map->width, &one);
    const char *body = one.data != NULL ? strchr(one.data, '\n') : NULL;
    if (st == ODIN3_OK && body != NULL) {
        st = odin3_strbuf_append(out,
                                 (odin3_bytes){body + 1, one.len - (size_t)(body + 1 - one.data)});
    }
    odin3_strbuf_free(&one);
    return st;
}

static const odin3_name_entry *entry_at(const odin3_name_map *map, size_t index) {
    return odin3_vec_cat(&map->entries, index);
}

/* --lookup: the map lines whose short name is value. */
static odin3_status run_lookup(const odin3_name_map *map, odin3_bytes value, odin3_strbuf *out) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < map->entries.len; i++) {
        const odin3_name_entry *entry = entry_at(map, i);
        st = same_bytes(entry->short_name, value) ? append_line(map, entry, out) : ODIN3_OK;
    }
    return st;
}

/* --reverse: the short name of every entry whose full name is value, each once (a set of the
 * short names already written). */
static odin3_status run_reverse(const odin3_name_map *map, odin3_bytes value, odin3_strbuf *out) {
    odin3_strtab *seen = odin3_strtab_create();
    odin3_status st = seen != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (size_t i = 0; st == ODIN3_OK && i < map->entries.len; i++) {
        const odin3_name_entry *entry = entry_at(map, i);
        uint32_t id = 0;
        if (!same_bytes(entry->full, value) || odin3_strtab_find(seen, entry->short_name, &id)) {
            continue;
        }
        st = odin3_strtab_intern(seen, entry->short_name, &id);
        st = st == ODIN3_OK ? odin3_strbuf_append(out, entry->short_name) : st;
        st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("\n")) : st;
    }
    odin3_strtab_destroy(seen);
    return st;
}

/* The version 2 line of --check: the counts of the R, C, F, T and object lines and each problem
 * (V1.2.0 3.5); *bad is set when a problem is not zero (the problems are logged). */
static odin3_status append_check_v2(const odin3_name_map *map, const char *path, odin3_strbuf *out,
                                    bool *bad) {
    odin3_name_check_v2 check = odin3_name_map_check_v2(map);
    *bad = !odin3_name_check_v2_report(&check, path) || *bad;
    return odin3_strbuf_appendf(out,
                                "records=%u constructs=%u files=%u runs=%u objects=%u bad_id=%u "
                                "bad_ref=%u bad_order=%u bad_class=%u no_record=%u\n",
                                (unsigned)check.records, (unsigned)check.constructs,
                                (unsigned)check.files, (unsigned)check.runs,
                                (unsigned)check.objects, (unsigned)check.bad_id,
                                (unsigned)check.bad_ref, (unsigned)check.bad_order,
                                (unsigned)check.bad_class, (unsigned)check.no_record);
}

/* --check: the counts (H-5); failure when an entry is bad. */
static odin3_status run_check(const odin3_name_map *map, const char *path, odin3_strbuf *out) {
    odin3_name_check check = odin3_name_map_check(map);
    odin3_status st = odin3_strbuf_appendf(out, "entries=%u bad_hash=%u duplicates=%u ignored=%u\n",
                                           (unsigned)check.entries, (unsigned)check.bad_hash,
                                           (unsigned)check.duplicates, (unsigned)map->ignored);
    bool bad = check.bad_hash != 0 || check.duplicates != 0 || map->ignored != 0;
    if (st == ODIN3_OK && map->version == ODIN3_NAME_MAP_VERSION_2) {
        st = append_check_v2(map, path, out, &bad);
    }
    return st == ODIN3_OK && bad ? ODIN3_ERR_PARSE : st;
}

/* --lookup and --chain of a version 2 map: *found is the number of lines that matched. */
static odin3_status run_v2(const names_args *args, const odin3_name_map *map, odin3_strbuf *out,
                           uint32_t *found) {
    names_v2 v2;
    odin3_status st = names_v2_init(&v2, map);
    if (st == ODIN3_OK) {
        names_v2_mode mode = args->action == ACTION_CHAIN ? NV2_CHAIN : NV2_LOOKUP;
        st = names_v2_print(&v2, args->value, mode, out, found);
        names_v2_free(&v2);
    }
    return st;
}

/* Runs the action; *found tells whether a --lookup, --reverse or --chain matched anything. */
static odin3_status run_action(const names_args *args, const odin3_name_map *map, const char *path,
                               odin3_strbuf *out, bool *found) {
    bool v2 = map->version == ODIN3_NAME_MAP_VERSION_2;
    odin3_status st = ODIN3_OK;
    uint32_t matches = 0;
    if (args->action == ACTION_CHAIN && !v2) {
        odin3_log(ODIN3_LOG_ERROR, "names: --chain needs a version 2 map (written with --prov-map)");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (args->action == ACTION_CHECK) {
        return run_check(map, path, out);
    }
    if (v2 && args->action != ACTION_REVERSE) {
        st = run_v2(args, map, out, &matches);
        *found = matches > 0;
        return st;
    }
    st = args->action == ACTION_LOOKUP ? run_lookup(map, args->value, out)
                                       : run_reverse(map, args->value, out);
    *found = out->len > 0;
    return st;
}

static odin3_status names_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                              void *user) {
    (void)ctx;
    (void)design;
    (void)user;
    names_args parsed = {.action = ACTION_NONE};
    odin3_name_map map = {0};
    odin3_strbuf text;
    odin3_strbuf_init(&parsed.map);
    odin3_strbuf_init(&text);
    odin3_status st = names_parse(args, &parsed);
    st = st == ODIN3_OK ? load_map(parsed.map.data, &map) : st;
    bool matched = true; /* --check has no match to find */
    odin3_status found =
        st == ODIN3_OK ? run_action(&parsed, &map, parsed.map.data, &text, &matched) : st;
    if (st == ODIN3_OK && !matched && found == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "names: '%.*s' is not in %s", (int)parsed.value.len,
                  (const char *)parsed.value.ptr, parsed.map.data);
        found = ODIN3_ERR_CHECK; /* not a usage error: scripts tell "absent" from "bad call" */
    }
    odin3_status written = st == ODIN3_OK ? write_stdout(&text) : ODIN3_OK;
    if (st == ODIN3_OK) {
        st = found != ODIN3_OK ? found : written;
    }
    odin3_name_map_free(&map);
    odin3_strbuf_free(&text);
    odin3_strbuf_free(&parsed.map);
    return st;
}

const odin3_pass_def odin3_pass_names = {
    "names",
    "names --map <file> (--lookup <short> | --reverse <full> | --chain <short> | --check): look "
    "hashed names up in a name map written by `--name-style hash` (needs no design; fails with "
    "CHECK when the name is absent, PARSE for a bad map, INVALID_ARG for a bad call); a version 2 "
    "map (--prov-map) also gives the records and the HDL chain of an object (--lookup, --chain)",
    names_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
