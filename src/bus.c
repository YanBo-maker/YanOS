#include "yan/bus.h"

YanStatus yan_bus_init(YanBus *bus, YanRam *ram, uint32_t base)
{
    if (bus == NULL || ram == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (ram->data == NULL || ram->size == 0) {
        return YAN_INVALID_STATE;
    }
    /* The exclusive end may equal 2^32, which does not fit in uint32_t. */
    const uint64_t address_space_size = UINT64_C(1) << 32;
    if (ram->size > address_space_size - base) {
        return YAN_OUT_OF_BOUNDS;
    }
    bus->ram = ram;
    bus->ram_base = base;
    /* Devices are wired by the owner after this call; NULL means "not mapped". */
    bus->clint = NULL;
    bus->plic = NULL;
    bus->uart = NULL;
    bus->transport = NULL;
    return YAN_OK;
}

static YanStatus ram_translate(const YanBus *bus, uint32_t address, size_t width,
                               size_t *offset)
{
    if (bus == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (bus->ram == NULL || bus->ram->data == NULL || bus->ram->size == 0) {
        return YAN_INVALID_STATE;
    }
    if (width != 1 && width != 2 && width != 4) {
        return YAN_INVALID_WIDTH;
    }
    if (address % width != 0) {
        return YAN_UNALIGNED;
    }
    if (address < bus->ram_base) {
        return YAN_UNMAPPED;
    }
    size_t relative = (size_t)(address - bus->ram_base);
    if (relative >= bus->ram->size) {
        return YAN_UNMAPPED;
    }
    /* RAM validates the full width before touching any byte. */
    *offset = relative;
    return YAN_OK;
}

static YanStatus ram_read(const YanBus *bus, uint32_t address, size_t width,
                          uint32_t *value)
{
    size_t offset = 0;
    YanStatus status = ram_translate(bus, address, width, &offset);
    if (status == YAN_OK) {
        status = yan_ram_read(bus->ram, offset, width, value);
    }
    return status;
}

static YanStatus ram_write(const YanBus *bus, uint32_t address, size_t width,
                           uint32_t value)
{
    size_t offset = 0;
    YanStatus status = ram_translate(bus, address, width, &offset);
    if (status == YAN_OK) {
        status = yan_ram_write(bus->ram, offset, width, value);
    }
    return status;
}

/* True when the address falls inside [base, base + size); size never wraps
 * because every device window is far below the top of the address space. */
static int device_offset(uint32_t address, uint32_t base, uint32_t size,
                         uint32_t *offset)
{
    if (address < base || address - base >= size) {
        return 0;
    }
    *offset = address - base;
    return 1;
}

/* MMIO windows are decoded before RAM and answer 32-bit word accesses only.
 * Any other width falls through to RAM, which keeps the previous error codes
 * for addresses that no device claims. */
YanBusResult yan_bus_read(const YanBus *bus, uint32_t address, size_t width,
                          uint32_t *value)
{
    YanBusResult result = {YAN_INVALID_ARGUMENT, address, width, YAN_ACCESS_READ};
    if (value == NULL) {
        return result;
    }
    uint32_t offset = 0;
    if (bus != NULL && width == 4 && bus->clint != NULL &&
        device_offset(address, YAN_CLINT_BASE, YAN_CLINT_SIZE, &offset)) {
        result.status = yan_clint_read(bus->clint, offset, value);
    } else if (bus != NULL && width == 4 && bus->plic != NULL &&
               device_offset(address, YAN_PLIC_BASE, YAN_PLIC_SIZE, &offset)) {
        result.status = yan_plic_read(bus->plic, offset, value);
    } else if (bus != NULL && width == 4 && bus->uart != NULL &&
               device_offset(address, YAN_UART_BASE, YAN_UART_SIZE, &offset)) {
        result.status = yan_uart_read(bus->uart, offset, value);
    } else if (bus != NULL && width == 4 && bus->transport != NULL &&
               device_offset(address, YAN_TRANSPORT_BASE, YAN_TRANSPORT_SIZE,
                             &offset)) {
        result.status = yan_transport_read(bus->transport, offset, value);
    } else {
        result.status = ram_read(bus, address, width, value);
    }
    return result;
}

YanBusResult yan_bus_write(YanBus *bus, uint32_t address, size_t width,
                           uint32_t value)
{
    uint32_t offset = 0;
    YanStatus status;
    if (bus != NULL && width == 4 && bus->clint != NULL &&
        device_offset(address, YAN_CLINT_BASE, YAN_CLINT_SIZE, &offset)) {
        status = yan_clint_write(bus->clint, offset, value);
    } else if (bus != NULL && width == 4 && bus->plic != NULL &&
               device_offset(address, YAN_PLIC_BASE, YAN_PLIC_SIZE, &offset)) {
        status = yan_plic_write(bus->plic, offset, value);
    } else if (bus != NULL && width == 4 && bus->uart != NULL &&
               device_offset(address, YAN_UART_BASE, YAN_UART_SIZE, &offset)) {
        status = yan_uart_write(bus->uart, offset, value);
    } else if (bus != NULL && width == 4 && bus->transport != NULL &&
               device_offset(address, YAN_TRANSPORT_BASE, YAN_TRANSPORT_SIZE,
                             &offset)) {
        status = yan_transport_write(bus->transport, offset, value);
    } else {
        status = ram_write(bus, address, width, value);
    }
    YanBusResult result = {status, address, width, YAN_ACCESS_WRITE};
    return result;
}

/* One summary line per machine-level interrupt class. External device lines
 * reach mip through the PLIC, not through this summary: the arbitration on
 * enable and threshold belongs to the interrupt controller. */
uint32_t yan_bus_pending_interrupts(const YanBus *bus)
{
    if (bus == NULL) {
        return 0;
    }
    return yan_clint_pending(bus->clint) | yan_plic_pending(bus->plic);
}

YanBusResult yan_bus_fetch32(const YanBus *bus, uint32_t address,
                             uint32_t *instruction)
{
    /* Instruction fetch stays a RAM-only mapping: the CLINT and PLIC windows
     * added here are data windows, so executing from one keeps the previous
     * "unmapped" answer instead of becoming executable by accident. */
    YanBusResult result = {YAN_INVALID_ARGUMENT, address, 4, YAN_ACCESS_FETCH};
    if (instruction == NULL) {
        return result;
    }
    result.status = ram_read(bus, address, 4, instruction);
    return result;
}
