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

/* Splits the attribute value text into arena strings (in place copies). */
static odin3_status split_sinks(const arch_rd *rd, const char *text, odin3_arch_mport *port) {
    uint32_t count = 0;
    for (size_t i = 0; text[i] != '\0';) {
        while (is_space(text[i])) {
            i++;
        }
        size_t start = i;
        while (text[i] != '\0' && !is_space(text[i])) {
            i++;
        }
        count += i > start ? 1u : 0u;
    }
    const char **sinks = odin3_arena_alloc(rd->arch->arena, (count + 1u) * sizeof *sinks);
    if (sinks == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t n = 0;
    for (size_t i = 0; text[i] != '\0';) {
        while (is_space(text[i])) {
            i++;
        }
        size_t start = i;
        while (text[i] != '\0' && !is_space(text[i])) {
            i++;
        }
        if (i > start) {
            sinks[n] = odin3_arena_strndup(rd->arch->arena, text + start, i - start);
            if (sinks[n++] == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
        }
    }
    port->sinks = sinks;
    port->n_sinks = n;
    return ODIN3_OK;
}

static odin3_status read_port(const arch_rd *rd, const odin3_xml_elem *xp, bool is_out,
                              odin3_arch_mport *port) {
    const char *sinks = NULL;
    port->is_out = is_out;
    port->line = xp->line;
    odin3_status st = arch_attr_req(rd, xp, "name", &port->name);
    st = st != ODIN3_OK ? st : arch_attr_bool(rd, xp, "is_clock", false, &port->is_clock);
    st = st != ODIN3_OK ? st
                        : arch_attr_bool(rd, xp, "is_non_clock_global", false,
                                         &port->non_clock_global);
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

/* Checks sinks (output of the same model, none on an output) and duplicate port names. */
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
        if (port->is_out && port->n_sinks > 0) {
            return arch_fail(rd, at, "output port '%s' of model '%s' cannot have "
                                     "combinational_sink_ports",
                             port->name, model->name);
        }
        for (uint32_t s = 0; s < port->n_sinks; s++) {
            bool found = false;
            for (uint32_t j = 0; j < model->n_ports; j++) {
                found = found || (model->ports[j].is_out &&
                                  strcmp(model->ports[j].name, port->sinks[s]) == 0);
            }
            if (!found) {
                return arch_fail(rd, at, "combinational sink '%s' is not an output of model '%s'",
                                 port->sinks[s], model->name);
            }
        }
    }
    return ODIN3_OK;
}

static odin3_status read_port_group(const arch_rd *rd, const odin3_xml_elem *group, bool is_out,
                                    odin3_arch_mport *ports, uint32_t *n) {
    for (const odin3_xml_elem *xp = arch_first(rd, group); xp != NULL; xp = arch_next(rd, xp)) {
        if (!arch_tag_is(xp, "port")) {
            return arch_fail(rd, xp, "unsupported element '%.*s' in <%.*s>", (int)xp->tag.len,
                             (const char *)xp->tag.ptr, (int)group->tag.len,
                             (const char *)group->tag.ptr);
        }
        odin3_status st = read_port(rd, xp, is_out, &ports[*n]);
        if (st != ODIN3_OK) {
            return st;
        }
        (*n)++;
    }
    return ODIN3_OK;
}

static uint32_t count_ports(const arch_rd *rd, const odin3_xml_elem *xm) {
    uint32_t total = 0;
    for (const odin3_xml_elem *g = arch_first(rd, xm); g != NULL; g = arch_next(rd, g)) {
        total += arch_count(rd, g, "port");
    }
    return total;
}

static odin3_status read_model(const arch_rd *rd, const odin3_xml_elem *xm, odin3_arch_model *model) {
    model->line = xm->line;
    odin3_status st = arch_attr_req(rd, xm, "name", &model->name);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_arch_mport *ports =
        odin3_arena_alloc(rd->arch->arena, (count_ports(rd, xm) + 1u) * sizeof *ports);
    if (ports == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t n = 0;
    for (const odin3_xml_elem *g = arch_first(rd, xm); st == ODIN3_OK && g != NULL;
         g = arch_next(rd, g)) {
        bool in = arch_tag_is(g, "input_ports");
        if (!in && !arch_tag_is(g, "output_ports")) {
            return arch_fail(rd, g, "unsupported element '%.*s' in <model>", (int)g->tag.len,
                             (const char *)g->tag.ptr);
        }
        st = read_port_group(rd, g, !in, ports, &n);
    }
    model->ports = ports;
    model->n_ports = n;
    return st != ODIN3_OK ? st : check_ports(rd, model, xm);
}

odin3_status arch_parse_models(arch_rd *rd, const odin3_xml_elem *models) {
    if (models == NULL) {
        return ODIN3_OK;
    }
    uint32_t total = 0;
    for (const odin3_xml_elem *c = arch_first(rd, models); c != NULL; c = arch_next(rd, c)) {
        if (!arch_tag_is(c, "model")) {
            return arch_fail(rd, c, "unsupported element '%.*s' in <models>", (int)c->tag.len,
                             (const char *)c->tag.ptr);
        }
        total++;
    }
    odin3_arch *arch = rd->arch;
    arch->models = odin3_arena_alloc(arch->arena, (total + 1u) * sizeof *arch->models);
    if (arch->models == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (const odin3_xml_elem *c = arch_first(rd, models); c != NULL; c = arch_next(rd, c)) {
        odin3_arch_model *model = &arch->models[arch->n_models];
        odin3_status st = read_model(rd, c, model);
        if (st != ODIN3_OK) {
            return st;
        }
        if (odin3_arch_model_find(arch, model->name) != NULL) {
            return arch_fail(rd, c, "duplicate model '%s'", model->name);
        }
        arch->n_models++;
    }
    return ODIN3_OK;
}
