#ifndef YAN_IMAGE_H
#define YAN_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#include "yan/ram.h"
#include "yan/status.h"

/* Result of loading a Guest image: only the architectural entry address is
 * exposed. Segment placement is a property of RAM, not of the image. */
typedef struct {
    uint32_t entry;
} YanImageInfo;

/* Loads the PT_LOAD segments of an ELF32 little-endian RISC-V image into RAM.
 * `base` is the Guest address of RAM offset zero. The image is validated
 * before any byte is written, so a rejected image leaves RAM unchanged. */
YanStatus yan_image_load_elf(YanRam *ram, uint32_t base, const uint8_t *data,
                             size_t size, YanImageInfo *info);

/* Returns YAN_OK and the symbol value, or YAN_UNMAPPED when the image carries
 * no such defined symbol. Harnesses use this for `tohost` instead of hard
 * coding an address: a test linker places the symbol wherever it fits. */
YanStatus yan_image_find_symbol(const uint8_t *data, size_t size,
                                const char *name, uint32_t *address);

#endif
