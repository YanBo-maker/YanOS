#ifndef YAN_BUS_H
#define YAN_BUS_H

#include "yan/ram.h"
#include "yan/interrupt.h"
#include "yan/uart.h"
#include "yan/transport.h"

/* Borrowed mappings: the RAM and every device must outlive the Bus. A device
 * pointer left NULL means that MMIO window is not mapped on this Bus. */
typedef struct {
    YanRam *ram;
    uint32_t ram_base;
    YanClint *clint;
    YanPlic *plic;
    YanUart *uart;
    YanTransport *transport;
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
/* Data reads and writes decode the device windows before RAM; every device
 * answers 32-bit word accesses only. Instruction fetch stays RAM only: the
 * MMIO windows are data mappings. Fetch always reads four bytes. */
YanBusResult yan_bus_fetch32(const YanBus *bus, uint32_t address,
                             uint32_t *instruction);
/* Machine-level interrupt lines as mip/mie bit positions: MSIP, MTIP, MEIP. */
uint32_t yan_bus_pending_interrupts(const YanBus *bus);

#endif
