/*
 * celltype.c — the public ABI's cell-type group: plugin cell types from a plain-data definition.
 */
#include "api/api.h"

#include "ir/celltype.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    ALL_FLAGS = ODIN3_CT_TRISTATE | ODIN3_CT_ANYVIEW | ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL |
                ODIN3_CT_CLOCK_PIN0
};

enum { NAME_CHAR_MIN = '!', NAME_CHAR_MAX = '~' };

static const char k_fn[] = "odin3_celltype_register";

/* Logs "odin3_celltype_register: cell type '<name>': <why>" and returns INVALID_ARG. */
static odin3_status refuse(const odin3_plugin_celltype *def, const char *why) {
    const char *name = def->name != NULL ? def->name : "";
    odin3_log(ODIN3_LOG_ERROR, "%s: cell type '%s': %s", k_fn, name, why);
    return ODIN3_ERR_INVALID_ARG;
}

static bool gran_ok(uint32_t gran) {
    return gran == (uint32_t)ODIN3_GRAN_WORD || gran == (uint32_t)ODIN3_GRAN_BIT ||
           gran == (uint32_t)ODIN3_GRAN_HARD || gran == (uint32_t)ODIN3_GRAN_BLACKBOX;
}

/* True when name (NULL and "" are left to the IR's checks) is printable ASCII without blanks and,
 * for a port (a BLIF formal), without '=', '[' or ']'. */
static bool name_chars_ok(const char *name, bool port) {
    for (const char *at = name; at != NULL && *at != '\0'; at++) {
        if (*at < NAME_CHAR_MIN || *at > NAME_CHAR_MAX || (port && strchr("=[]", *at) != NULL)) {
            return false;
        }
    }
    return true;
}

static const char *port_error(const odin3_plugin_port *port) {
    if (port->dir > (uint32_t)ODIN3_DIR_INOUT) {
        return "port direction out of range";
    }
    if (!name_chars_ok(port->name, true)) {
        return "a port name must be printable ASCII without blanks, '=', '[' or ']'";
    }
    if (port->width_param != NULL && port->width != 0) {
        return "a port sized by width_param must have width 0";
    }
    if (port->scalar && (port->width_param != NULL || port->width != 1)) {
        return "a scalar port must have the constant width 1";
    }
    return NULL;
}

static const char *ports_error(const odin3_plugin_celltype *def) {
    const char *err = NULL;
    for (uint32_t i = 0; err == NULL && i < def->n_ports; i++) {
        err = port_error(&def->ports[i]);
    }
    return err;
}

static const char *params_error(const odin3_plugin_celltype *def) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        const odin3_plugin_param *param = &def->params[i];
        if (param->kind > (uint32_t)ODIN3_VAL_COVER) {
            return "parameter kind out of range";
        }
        if (param->kind != (uint32_t)ODIN3_VAL_INT && param->dflt != 0) {
            return "only an INT parameter has a default (dflt must be 0)";
        }
        if (!name_chars_ok(param->name, false)) {
            return "a parameter name must be printable ASCII without blanks";
        }
    }
    return NULL;
}

/* NULL when the ABI-level fields are in range (the IR checks the rest), else why not. */
static const char *abi_error(const odin3_plugin_celltype *def) {
    for (size_t i = 0; i < sizeof def->reserved / sizeof def->reserved[0]; i++) {
        if (def->reserved[i] != NULL) {
            return "reserved slot is set (built for a later ABI?)";
        }
    }
    if (!name_chars_ok(def->name, false)) {
        return "the name must be printable ASCII without blanks";
    }
    if (!gran_ok(def->gran)) {
        return "granularity must be WORD, BIT, HARD or BLACKBOX";
    }
    if ((def->flags & ~(uint32_t)ALL_FLAGS) != 0) {
        return "unknown flag bits";
    }
    if ((def->n_ports > 0 && def->ports == NULL) || (def->n_params > 0 && def->params == NULL)) {
        return "NULL port or parameter array";
    }
    const char *err = ports_error(def);
    return err != NULL ? err : params_error(def);
}

/* Where copied strings go: one buffer holding them all, filled in order. */
typedef struct copy_buf {
    char *next; /* where the next string goes */
} copy_buf;

static size_t str_bytes(const char *text) {
    return text != NULL ? strlen(text) + 1 : 0;
}

