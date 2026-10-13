/*
 * clock.h — the monotonic clock behind the "(N ms)" figures of the info lines.
 */
#ifndef ODIN3_UTIL_CLOCK_H
#define ODIN3_UTIL_CLOCK_H

/* Milliseconds on the monotonic clock, from an arbitrary start; only differences mean anything. */
double odin3_now_ms(void);

#endif
