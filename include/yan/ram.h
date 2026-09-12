#ifndef YAN_RAM_H
#define YAN_RAM_H

#include <stddef.h>
#include <stdint.h>

#include "yan/status.h"

/* Zero-initialize before first use; an initialized RAM owns its buffer. */
typedef struct {
    uint8_t *data;
    size_t size;
} YanRam;

YanStatus yan_ram_init(YanRam *ram, size_t size);
void yan_ram_destroy(YanRam *ram);
YanStatus yan_ram_clear(YanRam *ram);

/* Width is 1, 2, or 4 bytes. Errors preserve data and read outputs. */
YanStatus yan_ram_read(const YanRam *ram, size_t offset, size_t width,
                       uint32_t *value);
YanStatus yan_ram_write(YanRam *ram, size_t offset, size_t width,
                        uint32_t value);

#endif
