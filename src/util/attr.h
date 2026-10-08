/*
 * attr.h — compiler attribute macros shared by Odin III sources.
 */
#ifndef ODIN3_UTIL_ATTR_H
#define ODIN3_UTIL_ATTR_H

/* Marks a definition as part of the libodin3 ABI; everything else is built hidden. */
#define ODIN3_EXPORT __attribute__((visibility("default")))

/* Marks a printf-style function: `f` is the format argument, `a` the first variadic one. */
#define ODIN3_PRINTF(f, a) __attribute__((format(printf, f, a)))

#endif
