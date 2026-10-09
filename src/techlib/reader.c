/* reader.c — the .o3lib reader: lines and statements, file input, registration. */
#include "techlib/reader.h"

#include "techlib/reader_internal.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { READ_CHUNK = 4096 };

/* --- diagnostics and spans ----------------------------------------------------------------- */

odin3_status odin3_rd_err(const odin3_reader *rd, odin3_rd_loc loc, const char *fmt, ...) {
    char msg[ODIN3_RD_MSG_MAX];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    if (loc.col > 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s:%u:%u: %s", rd->file, loc.line, loc.col, msg);
    } else {
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: %s", rd->file, loc.line, msg);
    }
    return ODIN3_RD_PARSE_ERROR;
}

odin3_rd_loc odin3_rd_at(const odin3_reader *rd, uint32_t col) {
    return (odin3_rd_loc){rd->line, col};
}

bool odin3_rd_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\v' || ch == '\f';
}

static bool is_ident_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

static bool is_ident_char(char ch) {
    return is_ident_start(ch) || (ch >= '0' && ch <= '9') || ch == '$';
}

void odin3_rd_advance(odin3_span *sp, size_t count) {
    sp->ptr += count;
    sp->len -= count;
    sp->col += (uint32_t)count;
}

void odin3_rd_skip_ws(odin3_span *sp) {
    while (sp->len > 0 && odin3_rd_space(sp->ptr[0])) {
        odin3_rd_advance(sp, 1);
    }
}

odin3_span odin3_rd_trim(odin3_span sp) {
    odin3_rd_skip_ws(&sp);
    while (sp.len > 0 && odin3_rd_space(sp.ptr[sp.len - 1])) {
        sp.len--;
    }
    return sp;
}

bool odin3_rd_word(odin3_span *sp, odin3_span *word) {
    odin3_rd_skip_ws(sp);
    size_t count = 0;
    while (count < sp->len && !odin3_rd_space(sp->ptr[count])) {
        count++;
    }
    *word = (odin3_span){sp->ptr, count, sp->col};
    odin3_rd_advance(sp, count);
    return count > 0;
}

bool odin3_rd_is(odin3_span word, const char *keyword) {
    size_t len = strlen(keyword);
    return word.len == len && memcmp(word.ptr, keyword, len) == 0;
}

bool odin3_rd_ident(odin3_span *sp, odin3_span *ident) {
    odin3_rd_skip_ws(sp);
    if (sp->len == 0 || !is_ident_start(sp->ptr[0])) {
        return false;
    }
    size_t count = 1;
    while (count < sp->len && is_ident_char(sp->ptr[count])) {
        count++;
    }
    *ident = (odin3_span){sp->ptr, count, sp->col};
    odin3_rd_advance(sp, count);
    return true;
}

bool odin3_rd_lit(odin3_span *sp, const char *lit) {
    odin3_rd_skip_ws(sp);
    size_t len = strlen(lit);
    if (sp->len < len || memcmp(sp->ptr, lit, len) != 0) {
        return false;
    }
    odin3_rd_advance(sp, len);
    return true;
}

odin3_status odin3_rd_end(const odin3_reader *rd, odin3_span rest) {
    odin3_span word;
    if (odin3_rd_word(&rest, &word)) {
        return odin3_rd_err(rd, odin3_rd_at(rd, word.col), "unexpected '%.*s'", (int)word.len,
                            word.ptr);
    }
    return ODIN3_OK;
}

odin3_status odin3_rd_intern(odin3_reader *rd, odin3_span sp, uint32_t *id) {
    return odin3_strtab_intern(rd->strtab, (odin3_bytes){sp.ptr, sp.len}, id);
}

