/*
 * objective.c — the objective of an abc run, for the provenance name map (V1.2.0 3.6, Task 10).
 *
 * The map's Q line says why cells were joined. For abc the answer is the goal of the script, and
 * the script is only known here. Two sources, in this order:
 *
 *  1. Stated: a comment line `# objective: <word>` (word: a-z, 0-9, `_` or `-`, at most
 *     ODIN3_ABC_OBJECTIVE_BYTES - 1 bytes). The first such line wins. The goal is read as written.
 *  2. Rule: the last mapping command of the script (`if`, `&if`, `map`, `&nf`) maps for `area` when
 *     the letter `a` occurs an odd number of times among its option letters (the `-a` flag of
 *     all four toggles, abc.c:21635 map, 23500 if, 45851 &if, 47281 &nf at the pin a3001b72) and
 *     for `depth` when it occurs an even number of times (each maps for delay by default and
 *     recovers area afterwards). A script with no such command states no objective.
 *
 * A valued option letter (`-K 6`, `-S str`) takes the rest of its cluster or the next word, so
 * the letters after it are not flags. Pure C; it never includes or calls ABC.
 */
#include "passes/abc/script.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { OBJ_PREFIX_BYTES = 10 }; /* the bytes of `objective:` */

static const char OBJ_PREFIX[] = "objective:";

/* The valued option letters of the four mapping commands (abc.c usage lines at the pin). */
static const struct {
    const char *name;
    const char *valued;
} k_obj_cmds[] = {{"if", "KCFAGRNTXYZMUDEWSJ"},
                  {"&if", "KCFAGRTXYZMDEWSJ"},
                  {"map", "DABFSGM"},
                  {"&nf", "KCFARLEDQMZ"}};

enum { OBJ_CMDS = sizeof k_obj_cmds / sizeof k_obj_cmds[0] };

static bool word_byte(uint8_t ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
}

/* The objective word of one comment line [at, end) (after its `#`), or false. */
static bool stated_in_line(const uint8_t *at, const uint8_t *end, char *out) {
    while (at < end && (*at == ' ' || *at == '\t')) {
        at++;
    }
    if ((size_t)(end - at) <= OBJ_PREFIX_BYTES || memcmp(at, OBJ_PREFIX, OBJ_PREFIX_BYTES) != 0) {
        return false;
    }
    at += OBJ_PREFIX_BYTES;
    while (at < end && (*at == ' ' || *at == '\t')) {
        at++;
    }
    size_t len = 0;
    while (at + len < end && word_byte(at[len])) {
        len++;
    }
    if (len == 0 || len >= ODIN3_ABC_OBJECTIVE_BYTES) {
        return false;
    }
    memcpy(out, at, len);
    out[len] = '\0';
    return true;
}

/* A comment line is `#` as the first non-blank byte of the line (a `#` after a command ends the
 * command and is a comment too, but a stated goal is its own line). */
static bool find_stated(const uint8_t *bytes, size_t len, char *out) {
    size_t pos = 0;
    while (pos < len) {
        const uint8_t *nl = memchr(bytes + pos, '\n', len - pos);
        const uint8_t *end = nl != NULL ? nl : bytes + len;
        const uint8_t *at = bytes + pos;
        while (at < end && (*at == ' ' || *at == '\t')) {
            at++;
        }
        if (at < end && *at == '#' && stated_in_line(at + 1, end, out)) {
            return true;
        }
        pos = (size_t)(end - bytes) + 1U;
    }
    return false;
}

/* How many `a` flags the option words of a mapping command hold (words 1..). */
static unsigned area_flags(const char *line, const char *valued) {
    unsigned count = 0;
    const char *at = strchr(line, ' ');
    while (at != NULL) {
        while (*at == ' ') {
            at++;
        }
        if (at[0] == '-' && ((at[1] >= 'a' && at[1] <= 'z') || (at[1] >= 'A' && at[1] <= 'Z'))) {
            for (const char *ch = at + 1; *ch != '\0' && *ch != ' '; ch++) {
                if (strchr(valued, *ch) != NULL) {
                    break; /* the rest of the cluster, or the next word, is its value */
                }
                count += *ch == 'a';
            }
        }
        at = strchr(at, ' ');
    }
    return count;
}

/* The command row of a checked command line, or -1. */
static int mapping_cmd(const char *line) {
    size_t name_len = strcspn(line, " ");
    for (int i = 0; i < (int)OBJ_CMDS; i++) {
        if (strlen(k_obj_cmds[i].name) == name_len &&
            strncmp(line, k_obj_cmds[i].name, name_len) == 0) {
            return i;
        }
    }
    return -1;
}

void odin3_abc_objective(const uint8_t *bytes, size_t len, const odin3_abc_cmd *cmds,
                         uint32_t n_cmds, char out[ODIN3_ABC_OBJECTIVE_BYTES], bool *stated) {
    out[0] = '\0';
    *stated = false;
    if (bytes != NULL && find_stated(bytes, len, out)) {
        *stated = true;
        return;
    }
    for (uint32_t i = n_cmds; i > 0; i--) {
        int row = mapping_cmd(cmds[i - 1U].line);
        if (row >= 0) {
            const char *value = (area_flags(cmds[i - 1U].line, k_obj_cmds[row].valued) & 1U) != 0U
                                    ? "area"
                                    : "depth";
            memcpy(out, value, strlen(value) + 1U);
            return;
        }
    }
}
