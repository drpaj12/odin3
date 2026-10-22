/* vectors_in.c — the reader of the odin3-sim-vectors text format (SV-2, SV-3; see vectors.h). */
#include "odin3/odin3.h"
#include "sim/sim.h"
#include "sim/vectors.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { DECIMAL = 10, TITLE_WORDS = 4, MAX_VECTOR_WORDS = 3 };

/* The state of one parse. */
typedef struct vin {
    const odin3_sim *sim;
    const char *path;
    odin3_strbuf expected; /* the design's port lines, one per '\n'-ended line */
    const char *next_port; /* the next expected port line in expected.data */
    uint32_t port_total;   /* port lines the design has */
    uint32_t ports_seen;   /* port lines the file has had */
    uint32_t last_port_line;
    uint32_t line_no;
    uint32_t n_bits;
    uint32_t cycles;
    uint64_t seed;
    uint64_t title_cycles;
    bool first_hash_done; /* the first '#' line has been looked at as a title */
    bool has_title;
    bool in_vectors; /* a vector line has been seen */
    odin3_vec bits;
} vin;

/* A view of one line (no line end, no blanks at either end). */
typedef struct line_view {
    const char *ptr;
    size_t len;
} line_view;

static bool is_blank(char chr) {
    return chr == ' ' || chr == '\t';
}

/* The next blank-separated word of [*pos, end); false at the end. */
static bool next_word(const char **pos, const char *end, line_view *word) {
    const char *cur = *pos;
    while (cur < end && is_blank(*cur)) {
        cur++;
    }
    if (cur == end) {
        *pos = cur;
        return false;
    }
    word->ptr = cur;
    while (cur < end && !is_blank(*cur)) {
        cur++;
    }
    word->len = (size_t)(cur - word->ptr);
    *pos = cur;
    return true;
}

static bool word_is(line_view word, const char *text) {
    return strlen(text) == word.len && memcmp(word.ptr, text, word.len) == 0;
}

/* A decimal number of at most max (digits only, no sign). */
static bool parse_decimal(line_view word, uint64_t max, uint64_t *out) {
    if (word.len == 0) {
        return false;
    }
    uint64_t val = 0;
    for (size_t i = 0; i < word.len; i++) {
        if (word.ptr[i] < '0' || word.ptr[i] > '9') {
            return false;
        }
        uint64_t digit = (uint64_t)(word.ptr[i] - '0');
        if (val > (max - digit) / DECIMAL) {
            return false;
        }
        val = val * DECIMAL + digit;
    }
    *out = val;
    return true;
}

static uint32_t count_non_clock_bits(const odin3_sim *sim) {
    uint32_t count = 0;
    for (uint32_t port = 0; port < odin3_sim_input_count(sim); port++) {
        for (uint32_t k = 0; k < odin3_sim_input_width(sim, port); k++) {
            count += odin3_sim_input_is_clock(sim, (odin3_sim_bit){port, k}) ? 0U : 1U;
        }
    }
    return count;
}

/* The expected port lines: the header the writer makes, without its title line. */
static odin3_status vin_init(vin *state, const odin3_sim *sim, const char *path) {
    memset(state, 0, sizeof *state);
    state->sim = sim;
    state->path = path;
    state->n_bits = count_non_clock_bits(sim);
    odin3_strbuf_init(&state->expected);
    odin3_vec_init(&state->bits, sizeof(uint8_t));
    const odin3_sim_vectors run = {"x", 0, 0};
    odin3_status st = odin3_sim_vectors_header(sim, &run, &state->expected);
    const char *nl = st == ODIN3_OK ? strchr(state->expected.data, '\n') : NULL;
    state->next_port = nl != NULL ? nl + 1 : "";
    for (const char *cur = state->next_port; *cur != '\0'; cur = strchr(cur, '\n') + 1) {
        state->port_total++;
    }
    return st;
}

static void vin_free(vin *state) {
    odin3_strbuf_free(&state->expected);
    odin3_vec_free(&state->bits);
}

/* ---- header lines ----------------------------------------------------------------------- */

/* The title line: the last four words are `seed <S> cycles <N>`. */
static void read_title(vin *state, const char *pos, const char *end) {
    line_view last[TITLE_WORDS] = {{0}};
    line_view word;
    size_t count = 0;
    while (next_word(&pos, end, &word)) {
        memmove(last, last + 1, (TITLE_WORDS - 1) * sizeof last[0]);
        last[TITLE_WORDS - 1] = word;
        count++;
    }
    uint64_t seed = 0;
    uint64_t cycles = 0;
    if (count >= TITLE_WORDS && word_is(last[0], "seed") && word_is(last[2], "cycles") &&
        parse_decimal(last[1], UINT64_MAX, &seed) && parse_decimal(last[3], UINT32_MAX, &cycles)) {
        state->has_title = true;
        state->seed = seed;
        state->title_cycles = cycles;
    }
}

