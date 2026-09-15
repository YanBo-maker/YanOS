#include "yan/ram.h"

#include <stdlib.h>
#include <string.h>

static YanStatus check_access(const YanRam *ram, size_t offset, size_t width)
{
    if (ram == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (ram->data == NULL || ram->size == 0) {
        return YAN_INVALID_STATE;
    }
    if (width != 1 && width != 2 && width != 4) {
        return YAN_INVALID_WIDTH;
    }
    /* Subtract only after checking width, so even SIZE_MAX offsets are safe. */
    if (width > ram->size || offset > ram->size - width) {
        return YAN_OUT_OF_BOUNDS;
    }
    return YAN_OK;
}

YanStatus yan_ram_init(YanRam *ram, size_t size)
{
    if (ram == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (ram->data != NULL || ram->size != 0) {
        return YAN_INVALID_STATE;
    }
    if (size == 0) {
        return YAN_INVALID_ARGUMENT;
    }
    uint8_t *data = calloc(size, sizeof *data);
    if (data == NULL) {
        return YAN_OUT_OF_MEMORY;
    }
    ram->data = data;
    ram->size = size;
    return YAN_OK;
}

void yan_ram_destroy(YanRam *ram)
{
    if (ram != NULL) {
        free(ram->data);
        ram->data = NULL;
        ram->size = 0;
    }
}

YanStatus yan_ram_clear(YanRam *ram)
{
    YanStatus status = check_access(ram, 0, 1);
    if (status != YAN_OK) {
        return status;
    }
    memset(ram->data, 0, ram->size);
    return YAN_OK;
}

YanStatus yan_ram_read(const YanRam *ram, size_t offset, size_t width,
                       uint32_t *value)
{
    if (value == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    YanStatus status = check_access(ram, offset, width);
    if (status != YAN_OK) {
        return status;
    }
    uint32_t result = 0;
    /* Assemble bytes explicitly: Host endianness and alignment are irrelevant. */
    for (size_t i = 0; i < width; ++i) {
        result |= (uint32_t)ram->data[offset + i] << (8 * i);
    }
    *value = result;
    return YAN_OK;
}

YanStatus yan_ram_write(YanRam *ram, size_t offset, size_t width,
                        uint32_t value)
{
    YanStatus status = check_access(ram, offset, width);
    if (status != YAN_OK) {
        return status;
    }
    for (size_t i = 0; i < width; ++i) {
        ram->data[offset + i] = (uint8_t)(value >> (8 * i));
    }
    return YAN_OK;
}
