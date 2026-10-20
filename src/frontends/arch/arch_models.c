/*
 * arch_models.c — the `<models>` of a VPR architecture (spec M-1 … M-3). VPR's own checks apply:
 * duplicate models and ports, a port that is both a clock and a non-clock global, unknown or
 * misplaced combinational sinks.
 */
#include "frontends/arch/arch_read.h"

#include <string.h>

static bool is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

typedef struct word {
    size_t start, len;
} word;

/* The next whitespace-separated word of text at or after *pos; false at the end. */
static bool next_word(const char *text, size_t *pos, word *out) {
    while (is_space(text[*pos])) {
        (*pos)++;
    }
    out->start = *pos;
    while (text[*pos] != '\0' && !is_space(text[*pos])) {
        (*pos)++;
    }
    out->len = *pos - out->start;
    return out->len > 0;
}

/* Splits the attribute value text into arena strings. */
static odin3_status split_sinks(const arch_rd *rd, const char *text, odin3_arch_mport *port) {
    uint32_t count = 0;
    size_t pos = 0;
    word wd = {0, 0};
    while (next_word(text, &pos, &wd)) {
        count++;
    }
    const char **sinks =
        (const char **)odin3_arena_alloc(rd->arch->arena, (count + 1U) * sizeof *sinks);
    if (sinks == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    pos = 0;
    for (uint32_t idx = 0; idx < count && next_word(text, &pos, &wd); idx++) {
        sinks[idx] = odin3_arena_strndup(rd->arch->arena, text + wd.start, wd.len);
        if (sinks[idx] == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    port->sinks = sinks;
    port->n_sinks = count;
    return ODIN3_OK;
}

static odin3_status read_port(const arch_rd *rd, const odin3_xml_elem *xp, bool is_out,
                              odin3_arch_mport *port) {
    const char *sinks = NULL;
    port->is_out = is_out;
    port->line = xp->line;
    odin3_status st = arch_attr_req(rd, xp, "name", &port->name);
    st = st != ODIN3_OK ? st : arch_attr_bool(rd, xp, "is_clock", false, &port->is_clock);
    st = st != ODIN3_OK
             ? st
             : arch_attr_bool(rd, xp, "is_non_clock_global", false, &port->non_clock_global);
    st = st != ODIN3_OK ? st : arch_attr(rd, xp, "clock", &port->clock);
    st = st != ODIN3_OK ? st : arch_attr(rd, xp, "combinational_sink_ports", &sinks);
    if (st == ODIN3_OK && sinks != NULL) {
        st = split_sinks(rd, sinks, port);
    }
    if (st == ODIN3_OK && port->is_clock && port->non_clock_global) {
        st = arch_fail(rd, xp, "port '%s' cannot be both a clock and a non-clock global",
                       port->name);
    }
    return st;
}

static bool has_output(const odin3_arch_model *model, const char *name) {
    for (uint32_t i = 0; i < model->n_ports; i++) {
        if (model->ports[i].is_out && strcmp(model->ports[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* The sinks of port: none on an output, each an output of the same model. */
static odin3_status check_sinks(const arch_rd *rd, const odin3_arch_model *model,
                                const odin3_arch_mport *port, const odin3_xml_elem *at) {
    if (port->is_out && port->n_sinks > 0) {
        return arch_fail(rd, at,
                         "output port '%s' of model '%s' cannot have combinational_sink_ports",
                         port->name, model->name);
    }
    for (uint32_t idx = 0; idx < port->n_sinks; idx++) {
        if (!has_output(model, port->sinks[idx])) {
            return arch_fail(rd, at, "combinational sink '%s' is not an output of model '%s'",
                             port->sinks[idx], model->name);
        }
    }
    return ODIN3_OK;
}

/* Checks duplicate port names and the sinks. */
static odin3_status check_ports(const arch_rd *rd, const odin3_arch_model *model,
                                const odin3_xml_elem *at) {
    for (uint32_t i = 0; i < model->n_ports; i++) {
        const odin3_arch_mport *port = &model->ports[i];
        for (uint32_t j = 0; j < i; j++) {
            if (strcmp(model->ports[j].name, port->name) == 0) {
                return arch_fail(rd, at, "duplicate port '%s' in model '%s'", port->name,
                                 model->name);
            }
        }
        odin3_status st = check_sinks(rd, model, port, at);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_status read_port_group(const arch_rd *rd, const odin3_xml_elem *group, bool is_out,
                                    odin3_arch_mport *ports, uint32_t *filled) {
    for (const odin3_xml_elem *xp = arch_first(rd, group); xp != NULL; xp = arch_next(rd, xp)) {
        if (!arch_tag_is(xp, "port")) {
            return arch_fail(rd, xp, "unsupported element '%.*s' in <%.*s>", (int)xp->tag.len,
                             (const char *)xp->tag.ptr, (int)group->tag.len,
                             (const char *)group->tag.ptr);
        }
        odin3_status st = read_port(rd, xp, is_out, &ports[*filled]);
        if (st != ODIN3_OK) {
            return st;
        }
        (*filled)++;
    }
    return ODIN3_OK;
}

static uint32_t count_ports(const arch_rd *rd, const odin3_xml_elem *xm) {
    uint32_t total = 0;
    for (const odin3_xml_elem *group = arch_first(rd, xm); group != NULL;
         group = arch_next(rd, group)) {
        total += arch_count(rd, group, "port");
    }
    return total;
}

static odin3_status read_model(const arch_rd *rd, const odin3_xml_elem *xm,
                               odin3_arch_model *model) {
    model->line = xm->line;
    odin3_status st = arch_attr_req(rd, xm, "name", &model->name);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_arch_mport *ports =
        odin3_arena_alloc(rd->arch->arena, (count_ports(rd, xm) + 1U) * sizeof *ports);
    if (ports == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t filled = 0;
    for (const odin3_xml_elem *group = arch_first(rd, xm); st == ODIN3_OK && group != NULL;
         group = arch_next(rd, group)) {
        bool in = arch_tag_is(group, "input_ports");
        if (!in && !arch_tag_is(group, "output_ports")) {
            return arch_fail(rd, group, "unsupported element '%.*s' in <model>",
                             (int)group->tag.len, (const char *)group->tag.ptr);
        }
        st = read_port_group(rd, group, !in, ports, &filled);
    }
    model->ports = ports;
    model->n_ports = filled;
    return st != ODIN3_OK ? st : check_ports(rd, model, xm);
}

odin3_status arch_parse_models(arch_rd *rd, const odin3_xml_elem *models) {
    if (models == NULL) {
        return ODIN3_OK;
    }
    uint32_t total = 0;
    for (const odin3_xml_elem *child = arch_first(rd, models); child != NULL;
         child = arch_next(rd, child)) {
        if (!arch_tag_is(child, "model")) {
            return arch_fail(rd, child, "unsupported element '%.*s' in <models>",
                             (int)child->tag.len, (const char *)child->tag.ptr);
        }
        total++;
    }
    odin3_arch *arch = rd->arch;
    arch->models = odin3_arena_alloc(arch->arena, (total + 1U) * sizeof *arch->models);
    if (arch->models == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (const odin3_xml_elem *child = arch_first(rd, models); child != NULL;
         child = arch_next(rd, child)) {
        odin3_arch_model *model = &arch->models[arch->n_models];
        odin3_status st = read_model(rd, child, model);
        if (st != ODIN3_OK) {
            return st;
        }
        if (odin3_arch_model_find(arch, model->name) != NULL) {
            return arch_fail(rd, child, "duplicate model '%s'", model->name);
        }
        arch->n_models++;
    }
    return ODIN3_OK;
}
