/* lexer.c — BLIF lexer: 64 KiB buffered reads, logical lines of NUL-terminated tokens. */
#include "frontends/blif/lexer.h"

#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdio.h>
#include <string.h>

enum { LEX_CHUNK = 64 * 1024 };

struct odin3_blif_lexer {
    FILE *file;
    unsigned char *chunk;
    size_t pos, len;
    bool eof;
    uint32_t phys_line;  /* physical line of the next unread byte */
    uint32_t first_line; /* first physical line with a token of the current logical line */
    bool in_comment, in_token, pending_bs, bs_blank;
    char *path;
    odin3_status status;
    odin3_strbuf text; /* tokens of the current logical line, each NUL-terminated */
    odin3_vec starts;  /* size_t start offset per token */
    odin3_vec tokens;  /* odin3_bytes, rebuilt for each returned line */
};

odin3_blif_lexer *odin3_blif_lexer_open(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "%s: cannot open file", path);
        return NULL;
    }
    odin3_blif_lexer *lx = odin3_util_calloc(sizeof *lx);
    unsigned char *chunk = lx != NULL ? odin3_util_malloc(LEX_CHUNK) : NULL;
    char *path_copy = chunk != NULL ? odin3_util_malloc(strlen(path) + 1) : NULL;
    if (path_copy == NULL) {
        odin3_util_free(chunk);
        odin3_util_free(lx);
        (void)fclose(file);
        odin3_log(ODIN3_LOG_ERROR, "%s: out of memory", path);
        return NULL;
    }
    lx->file = file;
    lx->chunk = chunk;
    lx->path = strcpy(path_copy, path);
    lx->phys_line = 1;
    odin3_strbuf_init(&lx->text);
    odin3_vec_init(&lx->starts, sizeof(size_t));
    odin3_vec_init(&lx->tokens, sizeof(odin3_bytes));
    return lx;
}

void odin3_blif_lexer_close(odin3_blif_lexer *lx) {
    if (lx == NULL) {
        return;
    }
    (void)fclose(lx->file);
    odin3_util_free(lx->chunk);
    odin3_util_free(lx->path);
    odin3_strbuf_free(&lx->text);
    odin3_vec_free(&lx->starts);
    odin3_vec_free(&lx->tokens);
    odin3_util_free(lx);
}

odin3_status odin3_blif_lexer_status(const odin3_blif_lexer *lx) {
    return lx->status;
}

/* Ensures unread bytes are available; false at end of input or on failure. */
static bool lex_fill(odin3_blif_lexer *lx) {
    if (lx->pos < lx->len) {
        return true;
    }
    if (lx->eof) {
        return false;
    }
    lx->pos = 0;
    lx->len = fread(lx->chunk, 1, LEX_CHUNK, lx->file);
    if (lx->len < LEX_CHUNK) {
        lx->eof = true;
        if (ferror(lx->file) != 0) {
            lx->status = ODIN3_ERR_IO;
            odin3_log(ODIN3_LOG_ERROR, "%s:%u: read error", lx->path, (unsigned)lx->phys_line);
        }
    }
    return lx->len > 0;
}

static void lex_fail_memory(odin3_blif_lexer *lx) {
    lx->status = ODIN3_ERR_NO_MEMORY;
    odin3_log(ODIN3_LOG_ERROR, "%s:%u: out of memory", lx->path, (unsigned)lx->phys_line);
}

/* Appends bytes to the current token, starting it if needed. */
static void lex_add(odin3_blif_lexer *lx, const void *ptr, size_t len) {
    if (!lx->in_token) {
        size_t *start = odin3_vec_push(&lx->starts);
        if (start == NULL) {
            lex_fail_memory(lx);
            return;
        }
        *start = lx->text.len;
        lx->in_token = true;
        if (lx->starts.len == 1) {
            lx->first_line = lx->phys_line;
        }
    }
    if (odin3_strbuf_append(&lx->text, (odin3_bytes){ptr, len}) != ODIN3_OK) {
        lex_fail_memory(lx);
    }
}

static void lex_end_token(odin3_blif_lexer *lx) {
    if (lx->in_token) {
        lx->in_token = false;
        if (odin3_strbuf_append(&lx->text, (odin3_bytes){"", 1}) != ODIN3_OK) {
            lex_fail_memory(lx);
        }
    }
}

static bool lex_is_blank(unsigned char chr) {
    return chr == ' ' || chr == '\t';
}