static odin3_status count_error(const vin *state, uint32_t line, uint32_t listed) {
    odin3_log(ODIN3_LOG_ERROR, "%s:%u: the header lists %u port lines, the design has %u",
              state->path, line, listed, state->port_total);
    return ODIN3_ERR_PARSE;
}

/* A port line must equal the next expected one (trailing blanks already dropped). */
static odin3_status check_port_line(vin *state, line_view line) {
    if (state->ports_seen >= state->port_total) {
        return count_error(state, state->line_no, state->ports_seen + 1);
    }
    const char *nl = strchr(state->next_port, '\n');
    size_t want = (size_t)(nl - state->next_port);
    if (want != line.len || memcmp(state->next_port, line.ptr, want) != 0) {
        odin3_log(ODIN3_LOG_ERROR,
                  "%s:%u: port line `%.*s` does not match the design: expected `%.*s`", state->path,
                  state->line_no, (int)line.len, line.ptr, (int)want, state->next_port);
        return ODIN3_ERR_PARSE;
    }
    state->next_port = nl + 1;
    state->ports_seen++;
    state->last_port_line = state->line_no;
    return ODIN3_OK;
}

static odin3_status header_line(vin *state, line_view line) {
    const char *pos = line.ptr;
    const char *end = line.ptr + line.len;
    if (!state->first_hash_done) {
        state->first_hash_done = true;
        read_title(state, pos, end);
    }
    line_view word;
    line_view kind;
    if (state->in_vectors || !next_word(&pos, end, &word) || !next_word(&pos, end, &kind)) {
        return ODIN3_OK;
    }
    if (word_is(kind, "input") || word_is(kind, "clock") || word_is(kind, "output")) {
        return check_port_line(state, line);
    }
    return ODIN3_OK;
}

/* ---- vector lines ----------------------------------------------------------------------- */

static odin3_status input_bits_error(const vin *state, line_view word) {
    if (state->n_bits == 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: expected `-`: the design has no non-clock input bit",
                  state->path, state->line_no);
    } else {
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: expected %u input bits of 0 and 1, found `%.*s`",
                  state->path, state->line_no, state->n_bits, (int)word.len, word.ptr);
    }
    return ODIN3_ERR_PARSE;
}

/* Appends the word's bits to state->bits; the word must be n_bits of 0 and 1 (or "-" for none). */
static odin3_status take_input_bits(vin *state, line_view word) {
    if (state->n_bits == 0) {
        return word_is(word, "-") ? ODIN3_OK : input_bits_error(state, word);
    }
    bool ok = word.len == state->n_bits;
    for (size_t i = 0; ok && i < word.len; i++) {
        ok = word.ptr[i] == '0' || word.ptr[i] == '1';
    }
    if (!ok) {
        return input_bits_error(state, word);
    }
    odin3_status st = odin3_vec_reserve(&state->bits, state->bits.len + word.len);
    for (size_t i = 0; st == ODIN3_OK && i < word.len; i++) {
        uint8_t *slot = odin3_vec_push(&state->bits);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = word.ptr[i] == '1' ? 1U : 0U;
    }
    return st;
}

static odin3_status vector_line(vin *state, line_view line) {
    const char *pos = line.ptr;
    const char *end = line.ptr + line.len;
    line_view words[MAX_VECTOR_WORDS + 1];
    size_t count = 0;
    while (count <= MAX_VECTOR_WORDS && next_word(&pos, end, &words[count])) {
        count++;
    }
    if (count < 2 || count > MAX_VECTOR_WORDS) {
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: expected `<cycle> <inputs> [<outputs>]`", state->path,
                  state->line_no);
        return ODIN3_ERR_PARSE;
    }
    uint64_t cycle = 0;
    if (!parse_decimal(words[0], UINT32_MAX, &cycle) || cycle != state->cycles) {
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: expected cycle %u, found `%.*s`", state->path,
                  state->line_no, state->cycles, (int)words[0].len, words[0].ptr);
        return ODIN3_ERR_PARSE;
    }
    odin3_status st = take_input_bits(state, words[1]);
    state->cycles += st == ODIN3_OK ? 1U : 0U;
    return st;
}

