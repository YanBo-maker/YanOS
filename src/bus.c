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
    return YAN_OK;
}

static YanStatus translate(const YanBus *bus, uint32_t address, size_t width,
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

YanBusResult yan_bus_read(const YanBus *bus, uint32_t address, size_t width,
                          uint32_t *value)
{
    size_t offset = 0;
    YanStatus status = value == NULL ? YAN_INVALID_ARGUMENT
                                    : translate(bus, address, width, &offset);
    if (status == YAN_OK) {
        status = yan_ram_read(bus->ram, offset, width, value);
    }
    YanBusResult result = {status, address, width, YAN_ACCESS_READ};
    return result;
}

YanBusResult yan_bus_write(YanBus *bus, uint32_t address, size_t width,
                           uint32_t value)
{
    size_t offset = 0;
    YanStatus status = translate(bus, address, width, &offset);
    if (status == YAN_OK) {
        status = yan_ram_write(bus->ram, offset, width, value);
    }
    YanBusResult result = {status, address, width, YAN_ACCESS_WRITE};
    return result;
}