static bool lex_is_special(unsigned char chr) {
    return lex_is_blank(chr) || chr == '\n' || chr == '\r' || chr == '#' || chr == '\\' ||
           chr == '\0';
}

/* Handles a byte while a backslash is pending; returns true when the byte was consumed. */
static bool lex_pending_byte(odin3_blif_lexer *lx, unsigned char chr) {
    if (lex_is_blank(chr) || chr == '\r') {
        lx->bs_blank = lx->bs_blank || lex_is_blank(chr);
        return true;
    }
    if (chr == '\n') {
        lx->pending_bs = false;
        lx->bs_blank = false;
        lx->phys_line++;
        lex_end_token(lx);
        return true;
    }
    lx->pending_bs = false;
    lex_add(lx, "\\", 1); /* a lone backslash is an ordinary character */
    if (lx->bs_blank) {
        lx->bs_blank = false;
        lex_end_token(lx);
    }
    return false;
}

/* Consumes one byte; returns true when it ended a non-empty logical line. */
static bool lex_byte(odin3_blif_lexer *lx, unsigned char chr) {
    if (chr == '\0') {
        lx->status = ODIN3_ERR_PARSE;
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: NUL byte in input", lx->path, (unsigned)lx->phys_line);
        return false;
    }
    if (lx->in_comment) {
        if (chr == '\n') {
            lx->in_comment = false;
            lx->phys_line++;
            lex_end_token(lx);
            return lx->starts.len > 0;
        }
        return false;
    }
    if (lx->pending_bs && lex_pending_byte(lx, chr)) {
        return false;
    }
    if (chr == '\n') {
        lx->phys_line++;
        lex_end_token(lx);
        return lx->starts.len > 0;
    }
    if (chr == '#') {
        lx->in_comment = true;
    } else if (chr == '\\') {
        lx->pending_bs = true;
    } else if (lex_is_blank(chr) || chr == '\r') {
        lex_end_token(lx);
    } else {
        lex_add(lx, &chr, 1); /* reached only after a literal backslash */
    }
    return false;
}

/* Consumes input up to the next special byte as token characters (one append per run). */
static void lex_run(odin3_blif_lexer *lx) {
    size_t end = lx->pos;
    while (end < lx->len && !lex_is_special(lx->chunk[end])) {
        end++;
    }
    lex_add(lx, lx->chunk + lx->pos, end - lx->pos);
    lx->pos = end;
}

static void lex_publish(odin3_blif_lexer *lx, odin3_blif_line *out) {
    odin3_vec_clear(&lx->tokens);
    for (size_t i = 0; i < lx->starts.len; i++) {
        odin3_bytes *tok = odin3_vec_push(&lx->tokens);
        if (tok == NULL) {
            lex_fail_memory(lx);
            return;
        }
        size_t start = *(const size_t *)odin3_vec_cat(&lx->starts, i);
        tok->ptr = lx->text.data + start;
        tok->len = strlen(lx->text.data + start);
    }
    out->tokens = lx->tokens.data;
    out->count = (uint32_t)lx->tokens.len;
    out->line = lx->first_line;
}

static void lex_reset_line(odin3_blif_lexer *lx) {
    odin3_strbuf_clear(&lx->text);
    odin3_vec_clear(&lx->starts);
    lx->in_token = false;
}

/* At end of input: flush a pending backslash and token; true if a line is complete. */
static bool lex_finish(odin3_blif_lexer *lx) {
    if (lx->pending_bs && !lx->in_comment) {
        lex_add(lx, "\\", 1);
        lx->pending_bs = false;
    }
    lex_end_token(lx);
    return lx->starts.len > 0 && lx->status == ODIN3_OK;
}

bool odin3_blif_lexer_next(odin3_blif_lexer *lx, odin3_blif_line *out) {
    if (lx->status != ODIN3_OK) {
        return false;
    }
    lex_reset_line(lx);
    while (lx->status == ODIN3_OK) {
        if (!lex_fill(lx)) {
            break;
        }
        if (!lx->in_comment && !lx->pending_bs && !lex_is_special(lx->chunk[lx->pos])) {
            lex_run(lx);
        } else if (lex_byte(lx, lx->chunk[lx->pos++])) {
            lex_publish(lx, out);
            return lx->status == ODIN3_OK;
        }
    }
    if (lx->status != ODIN3_OK || !lex_finish(lx)) {
        return false;
    }
    lex_publish(lx, out);
    return lx->status == ODIN3_OK;
}
