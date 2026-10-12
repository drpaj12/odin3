/*
 * passrun_fixture_lib.c — a test hook in the test-only odin3_procfix library (3D Task 8, fix
 * round 1): odin3_passrun_info_append reached from Python, so that test_python_walk.py checks
 * qor_netstats' parser of the `# abc:` header (tools/vtr-qor) against the words the core writes.
 * Never installed; libodin3's own exports are unchanged.
 */
#include "passrun_fixture.h"

#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/hash.h"
#include "util/str.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

ODIN3_EXPORT odin3_status odin3_test_passrun_info(const char *const *pairs, uint32_t count,
                                                  char *out, size_t cap) {
    if (pairs == NULL || out == NULL || cap == 0) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        st = odin3_passrun_info_append(&buf, pairs[2 * i], odin3_bytes_cstr(pairs[(2 * i) + 1]));
    }
    if (st == ODIN3_OK && buf.len >= cap) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (st == ODIN3_OK) {
        memcpy(out, buf.len > 0 ? buf.data : "", buf.len + 1);
    }
    odin3_strbuf_free(&buf);
    return st;
}
