/*
 * arch_templates.h — the compiled-in `.o3lib` cells of the four VTR primitives (spec L-2).
 */
#ifndef ODIN3_FRONTENDS_ARCH_ARCH_TEMPLATES_H
#define ODIN3_FRONTENDS_ARCH_ARCH_TEMPLATES_H

enum { ODIN3_ARCH_N_TEMPLATES = 4 };

typedef struct odin3_arch_template {
    const char *name;      /* the model and cell name */
    const char *signature; /* "i:a o:out c:clk ...", model ports in any order */
    const char *text;      /* the cell stanza, `cell` through `end`, lines ending in '\n' */
} odin3_arch_template;

extern const odin3_arch_template odin3_arch_templates[ODIN3_ARCH_N_TEMPLATES];

#endif
