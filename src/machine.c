#include "yan/machine.h"

#include <string.h>

static YanStatus check_machine(const YanMachine *machine)
{
    if (machine == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (machine->ram.data == NULL || machine->ram.size != machine->ram_size ||
        machine->ram_size == 0 || machine->bus.ram != &machine->ram ||
        machine->bus.ram_base != machine->ram_base) {
        return YAN_INVALID_STATE;
    }
    /* The Bus reaches the devices through these links; a Machine that lost one
     * is not usable, and silently ignoring interrupts would hide that. */
    if (machine->bus.clint != &machine->clint || machine->bus.plic != &machine->plic ||
        machine->bus.uart != &machine->uart ||
        machine->bus.transport != &machine->transport) {
        return YAN_INVALID_STATE;
    }
    return YAN_OK;
}

/* The Bus decodes device windows before RAM, so a RAM window that overlapped
 * one would be partly invisible to the guest with no diagnostic at all.
 * Assembly refuses such a layout instead of building a machine that quietly
 * loses memory. The exclusive end may reach 2^32, which is why the arithmetic
 * is done in 64 bits. */
static int window_overlaps(uint32_t base, size_t size, uint32_t device_base,
                           uint32_t device_size)
{
    const uint64_t start = base;
    const uint64_t end = start + size;
    const uint64_t device_start = device_base;
    const uint64_t device_end = device_start + device_size;
    return start < device_end && device_start < end;
}

static int ram_overlaps_a_device(uint32_t base, size_t size)
{
    return window_overlaps(base, size, YAN_CLINT_BASE, YAN_CLINT_SIZE) ||
           window_overlaps(base, size, YAN_PLIC_BASE, YAN_PLIC_SIZE) ||
           window_overlaps(base, size, YAN_UART_BASE, YAN_UART_SIZE) ||
           window_overlaps(base, size, YAN_TRANSPORT_BASE, YAN_TRANSPORT_SIZE);
}

YanStatus yan_machine_init(YanMachine *machine)
{
    return yan_machine_init_with(machine, YAN_RAM_BASE, YAN_RAM_SIZE);
}

YanStatus yan_machine_init_with(YanMachine *machine, uint32_t ram_base,
                                size_t ram_size)
{
    if (machine == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (ram_size == 0) {
        return YAN_INVALID_ARGUMENT;
    }
    if (ram_overlaps_a_device(ram_base, ram_size)) {
        return YAN_INVALID_ARGUMENT;
    }
    if (machine->ram.data != NULL || machine->ram.size != 0 ||
        machine->bus.ram != NULL || machine->bus.ram_base != 0 ||
        machine->bus.clint != NULL || machine->bus.plic != NULL ||
        machine->bus.uart != NULL || machine->bus.transport != NULL) {
        return YAN_INVALID_STATE;
    }
    YanStatus status = yan_ram_init(&machine->ram, ram_size);
    if (status != YAN_OK) {
        return status;
    }
    status = yan_bus_init(&machine->bus, &machine->ram, ram_base);
    if (status == YAN_OK) {
        yan_clint_reset(&machine->clint);
        yan_plic_reset(&machine->plic);
        yan_uart_reset(&machine->uart);
        yan_transport_reset(&machine->transport);
        machine->bus.clint = &machine->clint;
        machine->bus.plic = &machine->plic;
        machine->bus.uart = &machine->uart;
        machine->bus.transport = &machine->transport;
        machine->ram_base = ram_base;
        machine->ram_size = ram_size;
        status = yan_cpu_reset(&machine->cpu, ram_base);
    }
    if (status != YAN_OK) {
        yan_machine_destroy(machine);
    }
    return status;
}

void yan_machine_destroy(YanMachine *machine)
{
    if (machine != NULL) {
        machine->bus.ram = NULL;
        machine->bus.ram_base = 0;
        machine->bus.clint = NULL;
        machine->bus.plic = NULL;
        machine->bus.uart = NULL;
        machine->bus.transport = NULL;
        machine->clint = (YanClint){0};
        machine->plic = (YanPlic){0};
        machine->uart = (YanUart){0};
        machine->transport = (YanTransport){0};
        machine->ram_base = 0;
        machine->ram_size = 0;
        yan_ram_destroy(&machine->ram);
        machine->cpu = (YanCpu){0};
    }
}

YanStatus yan_machine_reset(YanMachine *machine)
{
    YanStatus status = check_machine(machine);
    if (status != YAN_OK) {
        return status;
    }
    status = yan_ram_clear(&machine->ram);
    if (status != YAN_OK) {
        return status;
    }
    yan_clint_reset(&machine->clint);
    yan_plic_reset(&machine->plic);
    yan_uart_reset(&machine->uart);
    yan_transport_reset(&machine->transport);
    return yan_cpu_reset(&machine->cpu, machine->ram_base);
}

YanStatus yan_machine_load_image(YanMachine *machine, const uint8_t *image,
                                 size_t size)
{
    YanStatus status = check_machine(machine);
    if (status != YAN_OK) {
        return status;
    }
    if (image == NULL && size != 0) {
        return YAN_INVALID_ARGUMENT;
    }
    if (size > machine->ram.size) {
        return YAN_OUT_OF_BOUNDS;
    }
    /* Avoid passing NULL even for a zero-byte operation; allow overlapping images. */
    if (size != 0) {
        memmove(machine->ram.data, image, size);
    }
    return YAN_OK;
}

/* Time only advances when the host asks for it, so a run is reproducible. */
YanStatus yan_machine_tick(YanMachine *machine, uint64_t ticks)
{
    YanStatus status = check_machine(machine);
    if (status != YAN_OK) {
        return status;
    }
    yan_clint_tick(&machine->clint, ticks);
    return YAN_OK;
}

/* Device lines report a level. The platform maps each line to a PLIC source
 * and drives it on every sample, in both directions; the PLIC gateway alone
 * decides whether the line produces MEIP. */
YanStatus yan_machine_sample_devices(YanMachine *machine)
{
    YanStatus status = check_machine(machine);
    if (status != YAN_OK) {
        return status;
    }
    yan_plic_set_level(&machine->plic, YAN_MACHINE_PLIC_SOURCE_UART,
                       yan_uart_pending(&machine->uart));
    yan_plic_set_level(&machine->plic, YAN_MACHINE_PLIC_SOURCE_TRANSPORT,
                       yan_transport_pending(&machine->transport));
    return YAN_OK;
}

/* One cycle: advance the timer by one tick, publish the device lines, then
 * execute one instruction. An interrupt raised by either is therefore visible
 * to this same step. */
YanStatus yan_machine_step(YanMachine *machine)
{
    YanStatus status = check_machine(machine);
    if (status != YAN_OK) {
        return status;
    }
    yan_clint_tick(&machine->clint, 1);
    status = yan_machine_sample_devices(machine);
    if (status != YAN_OK) {
        return status;
    }
    return yan_cpu_step(&machine->cpu, &machine->bus);
}