static size_t strings_bytes(const odin3_plugin_celltype *def) {
    size_t bytes = str_bytes(def->name);
    for (uint32_t i = 0; i < def->n_ports; i++) {
        bytes += str_bytes(def->ports[i].name) + str_bytes(def->ports[i].width_param);
    }
    for (uint32_t i = 0; i < def->n_params; i++) {
        bytes += str_bytes(def->params[i].name);
    }
    return bytes;
}

static const char *copy_str(copy_buf *buf, const char *text) {
    if (text == NULL) {
        return NULL;
    }
    size_t len = strlen(text) + 1;
    char *at = buf->next;
    memcpy(at, text, len);
    buf->next += len;
    return at;
}

static void copy_ports(copy_buf *buf, const odin3_plugin_celltype *def, odin3_port_def *ports) {
    for (uint32_t i = 0; i < def->n_ports; i++) {
        const odin3_plugin_port *src = &def->ports[i];
        ports[i] = (odin3_port_def){.name = copy_str(buf, src->name),
                                    .dir = (odin3_dir)src->dir,
                                    .scalar = src->scalar,
                                    .width = src->width,
                                    .width_param = copy_str(buf, src->width_param)};
    }
}

static void copy_params(copy_buf *buf, const odin3_plugin_celltype *def, odin3_param_def *params) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        const odin3_plugin_param *src = &def->params[i];
        odin3_value_kind kind = (odin3_value_kind)src->kind;
        params[i] = (odin3_param_def){
            .name = copy_str(buf, src->name), .kind = kind, .dflt = {.kind = kind, .i = src->dflt}};
    }
}

/*
 * A registered copy: the internal definition first (the registry holds &copy->def, which keeps
 * the whole copy reachable for the life of the process), then the pieces it points to.
 */
typedef struct celltype_copy {
    odin3_celltype_def def;
    odin3_port_def *ports;
    odin3_param_def *params;
    char *strings;
} celltype_copy;

static void copy_free(celltype_copy *copy) {
    if (copy != NULL) {
        odin3_util_free(copy->ports);
        odin3_util_free(copy->params);
        odin3_util_free(copy->strings);
        odin3_util_free(copy);
    }
}

/* Allocates the copy's pieces (none for an empty array); false on out of memory. */
static bool copy_alloc(celltype_copy *copy, const odin3_plugin_celltype *def) {
    if (def->n_ports > 0) {
        copy->ports = odin3_util_calloc((size_t)def->n_ports * sizeof(odin3_port_def));
    }
    if (def->n_params > 0) {
        copy->params = odin3_util_calloc((size_t)def->n_params * sizeof(odin3_param_def));
    }
    size_t bytes = strings_bytes(def);
    copy->strings = bytes > 0 ? odin3_util_malloc(bytes) : NULL;
    return (def->n_ports == 0 || copy->ports != NULL) &&
           (def->n_params == 0 || copy->params != NULL) && (bytes == 0 || copy->strings != NULL);
}

/* The internal definition of def with everything it points to copied; NULL on out of memory. */
static celltype_copy *copy_def(const odin3_plugin_celltype *def) {
    celltype_copy *copy = odin3_util_calloc(sizeof *copy);
    if (copy == NULL || !copy_alloc(copy, def)) {
        copy_free(copy);
        return NULL;
    }
    copy_buf buf = {copy->strings};
    copy_ports(&buf, def, copy->ports);
    copy_params(&buf, def, copy->params);
    copy->def = (odin3_celltype_def){.name = copy_str(&buf, def->name),
                                     .gran = (odin3_granularity)def->gran,
                                     .flags = def->flags,
                                     .ports = copy->ports,
                                     .n_ports = def->n_ports,
                                     .params = copy->params,
                                     .n_params = def->n_params,
                                     .simulate = def->simulate,
                                     .sim_scratch_bytes = def->sim_scratch_bytes};
    return copy;
}

ODIN3_EXPORT odin3_status odin3_celltype_register(const odin3_plugin_celltype *def) {
    if (def == NULL) {
        return odin3_api_invalid(k_fn);
    }
    const char *err = abi_error(def);
    if (err != NULL) {
        return refuse(def, err);
    }
    celltype_copy *copy = copy_def(def);
    if (copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    /* The registry keeps the copy for the life of the process (it is never unregistered). */
    odin3_status st = odin3_celltype_register_global(&copy->def);
    if (st != ODIN3_OK) {
        copy_free(copy);
    }
    return st;
}
