/*
 * arch.c — the architecture record (create, destroy, queries) and the import entry points.
 */
#include "frontends/arch/arch.h"

#include "frontends/arch/arch_read.h"
#include "ir/celltype.h"
#include "techlib/reader.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

odin3_arch *odin3_arch_create(void) {
    odin3_arch *arch = odin3_util_calloc(sizeof *arch);
    if (arch == NULL) {
        return NULL;
    }
    arch->arena = odin3_arena_create(0);
    if (arch->arena == NULL) {
        odin3_util_free(arch);
        return NULL;
    }
    odin3_vec_init(&arch->leaves, sizeof(odin3_arch_leaf));
    odin3_vec_init(&arch->ics, sizeof(odin3_arch_ic));
    odin3_vec_init(&arch->directs, sizeof(odin3_arch_direct));
    odin3_strbuf_init(&arch->text);
    return arch;
}

void odin3_arch_destroy(odin3_arch *arch) {
    if (arch == NULL) {
        return;
    }
    odin3_vec_free(&arch->leaves);
    odin3_vec_free(&arch->ics);
    odin3_vec_free(&arch->directs);
    odin3_strbuf_free(&arch->text);
    odin3_arena_destroy(arch->arena);
    odin3_util_free(arch);
}

/* --- queries ------------------------------------------------------------------------------- */

uint32_t odin3_arch_lut_k(const odin3_arch *arch) {
    return arch->lut_k;
}

uint32_t odin3_arch_lut_k_min(const odin3_arch *arch) {
    return arch->lut_k_min;
}

bool odin3_arch_has_adder(const odin3_arch *arch) {
    return arch->has_adder;
}

uint32_t odin3_arch_model_count(const odin3_arch *arch) {
    return arch->n_models;
}

const odin3_arch_model *odin3_arch_model_at(const odin3_arch *arch, uint32_t index) {
    return index < arch->n_models ? &arch->models[index] : NULL;
}

const odin3_arch_model *odin3_arch_model_find(const odin3_arch *arch, const char *name) {
    for (uint32_t i = 0; i < arch->n_models; i++) {
        if (strcmp(arch->models[i].name, name) == 0) {
            return &arch->models[i];
        }
    }
    return NULL;
}

uint32_t odin3_arch_leaf_count(const odin3_arch *arch) {
    return (uint32_t)arch->leaves.len;
}

const odin3_arch_leaf *odin3_arch_leaf_at(const odin3_arch *arch, uint32_t index) {
    return index < arch->leaves.len ? odin3_vec_cat(&arch->leaves, index) : NULL;
}

bool odin3_arch_next_leaf_of(const odin3_arch *arch, const char *model, uint32_t from,
                             uint32_t *index) {
    for (uint32_t i = from; i < arch->leaves.len; i++) {
        const odin3_arch_leaf *leaf = odin3_vec_cat(&arch->leaves, i);
        if (leaf->kind == ODIN3_ARCH_LEAF_SUBCKT && strcmp(leaf->model, model) == 0) {
            *index = i;
            return true;
        }
    }
    return false;
}

uint32_t odin3_arch_ic_count(const odin3_arch *arch) {
    return (uint32_t)arch->ics.len;
}

const odin3_arch_ic *odin3_arch_ic_at(const odin3_arch *arch, uint32_t index) {
    return index < arch->ics.len ? odin3_vec_cat(&arch->ics, index) : NULL;
}

uint32_t odin3_arch_direct_count(const odin3_arch *arch) {
    return (uint32_t)arch->directs.len;
}

const odin3_arch_direct *odin3_arch_direct_at(const odin3_arch *arch, uint32_t index) {
    return index < arch->directs.len ? odin3_vec_cat(&arch->directs, index) : NULL;
}

const char *odin3_arch_library_text(const odin3_arch *arch) {
    return arch->text.data != NULL ? arch->text.data : "";
}

/* --- accessors of partial_map ---------------------------------------------------------------- */

uint32_t odin3_arch_model_port_width(const odin3_arch_model *model, const char *port) {
    for (uint32_t i = 0; model != NULL && i < model->n_ports; i++) {
        if (strcmp(model->ports[i].name, port) == 0) {
            return model->ports[i].max_width;
        }
    }
    return 0;
}

bool odin3_arch_model_is_template(const odin3_arch *arch, const char *model) {
    const odin3_arch_model *found = odin3_arch_model_find(arch, model);
    return found != NULL && found->registered && found->is_template;
}

/* The largest num_pins of an input pin of leaf whose name contains `part`. */
static uint32_t leaf_pins_named(const odin3_arch_leaf *leaf, const char *part) {
    uint32_t most = 0;
    for (uint32_t i = 0; i < leaf->n_pins; i++) {
        const odin3_arch_pin *pin = &leaf->pins[i];
        if (pin->dir == ODIN3_ARCH_PIN_IN && strstr(pin->name, part) != NULL &&
            pin->num_pins > most) {
            most = pin->num_pins;
        }
    }
    return most;
}