odin3_status odin3_rd_expr(odin3_reader *rd, odin3_span text, const odin3_expr **out) {
    text = odin3_rd_trim(text);
    if (text.len == 0) {
        return odin3_rd_err(rd, odin3_rd_at(rd, text.col), "expected an expression");
    }
    /* The parser sees the line with everything before the expression blanked, so the columns
     * of its messages are columns of the line. */
    size_t lead = text.col - 1U;
    odin3_strbuf_clear(&rd->text);
    if (odin3_strbuf_append(&rd->text, (odin3_bytes){rd->line_start, lead + text.len}) !=
        ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(rd->text.data, ' ', lead);
    const odin3_expr_parser parser = {rd->arena, rd->strtab, rd->file, rd->line};
    odin3_status st = odin3_expr_parse(&parser, (odin3_bytes){rd->text.data, rd->text.len}, out);
    if (st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY) {
        return st;
    }
    return ODIN3_RD_PARSE_ERROR;
}

/* --- library statement and dispatch -------------------------------------------------------- */

static odin3_status st_library(odin3_reader *rd, odin3_span rest) {
    if (rd->library != 0) {
        return odin3_rd_err(rd, odin3_rd_at(rd, 1),
                            "second 'library' statement (the first is on line %u)",
                            rd->library_line);
    }
    odin3_span name;
    if (!odin3_rd_word(&rest, &name)) {
        return odin3_rd_err(rd, odin3_rd_at(rd, rest.col), "expected a library name");
    }
    odin3_status st = odin3_rd_end(rd, rest);
    if (st == ODIN3_OK) {
        st = odin3_rd_intern(rd, name, &rd->library);
        rd->library_line = rd->line;
    }
    return st;
}

typedef odin3_status (*stmt_fn)(odin3_reader *rd, odin3_span rest);

typedef struct stmt_kind {
    const char *keyword;
    bool in_cell; /* legal only between `cell` and `end` (else only outside) */
    stmt_fn run;
} stmt_kind;

static const stmt_kind k_stmts[] = {
    {"library", false, st_library},         {"cell", false, odin3_rd_st_cell},
    {"param", true, odin3_rd_st_param},     {"in", true, odin3_rd_st_in},
    {"out", true, odin3_rd_st_out},         {"inout", true, odin3_rd_st_inout},
    {"fn", true, odin3_rd_st_fn},           {"seq", true, odin3_rd_st_seq},
    {"memory", true, odin3_rd_st_memory},   {"width", true, odin3_rd_st_mem_width},
    {"write", true, odin3_rd_st_mem_write}, {"read", true, odin3_rd_st_mem_read},
    {"end", true, odin3_rd_st_cell_end},
};

static const stmt_kind *find_stmt(odin3_span word) {
    for (size_t i = 0; i < sizeof k_stmts / sizeof k_stmts[0]; i++) {
        if (odin3_rd_is(word, k_stmts[i].keyword)) {
            return &k_stmts[i];
        }
    }
    return NULL;
}

static odin3_status statement(odin3_reader *rd, odin3_span stmt) {
    odin3_span word;
    if (!odin3_rd_word(&stmt, &word)) {
        return ODIN3_OK; /* empty statement */
    }
    const odin3_rd_loc loc = odin3_rd_at(rd, word.col);
    const stmt_kind *kind = find_stmt(word);
    if (kind == NULL) {
        return odin3_rd_err(rd, loc, "unknown statement '%.*s'", (int)word.len, word.ptr);
    }
    if (rd->library == 0 && kind->run != st_library) {
        return odin3_rd_err(rd, loc, "expected 'library' before any other statement");
    }
    if (kind->in_cell && !rd->in_cell) {
        return odin3_rd_err(rd, loc, "'%s' outside a cell", kind->keyword);
    }
    if (!kind->in_cell && rd->in_cell) {
        return odin3_rd_err(rd, loc, "'%s' inside cell '%s' (missing 'end'?)", kind->keyword,
                            odin3_rd_cell_name(rd));
    }
    return kind->run(rd, stmt);
}

/* One line: '#' starts a comment, ';' separates statements. */
static odin3_status do_line(odin3_reader *rd, odin3_span line) {
    const char *nul = memchr(line.ptr, '\0', line.len);
    if (nul != NULL) {
        return odin3_rd_err(rd, odin3_rd_at(rd, (uint32_t)(nul - line.ptr) + 1U),
                            "NUL byte in the library");
    }
    const char *hash = memchr(line.ptr, '#', line.len);
    if (hash != NULL) {
        line.len = (size_t)(hash - line.ptr);
    }
    for (;;) {
        const char *semi = memchr(line.ptr, ';', line.len);
        size_t count = semi != NULL ? (size_t)(semi - line.ptr) : line.len;
        odin3_status st = statement(rd, (odin3_span){line.ptr, count, line.col});
        if (st != ODIN3_OK || semi == NULL) {
            return st;
        }
        odin3_rd_advance(&line, count + 1);
    }
}

static odin3_status parse_text(odin3_reader *rd, odin3_bytes text) {
    const char *ptr = text.ptr;
    size_t left = text.len;
    while (left > 0) {
        const char *newline = memchr(ptr, '\n', left);
        size_t count = newline != NULL ? (size_t)(newline - ptr) : left;
        rd->line++;
        rd->line_start = ptr;
        odin3_status st = do_line(rd, (odin3_span){ptr, count, 1});
        if (st != ODIN3_OK) {
            return st;
        }
        size_t step = newline != NULL ? count + 1 : count;
        ptr += step;
        left -= step;
    }
    if (rd->in_cell) {
        return odin3_rd_err(rd, (odin3_rd_loc){rd->cell.line, 0}, "cell '%s' has no 'end'",
                            odin3_rd_cell_name(rd));
    }
    if (rd->library == 0) {
        return odin3_rd_err(rd, (odin3_rd_loc){rd->line > 0 ? rd->line : 1, 0},
                            "no 'library' statement");
    }
    return ODIN3_OK;
}

/* --- reader lifetime and registration ------------------------------------------------------ */

static void reader_init(odin3_reader *rd, odin3_design *design) {
    memset(rd, 0, sizeof *rd);
    rd->design = design;
    rd->strtab = odin3_design_strtab(design);
    rd->arena = odin3_celltype_arena(design);
    odin3_rd_cell_init(&rd->cell);
    odin3_vec_init(&rd->pending, sizeof(odin3_rd_pending));
    odin3_vec_init(&rd->idents, sizeof(const odin3_expr *));
    odin3_vec_init(&rd->ids, sizeof(uint32_t));
    odin3_strbuf_init(&rd->text);
}

static void reader_free(odin3_reader *rd) {
    odin3_rd_cell_free(&rd->cell);
    odin3_vec_free(&rd->pending);
    odin3_vec_free(&rd->idents);
    odin3_vec_free(&rd->ids);
    odin3_strbuf_free(&rd->text);
}

static odin3_status register_all(odin3_reader *rd) {
    for (size_t i = 0; i < rd->pending.len; i++) {
        const odin3_rd_pending *cell = odin3_vec_cat(&rd->pending, i);
        odin3_celltype_id id = {0};
        odin3_status st = odin3_celltype_add_local(rd->design, cell->def, &id);
        if (st == ODIN3_OK) {
            st = odin3_celltype_set_lib(rd->design, id, cell->lib);
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_techlib_read_text(odin3_design *design, const odin3_techlib_text *src) {
    odin3_reader rd;
    reader_init(&rd, design);
    const char *name = src->name != NULL ? src->name : "<o3lib>";
    char *file = odin3_arena_strndup(rd.arena, name, strlen(name));
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    if (file != NULL) {
        rd.file = file;
        st = parse_text(&rd, src->text);
    }
    if (st == ODIN3_OK) {
        st = register_all(&rd);
    }
    reader_free(&rd);
    return st;
}

static odin3_status slurp(const char *path, odin3_strbuf *buf) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "%s: cannot open the tech library", path);
        return ODIN3_ERR_IO;
    }
    char chunk[READ_CHUNK];
    odin3_status st = ODIN3_OK;
    size_t count = 0;
    while (st == ODIN3_OK && (count = fread(chunk, 1, sizeof chunk, file)) > 0) {
        st = odin3_strbuf_append(buf, (odin3_bytes){chunk, count});
    }
    if (st == ODIN3_OK && ferror(file) != 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s: cannot read the tech library", path);
        st = ODIN3_ERR_IO;
    }
    (void)fclose(file);
    return st;
}

odin3_status odin3_techlib_read(odin3_design *design, const char *path) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    odin3_status st = slurp(path, &buf);
    if (st == ODIN3_OK) {
        const odin3_techlib_text src = {path, {buf.data, buf.len}};
        st = odin3_techlib_read_text(design, &src);
    }
    odin3_strbuf_free(&buf);
    return st;
}

const odin3_techlib_cell *odin3_techlib_cell_get(const odin3_design *design, odin3_celltype_id id) {
    return odin3_celltype_lib(design, id);
}
