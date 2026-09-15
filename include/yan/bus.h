#ifndef YAN_BUS_H
#define YAN_BUS_H

#include "yan/ram.h"

/* Borrowed mapping: the RAM must outlive the Bus and keep its capacity. */
typedef struct {
    YanRam *ram;
    uint32_t ram_base;
} YanBus;

typedef enum {
    YAN_ACCESS_READ,
    YAN_ACCESS_WRITE,
    YAN_ACCESS_FETCH
} YanAccess;

typedef struct {
    YanStatus status;
    uint32_t address;
    size_t width;
    YanAccess access;
} YanBusResult;

YanStatus yan_bus_init(YanBus *bus, YanRam *ram, uint32_t base);
YanBusResult yan_bus_read(const YanBus *bus, uint32_t address, size_t width,
                          uint32_t *value);
YanBusResult yan_bus_write(YanBus *bus, uint32_t address, size_t width,
                           uint32_t value);
/* Stage 1 maps executable RAM only. Fetch always reads four bytes. */
YanBusResult yan_bus_fetch32(const YanBus *bus, uint32_t address,
                             uint32_t *instruction);

#endif
