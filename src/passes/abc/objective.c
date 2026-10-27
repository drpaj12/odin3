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

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { OBJ_PREFIX_BYTES = 10 }; /* the bytes of `objective:` */

static const char OBJ_PREFIX[] = "objective:";

/* The valued option letters of the four mapping commands, checked against the usage lines of
 * third_party/abc/src/base/abci/abc.c at the pin: `if` 23480 (-KCFAGRNTXYZMU num, -DEW float, -SJ
 * str), `&if` 45832 (-KCFAGRTXYZM num, -DEW float, -SJ str), `map` 21626 (-DABFSG float, -M num),
 * `&nf` 47268 (-KCFARLEDQM num, -Z file). `-a` is a plain flag in all four (21635 map, 23500 if,
 * 45851 &if; &nf 47281 is the SAT-based area-oriented mapping, an area goal as well). */
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

/* Skips blanks. */
static const uint8_t *skip_blanks(const uint8_t *at, const uint8_t *end) {
    while (at < end && (*at == ' ' || *at == '\t')) {
        at++;
    }
    return at;
}

/* The objective word of one comment line [at, end) (after its `#`), or false. */
static bool stated_in_line(const uint8_t *at, const uint8_t *end, char *out) {
    at = skip_blanks(at, end);
    const size_t left = (size_t)(end - at);
    if (left < OBJ_PREFIX_BYTES || memcmp(at, OBJ_PREFIX, OBJ_PREFIX_BYTES) != 0) {
        return false;
    }
    at = skip_blanks(at + OBJ_PREFIX_BYTES, end);
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
static bool find_stated(odin3_bytes text, char *out) {
    const uint8_t *bytes = text.ptr;
    size_t pos = 0;
    while (bytes != NULL && pos < text.len) {
        const uint8_t *nl = memchr(bytes + pos, '\n', text.len - pos);
        const uint8_t *end = nl != NULL ? nl : bytes + text.len;
        const uint8_t *at = skip_blanks(bytes + pos, end);
        if (at < end && *at == '#' && stated_in_line(at + 1, end, out)) {
            return true;
        }
        pos = (size_t)(end - bytes) + 1U;
    }
    return false;
}

/* The `a` flags of one option word (`-abc`): letters up to the first valued letter, which takes
 * the rest of the word or the next word as its value. */
static unsigned word_area_flags(const char *word, int row) {
    const char *valued = k_obj_cmds[row].valued;
    unsigned count = 0;
    for (const char *ch = word + 1; *ch != '\0' && *ch != ' ' && strchr(valued, *ch) == NULL;
         ch++) {
        count += *ch == 'a';
    }
    return count;
}

/* How many `a` flags the option words of a mapping command hold (words 1..). */
static unsigned area_flags(const char *line, int row) {
    unsigned count = 0;
    const char *at = strchr(line, ' ');
    while (at != NULL) {
        at += strspn(at, " ");
        if (at[0] == '-' && isalpha((unsigned char)at[1]) != 0) {
            count += word_area_flags(at, row);
        }
        at = strchr(at, ' ');
    }
    return count;
}

/* The command row of a checked command line, or -1. */
static int mapping_cmd(const char *line) {
    size_t name_len = strcspn(line, " ");
    for (int i = 0; i < OBJ_CMDS; i++) {
        if (strlen(k_obj_cmds[i].name) == name_len &&
            strncmp(line, k_obj_cmds[i].name, name_len) == 0) {
            return i;
        }
    }
    return -1;
}

void odin3_abc_objective(odin3_bytes text, const odin3_abc_cmd *cmds, uint32_t n_cmds,
                         char out[ODIN3_ABC_OBJECTIVE_BYTES], bool *stated) {
    out[0] = '\0';
    *stated = false;
    if (find_stated(text, out)) {
        *stated = true;
        return;
    }
    for (uint32_t i = n_cmds; i > 0; i--) {
        int row = mapping_cmd(cmds[i - 1U].line);
        if (row >= 0) {
            const char *value = (area_flags(cmds[i - 1U].line, row) & 1U) != 0U ? "area" : "depth";
            memcpy(out, value, strlen(value) + 1U);
            return;
        }
    }
}

/* --- the memo ------------------------------------------------------------------------------- */

enum { MEMO_SLOTS = 32, MEMO_SHA_CHARS = 64 };

typedef struct objective_memo {
    char sha[MEMO_SHA_CHARS];
    char objective[ODIN3_ABC_OBJECTIVE_BYTES];
    bool stated;
    bool used;
} objective_memo;

static objective_memo g_memo[MEMO_SLOTS];
static uint32_t g_memo_next;

void odin3_abc_objective_note(const char *sha_hex, const char *objective, bool stated) {
    if (sha_hex == NULL || objective == NULL || strlen(sha_hex) != MEMO_SHA_CHARS ||
        strlen(objective) >= ODIN3_ABC_OBJECTIVE_BYTES) {
        return;
    }
    objective_memo *slot = NULL;
    for (uint32_t i = 0; i < MEMO_SLOTS && slot == NULL; i++) {
        if (g_memo[i].used && memcmp(g_memo[i].sha, sha_hex, MEMO_SHA_CHARS) == 0) {
            slot = &g_memo[i];
        }
    }
    if (slot == NULL) {
        slot = &g_memo[g_memo_next];
        g_memo_next = (g_memo_next + 1U) % MEMO_SLOTS;
    }
    memcpy(slot->sha, sha_hex, MEMO_SHA_CHARS);
    memcpy(slot->objective, objective, strlen(objective) + 1U);
    slot->stated = stated;
    slot->used = true;
}

bool odin3_abc_objective_find(odin3_bytes sha_hex, char out[ODIN3_ABC_OBJECTIVE_BYTES],
                              bool *stated) {
    if (sha_hex.ptr == NULL || sha_hex.len != MEMO_SHA_CHARS) {
        return false;
    }
    for (uint32_t i = 0; i < MEMO_SLOTS; i++) {
        if (g_memo[i].used && memcmp(g_memo[i].sha, sha_hex.ptr, MEMO_SHA_CHARS) == 0) {
            memcpy(out, g_memo[i].objective, ODIN3_ABC_OBJECTIVE_BYTES);
            *stated = g_memo[i].stated;
            return true;
        }
    }
    return false;
}