/* ---- the whole text --------------------------------------------------------------------- */

/* The first vector line (or the end) closes the header: its port lines must be all or none. */
static odin3_status close_header(const vin *state) {
    if (state->ports_seen > 0 && state->ports_seen != state->port_total) {
        return count_error(state, state->last_port_line, state->ports_seen);
    }
    return ODIN3_OK;
}

static odin3_status one_line(vin *state, line_view line) {
    if (line.len == 0) {
        return ODIN3_OK;
    }
    if (line.ptr[0] == '#') {
        return header_line(state, line);
    }
    odin3_status st = ODIN3_OK;
    if (!state->in_vectors) {
        st = close_header(state);
        state->in_vectors = true;
    }
    return st == ODIN3_OK ? vector_line(state, line) : st;
}

/* [ptr, end) without a CR before the line end and without blanks at either end. */
static line_view trim_line(const char *ptr, const char *end) {
    while (end > ptr && (is_blank(end[-1]) || end[-1] == '\r')) {
        end--;
    }
    while (ptr < end && is_blank(*ptr)) {
        ptr++;
    }
    return (line_view){ptr, (size_t)(end - ptr)};
}

static odin3_status parse_lines(vin *state, odin3_bytes text) {
    const char *cur = text.ptr;
    const char *end = cur + text.len;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && cur < end) {
        const char *nl = memchr(cur, '\n', (size_t)(end - cur));
        const char *stop = nl != NULL ? nl : end;
        state->line_no++;
        st = one_line(state, trim_line(cur, stop));
        cur = nl != NULL ? nl + 1 : end;
    }
    if (st == ODIN3_OK && !state->in_vectors) {
        st = close_header(state);
    }
    if (st == ODIN3_OK && state->has_title && state->title_cycles != state->cycles) {
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: the title says %llu cycles, the file has %u",
                  state->path, state->line_no, (unsigned long long)state->title_cycles,
                  state->cycles);
        st = ODIN3_ERR_PARSE;
    }
    return st;
}

odin3_status odin3_sim_vectors_parse(const odin3_sim *sim, const char *path, odin3_bytes text,
                                     odin3_sim_vectors_in *out) {
    memset(out, 0, sizeof *out);
    vin state;
    odin3_status st = vin_init(&state, sim, path);
    if (st == ODIN3_OK) {
        st = parse_lines(&state, text);
    }
    if (st == ODIN3_OK) {
        out->bits = state.bits.data;
        out->n_bits = state.n_bits;
        out->cycles = state.cycles;
        out->seed = state.has_title ? state.seed : 0;
        state.bits.data = NULL; /* handed over to out */
    }
    vin_free(&state);
    return st;
}

odin3_status odin3_sim_vectors_read(const odin3_sim *sim, const char *path,
                                    odin3_sim_vectors_in *out) {
    memset(out, 0, sizeof *out);
    odin3_file_bytes file;
    odin3_status st = odin3_file_read_all(path, ODIN3_SIM_VECTORS_MAX_BYTES, &file);
    if (st == ODIN3_ERR_IO) {
        odin3_log(ODIN3_LOG_ERROR, "sim: cannot read `%s`: %s", path, strerror(file.err));
    } else if (st == ODIN3_ERR_PARSE && file.too_big) {
        odin3_log(ODIN3_LOG_ERROR, "%s: larger than %u bytes", path, ODIN3_SIM_VECTORS_MAX_BYTES);
    } else if (st == ODIN3_OK) {
        st = odin3_sim_vectors_parse(sim, path, (odin3_bytes){file.data, file.len}, out);
    }
    odin3_file_bytes_free(&file);
    return st;
}

void odin3_sim_vectors_in_free(odin3_sim_vectors_in *in) {
    if (in != NULL) {
        odin3_util_free(in->bits);
        memset(in, 0, sizeof *in);
    }
}

void odin3_sim_vectors_drive(odin3_sim *sim, const odin3_sim_vectors_in *in, uint32_t cycle) {
    if (cycle >= in->cycles) {
        return;
    }
    const uint8_t *line = in->bits + (size_t)cycle * in->n_bits;
    size_t idx = 0;
    for (uint32_t port = 0; port < odin3_sim_input_count(sim); port++) {
        for (uint32_t k = odin3_sim_input_width(sim, port); k-- > 0;) {
            const odin3_sim_bit at = {port, k};
            if (!odin3_sim_input_is_clock(sim, at)) {
                (void)odin3_sim_set_input(sim, at, line[idx++] != 0U);
            }
        }
    }
}
