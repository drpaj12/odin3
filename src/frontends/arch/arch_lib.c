/*
 * arch_lib.c — the `.o3lib` library generated for the models of an architecture (spec L-1 … L-6).
 * The four VTR primitives come from compiled-in templates (arch_templates.c); every other model
 * is a black box with the VPR port sizes.
 */
#include "frontends/arch/arch_read.h"
#include "frontends/arch/arch_templates.h"
#include "ir/celltype.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

enum { LIB_TOKEN_MAX = 160 };

static bool is_ident_start(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

static bool is_ident(const char *text) {
    if (!is_ident_start(text[0])) {
        return false;
    }
    for (size_t i = 1; text[i] != '\0'; i++) {
        char ch = text[i];
        if (!is_ident_start(ch) && !(ch >= '0' && ch <= '9') && ch != '$') {
            return false;
        }
    }
    return true;
}

/* The `.o3lib` grammar reserves these as port names (clock is allowed since 5.1). */
static bool name_ok(const char *name) {
    return is_ident(name) && strcmp(name, "signed") != 0 && strcmp(name, "x") != 0;
}

static bool model_names_ok(const odin3_arch_model *model) {
    if (!is_ident(model->name)) {
        return false;
    }
    for (uint32_t i = 0; i < model->n_ports; i++) {
        if (!name_ok(model->ports[i].name)) {
            return false;
        }
    }
    return true;
}

/* --- templates ----------------------------------------------------------------------------------
 */

typedef struct sig_tok {
    char text[LIB_TOKEN_MAX];
} sig_tok;

static char kind_char(const odin3_arch_mport *port) {
    if (port->is_out) {
        return 'o';
    }
    return port->is_clock ? 'c' : 'i';
}

static void make_token(const odin3_arch_mport *port, sig_tok *tok) {
    (void)snprintf(tok->text, sizeof tok->text, "%c:%s", kind_char(port), port->name);
}

static bool sig_has(const char *sig, const sig_tok *tok) {
    size_t len = strlen(tok->text);
    for (const char *at = sig; *at != '\0';) {
        const char *end = strchr(at, ' ');
        size_t span = end != NULL ? (size_t)(end - at) : strlen(at);
        if (span == len && strncmp(at, tok->text, span) == 0) {
            return true;
        }
        at = end != NULL ? end + 1 : at + span;
    }
    return false;
}

static uint32_t sig_count(const char *sig) {
    uint32_t count = 1;
    for (const char *at = sig; *at != '\0'; at++) {
        count += *at == ' ' ? 1U : 0U;
    }
    return count;
}

/* The first way the model differs from the template's signature, in msg; false when it matches. */
static bool sig_differs(const odin3_arch_model *model, const char *sig, char *msg, size_t cap) {
    sig_tok tok;
    for (uint32_t i = 0; i < model->n_ports; i++) {
        make_token(&model->ports[i], &tok);
        if (!sig_has(sig, &tok)) {
            char suffix[LIB_TOKEN_MAX];
            (void)snprintf(suffix, sizeof suffix, ":%s", model->ports[i].name);
            (void)snprintf(msg, cap,
                           strstr(sig, suffix) != NULL
                               ? "port '%s' has another direction or clock flag"
                               : "port '%s' is not a port of the primitive",
                           model->ports[i].name);
            return true;
        }
    }
    if (sig_count(sig) != model->n_ports) {
        (void)snprintf(msg, cap, "the primitive has %u ports, the model %u", sig_count(sig),
                       model->n_ports);
        return true;
    }
    return false;
}

/* The template for the model: NULL when its name is none of the four or its ports differ (warned).
 */
static const odin3_arch_template *template_for(const arch_rd *rd, const odin3_arch_model *model) {
    for (int i = 0; i < ODIN3_ARCH_N_TEMPLATES; i++) {
        const odin3_arch_template *tpl = &odin3_arch_templates[i];
        char why[LIB_TOKEN_MAX * 2];
        if (strcmp(tpl->name, model->name) != 0) {
            continue;
        }
        if (!sig_differs(model, tpl->signature, why, sizeof why)) {
            return tpl;
        }
        odin3_log(ODIN3_LOG_WARN,
                  "%s:%u: model '%s' differs from the VTR primitive (%s); imported as a black box",
                  rd->file, model->line, model->name, why);
    }
    return NULL;
}

/* --- generic cells ------------------------------------------------------------------------------
 */

static odin3_status add_generic(const arch_rd *rd, const odin3_arch_model *model,
                                odin3_strbuf *out) {
    uint64_t total = 0;
    odin3_status st = odin3_strbuf_appendf(out, "cell %s blackbox\n", model->name);
    for (uint32_t i = 0; st == ODIN3_OK && i < model->n_ports; i++) {
        const odin3_arch_mport *port = &model->ports[i];
        uint32_t width = port->max_width;
        if (width == 0) {
            width = 1;
            odin3_log(ODIN3_LOG_INFO,
                      "%s:%u: model '%s' port '%s' is not used by any pb_type; "
                      "width 1",
                      rd->file, port->line, model->name, port->name);
        }
        total += width;
        if (width > ODIN3_READER_MAX_WIDTH || total > ODIN3_READER_MAX_TOTAL_WIDTH) {
            odin3_log(ODIN3_LOG_ERROR,
                      "%s:%u: model '%s' port '%s' is too wide for a library cell (a port is at "
                      "most %d bits, a cell %d bits in all)",
                      rd->file, port->line, model->name, port->name, (int)ODIN3_READER_MAX_WIDTH,
                      (int)ODIN3_READER_MAX_TOTAL_WIDTH);
            return ODIN3_ERR_PARSE;
        }
        st = odin3_strbuf_appendf(out, "  %s %s %u%s\n", port->is_out ? "out" : "in", port->name,
                                  width, (!port->is_out && port->is_clock) ? " clock" : "");
    }
    return st != ODIN3_OK ? st : odin3_strbuf_appendf(out, "end\n");
}

/* --- the library --------------------------------------------------------------------------------
 */

/* The file's stem as an identifier (spec L-1). */
static odin3_status library_name(odin3_arch *arch) {
    const char *base = strrchr(arch->file, '/');
    base = base != NULL ? base + 1 : arch->file;
    size_t len = strlen(base);
    const char *dot = strrchr(base, '.');
    len = (dot != NULL && dot != base) ? (size_t)(dot - base) : len;
    char *name = odin3_arena_alloc(arch->arena, len + 2U);
    if (name == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t at = 0;
    if (len == 0 || !is_ident_start(base[0])) {
        name[at++] = '_';
    }
    for (size_t i = 0; i < len; i++) {
        bool keep = is_ident_start(base[i]) || (base[i] >= '0' && base[i] <= '9');
        if (keep) {
            name[at++] = base[i];
        } else {
            name[at++] = '_';
        }
    }
    arch->library = name;
    return ODIN3_OK;
}

typedef struct lib_out {
    odin3_strbuf body;
    uint32_t skipped;
    const char *first_skipped;
} lib_out;

static odin3_status emit_models(arch_rd *rd, lib_out *out) {
    odin3_arch *arch = rd->arch;
    arch->n_registered = 0;
    for (uint32_t i = 0; i < arch->n_models; i++) {
        odin3_arch_model *model = &arch->models[i];
        model->registered = model_names_ok(model);
        if (!model->registered) {
            out->first_skipped = out->skipped == 0 ? model->name : out->first_skipped;
            out->skipped++;
            continue;
        }
        const odin3_arch_template *tpl = template_for(rd, model);
        odin3_status st = tpl != NULL ? odin3_strbuf_appendf(&out->body, "%s", tpl->text)
                                      : add_generic(rd, model, &out->body);
        if (st != ODIN3_OK) {
            return st;
        }
        arch->n_registered++;
    }
    return ODIN3_OK;
}

odin3_status odin3_arch_make_library(odin3_arch *arch) {
    arch_rd rd = {arch, NULL, arch->file};
    lib_out out = {.body = {0}};
    odin3_strbuf_init(&out.body);
    odin3_strbuf_clear(&arch->text);
    odin3_status st = library_name(arch);
    st = st != ODIN3_OK ? st : emit_models(&rd, &out);
    st = st != ODIN3_OK
             ? st
             : odin3_strbuf_appendf(&arch->text,
                                    "# generated by odin3 read_arch from %s (VPR "
                                    "architecture)\n# models: %u, registered: %u\n"
                                    "library %s\n",
                                    arch->file, arch->n_models, arch->n_registered, arch->library);
    if (st == ODIN3_OK && out.body.len > 0) {
        st = odin3_strbuf_append(&arch->text, (odin3_bytes){out.body.data, out.body.len});
    }
    odin3_strbuf_free(&out.body);
    if (st == ODIN3_OK && out.skipped > 0) {
        odin3_log(ODIN3_LOG_WARN,
                  "%s: %u models have names that cannot be .o3lib names and "
                  "are not registered (first: '%s')",
                  arch->file, out.skipped, out.first_skipped);
    }
    return st;
}
