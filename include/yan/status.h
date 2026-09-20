#ifndef YAN_STATUS_H
#define YAN_STATUS_H

typedef enum {
    YAN_OK = 0,
    YAN_INVALID_ARGUMENT,
    YAN_INVALID_STATE,
    YAN_OUT_OF_MEMORY,
    YAN_INVALID_WIDTH,
    YAN_OUT_OF_BOUNDS,
    YAN_UNALIGNED,
    YAN_UNMAPPED,
    YAN_UNSUPPORTED_INSTRUCTION,
    YAN_TRAP,
    YAN_INVALID_IMAGE,
    /* The operation is well formed but not available in the current
     * configuration: a host backend is not attached, or a device cannot accept
     * data right now. Distinct from YAN_INVALID_STATE, which reports a caller
     * error (a NULL or half-built object), so that a driver can tell "headless"
     * apart from "I called this wrong" and keep running. */
    YAN_UNAVAILABLE
} YanStatus;

#endif
