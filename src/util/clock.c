/*
 * clock.c — odin3_now_ms (clock.h).
 */
#include "util/clock.h"

#include <time.h>

enum { CLOCK_MS_PER_S = 1000, CLOCK_NS_PER_MS = 1000000 };

double odin3_now_ms(void) {
    struct timespec ts = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * CLOCK_MS_PER_S + (double)ts.tv_nsec / CLOCK_NS_PER_MS;
}
