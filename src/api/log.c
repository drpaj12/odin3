/*
 * log.c — the public ABI's logging group: odin3_log_write (odin3_log_set_level, _get_level and
 * _set_sink are exported from src/util/log.c as they are).
 */
#include "util/log.h"
#include "odin3/odin3.h"
#include "util/attr.h"

#include <stddef.h>

ODIN3_EXPORT odin3_status odin3_log_write(odin3_log_level level, const char *msg) {
    if (msg == NULL || (int)level < 0 || level >= ODIN3_LOG_LEVEL_COUNT) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_log(level, "%s", msg);
    return ODIN3_OK;
}
