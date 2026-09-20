#include "yan/machine.h"

#include <string.h>

static YanStatus check_machine(const YanMachine *machine)
{
    if (machine == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (machine->ram.data == NULL || machine->ram.size != YAN_RAM_SIZE ||
        machine->bus.ram != &machine->ram || machine->bus.ram_base != YAN_RAM_BASE) {
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

YanStatus yan_machine_init(YanMachine *machine)
{
    if (machine == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (machine->ram.data != NULL || machine->ram.size != 0 ||
        machine->bus.ram != NULL || machine->bus.ram_base != 0 ||
        machine->bus.clint != NULL || machine->bus.plic != NULL ||
        machine->bus.uart != NULL || machine->bus.transport != NULL) {
        return YAN_INVALID_STATE;
    }
    YanStatus status = yan_ram_init(&machine->ram, YAN_RAM_SIZE);
    if (status != YAN_OK) {
        return status;
    }
    status = yan_bus_init(&machine->bus, &machine->ram, YAN_RAM_BASE);
    if (status == YAN_OK) {
        yan_clint_reset(&machine->clint);
        yan_plic_reset(&machine->plic);
        yan_uart_reset(&machine->uart);
        yan_transport_reset(&machine->transport);
        machine->bus.clint = &machine->clint;
        machine->bus.plic = &machine->plic;
        machine->bus.uart = &machine->uart;
        machine->bus.transport = &machine->transport;
        status = yan_cpu_reset(&machine->cpu, YAN_RAM_BASE);
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
    return yan_cpu_reset(&machine->cpu, YAN_RAM_BASE);
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