uint32_t odin3_arch_max_addr_pins(const odin3_arch *arch) {
    static const char *const rams[] = {"single_port_ram", "dual_port_ram"};
    uint32_t most = 0;
    for (size_t ram = 0; ram < sizeof rams / sizeof rams[0]; ram++) {
        uint32_t idx = 0;
        for (uint32_t from = 0; odin3_arch_next_leaf_of(arch, rams[ram], from, &idx);
             from = idx + 1) {
            uint32_t pins = leaf_pins_named(odin3_arch_leaf_at(arch, idx), "addr");
            most = pins > most ? pins : most;
        }
    }
    return most;
}

uint32_t odin3_arch_modes_of(const odin3_arch *arch, const char *model, odin3_arch_ram_mode *out,
                             uint32_t cap) {
    uint32_t count = 0;
    uint32_t idx = 0;
    for (uint32_t from = 0; odin3_arch_next_leaf_of(arch, model, from, &idx); from = idx + 1) {
        if (count < cap) {
            const odin3_arch_leaf *leaf = odin3_arch_leaf_at(arch, idx);
            out[count] =
                (odin3_arch_ram_mode){leaf_pins_named(leaf, "addr"), leaf_pins_named(leaf, "data")};
        }
        count++;
    }
    return count;
}

/* --- carry pack patterns (4.2 C-2, C-3) --------------------------------------------------- */

/* True when ref is "<pb>.<port>" or "<pb>[range].<port>[range]" for exactly that pb and port. */
static bool ref_is(const char *ref, const char *pb, const char *port) {
    size_t pb_len = strlen(pb);
    size_t port_len = strlen(port);
    if (ref == NULL || strncmp(ref, pb, pb_len) != 0) {
        return false;
    }
    const char *at = ref + pb_len;
    if (*at == '[') {
        at = strchr(at, ']');
        at = at != NULL ? at + 1 : NULL;
    }
    if (at == NULL || *at != '.' || strncmp(at + 1, port, port_len) != 0) {
        return false;
    }
    return at[1 + port_len] == '\0' || at[1 + port_len] == '[';
}

/* True when pack has a port reference to `port` of some `adder` leaf. */
static bool pack_reaches(const odin3_arch *arch, const odin3_arch_pack *pack, const char *port) {
    uint32_t idx = 0;
    for (uint32_t from = 0; odin3_arch_next_leaf_of(arch, "adder", from, &idx); from = idx + 1) {
        const odin3_arch_leaf *leaf = odin3_arch_leaf_at(arch, idx);
        const char *pb = leaf->levels[leaf->n_levels - 1U].pb;
        if (ref_is(pack->in_port, pb, port) || ref_is(pack->out_port, pb, port)) {
            return true;
        }
    }
    return false;
}

/* A pack pattern's place: the interconnect index and the pattern index within it. */
typedef struct pack_pos {
    uint32_t ic, pack;
} pack_pos;

/* True when some pack named `name` reaches `port` of an `adder` leaf, looking only at the packs
 * before `stop` (all of them for {UINT32_MAX, UINT32_MAX}). */
static bool name_reaches(const odin3_arch *arch, const char *name, const char *port,
                         pack_pos stop) {
    for (uint32_t i = 0; i <= stop.ic && i < odin3_arch_ic_count(arch); i++) {
        const odin3_arch_ic *ic = odin3_arch_ic_at(arch, i);
        for (uint32_t j = 0; j < ic->n_packs && (i < stop.ic || j < stop.pack); j++) {
            if (strcmp(ic->packs[j].name, name) == 0 && pack_reaches(arch, &ic->packs[j], port)) {
                return true;
            }
        }
    }
    return false;
}

bool odin3_arch_chain_pattern_reaches_adder(const odin3_arch *arch) {
    const pack_pos all = {UINT32_MAX, UINT32_MAX};
    for (uint32_t i = 0; i < odin3_arch_ic_count(arch); i++) {
        const odin3_arch_ic *ic = odin3_arch_ic_at(arch, i);
        for (uint32_t j = 0; j < ic->n_packs; j++) {
            if (pack_reaches(arch, &ic->packs[j], "cin") &&
                name_reaches(arch, ic->packs[j].name, "cout", all)) {
                return true;
            }
        }
    }
    return false;
}

uint32_t odin3_arch_adder_cin_patterns(const odin3_arch *arch) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < odin3_arch_ic_count(arch); i++) {
        const odin3_arch_ic *ic = odin3_arch_ic_at(arch, i);
        for (uint32_t j = 0; j < ic->n_packs; j++) {
            bool first = pack_reaches(arch, &ic->packs[j], "cin") &&
                         !name_reaches(arch, ic->packs[j].name, "cin", (pack_pos){i, j});
            count += first ? 1U : 0U;
        }
    }
    return count;
}

/* --- parsing --------------------------------------------------------------------------------- */

