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
    if (machine->bus.clint != &machine->clint || machine->bus.plic != &machine->plic) {
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
        machine->bus.clint != NULL || machine->bus.plic != NULL) {
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
        machine->bus.clint = &machine->clint;
        machine->bus.plic = &machine->plic;
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
        machine->clint = (YanClint){0};
        machine->plic = (YanPlic){0};
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

/* One cycle: advance the timer by one tick, then execute one instruction. An
 * interrupt raised by this tick is therefore visible to this same step. */
YanStatus yan_machine_step(YanMachine *machine)
{
    YanStatus status = check_machine(machine);
    if (status != YAN_OK) {
        return status;
    }
    yan_clint_tick(&machine->clint, 1);
    return yan_cpu_step(&machine->cpu, &machine->bus);
}