static odin3_status parse_root(arch_rd *rd) {
    const odin3_xml_elem *root = odin3_xml_elem_at(rd->doc, 0);
    if (!arch_tag_is(root, "architecture")) {
        return arch_fail(rd, root, "expected the root element 'architecture'");
    }
    odin3_status st = arch_parse_models(rd, arch_child(rd, root, "models"));
    st = st != ODIN3_OK ? st : arch_parse_pbs(rd, arch_child(rd, root, "complexblocklist"));
    st = st != ODIN3_OK ? st : arch_parse_directlist(rd, arch_child(rd, root, "directlist"));
    if (st == ODIN3_OK) {
        arch_finish_widths(rd);
    }
    return st;
}

odin3_status odin3_arch_parse(odin3_arch *arch, const odin3_arch_src *src) {
    arch->file = odin3_arena_strndup(arch->arena, src->name, strlen(src->name));
    if (arch->file == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_xml_doc doc;
    odin3_xml_err err;
    odin3_xml_doc_init(&doc, ODIN3_ARCH_MAX_XML_NODES);
    odin3_status st = odin3_xml_parse(src->text, &doc, &err);
    if (st == ODIN3_ERR_PARSE) {
        odin3_log(ODIN3_LOG_ERROR, "%s:%u: %s%s", src->name, err.line, err.what,
                  err.over_cap ? " (more than 2^24 XML nodes)" : "");
    }
    if (st == ODIN3_OK) {
        arch_rd rd = {arch, &doc, arch->file};
        st = parse_root(&rd);
    }
    odin3_xml_doc_free(&doc);
    return st;
}

/* --- the import --------------------------------------------------------------------------------
 */

enum {
    ARCH_READ_CHUNK = 1 << 16,
    ARCH_NAME_MAX = 1024,
    ARCH_MIB_SHIFT = 20,
    ARCH_MAX_FILE_BYTES = 256 << ARCH_MIB_SHIFT
};

static odin3_status slurp(const char *path, odin3_strbuf *buf) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "read_arch: cannot read '%s'", path);
        return ODIN3_ERR_IO;
    }
    char chunk[ARCH_READ_CHUNK];
    odin3_status st = ODIN3_OK;
    size_t count = 0;
    while (st == ODIN3_OK && (count = fread(chunk, 1, sizeof chunk, file)) > 0) {
        if (buf->len + count > ARCH_MAX_FILE_BYTES) {
            odin3_log(ODIN3_LOG_ERROR, "%s: the file is larger than %u MiB", path,
                      (unsigned)(ARCH_MAX_FILE_BYTES >> ARCH_MIB_SHIFT));
            st = ODIN3_ERR_PARSE;
            break;
        }
        st = odin3_strbuf_append(buf, (odin3_bytes){chunk, count});
    }
    if (st == ODIN3_OK && ferror(file) != 0) {
        odin3_log(ODIN3_LOG_ERROR, "read_arch: cannot read '%s'", path);
        st = ODIN3_ERR_IO;
    }
    (void)fclose(file);
    return st;
}

/* Spec L-7: every model that becomes a cell must be free, so a failure registers nothing. */
static odin3_status check_collisions(const odin3_design *design, const odin3_arch *arch) {
    for (uint32_t i = 0; i < arch->n_models; i++) {
        const odin3_arch_model *model = &arch->models[i];
        uint32_t str = 0;
        odin3_celltype_id id = {0};
        if (model->registered &&
            odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(model->name), &str) &&
            odin3_celltype_find(design, str, &id)) {
            odin3_log(ODIN3_LOG_ERROR,
                      "%s:%u: model '%s' has the name of a cell type the design already has",
                      arch->file, model->line, model->name);
            return ODIN3_ERR_PARSE;
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_arch_read_text(odin3_design *design, const odin3_arch_src *src) {
    if (odin3_design_arch(design) != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "read_arch: an architecture has already been read");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_arch *arch = NULL;
    odin3_status st = odin3_design_get_arch(design, &arch);
    st = st != ODIN3_OK ? st : odin3_arch_parse(arch, src);
    st = st != ODIN3_OK ? st : odin3_arch_make_library(arch);
    st = st != ODIN3_OK ? st : check_collisions(design, arch);
    if (st == ODIN3_OK) {
        /* the lines in a message refer to the generated text, not to the XML */
        char name[ARCH_NAME_MAX];
        (void)snprintf(name, sizeof name, "%s (generated .o3lib)", src->name);
        const odin3_techlib_text lib = {
            name, {arch->text.data != NULL ? arch->text.data : "", arch->text.len}};
        st = odin3_techlib_read_text(design, &lib);
    }
    if (st != ODIN3_OK) {
        odin3_design_drop_arch(design);
        return st;
    }
    odin3_log(ODIN3_LOG_INFO, "read_arch: %s: %u models (%u registered), %u leaves, K=%u",
              arch->library, arch->n_models, arch->n_registered, (uint32_t)arch->leaves.len,
              arch->lut_k);
    return ODIN3_OK;
}

odin3_status odin3_arch_read(odin3_design *design, const char *path) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    odin3_status st = slurp(path, &buf);
    if (st == ODIN3_OK) {
        const odin3_arch_src src = {path, {buf.data != NULL ? buf.data : "", buf.len}};
        st = odin3_arch_read_text(design, &src);
    }
    odin3_strbuf_free(&buf);
    return st;
}
