#ifndef YAN_MACHINE_H
#define YAN_MACHINE_H

#include "yan/cpu.h"
#include "yan/interrupt.h"
#include "yan/uart.h"
#include "yan/transport.h"

#define YAN_RAM_BASE UINT32_C(0x80000000)
#define YAN_RAM_SIZE ((size_t)32 * 1024 * 1024)

/* Platform interrupt wiring. A device only reports whether its IRQ line is
 * asserted; the mapping from a line to a PLIC source belongs to the platform
 * and lives here, not in the device headers. Source 0 stays reserved. */
#define YAN_MACHINE_PLIC_SOURCE_TRANSPORT UINT32_C(1)
#define YAN_MACHINE_PLIC_SOURCE_UART UINT32_C(2)

/* Zero-initialize; do not copy a live Machine (its Bus borrows its RAM). The
 * RAM geometry is recorded here so check_machine can compare the live fields
 * against it. That catches a lost or rewired link, and it catches a record that
 * was zeroed, but it cannot catch a change that updates the record and the
 * fields together: the record is the only reference the check has.
 * YAN_RAM_BASE and YAN_RAM_SIZE are only the defaults yan_machine_init uses. */
typedef struct {
    YanRam ram;
    YanBus bus;
    YanCpu cpu;
    YanClint clint;
    YanPlic plic;
    YanUart uart;
    YanTransport transport;
    uint32_t ram_base;
    size_t ram_size;
} YanMachine;

/* Assemble a machine at the default geometry. */
YanStatus yan_machine_init(YanMachine *machine);
/* Assemble a machine at an explicit geometry. Tools that let the caller choose
 * the RAM window use this instead of hand-rolling a Bus and a CPU, so that
 * every machine advances through yan_machine_step. The window must not overlap
 * a device window: the Bus decodes devices before RAM, so an overlap would make
 * part of the RAM unreachable, and that is rejected rather than assembled. */
YanStatus yan_machine_init_with(YanMachine *machine, uint32_t ram_base,
                                size_t ram_size);
void yan_machine_destroy(YanMachine *machine);
/* Whole-machine reset clears RAM and resets CPU to the machine's own RAM base.
 * Devices keep their host configuration (UART terminal sink, transport ring
 * placement) and lose their run state. */
YanStatus yan_machine_reset(YanMachine *machine);
/* Load at RAM offset zero, preserving the tail. An empty image is valid. */
YanStatus yan_machine_load_image(YanMachine *machine, const uint8_t *image,
                                 size_t size);
YanStatus yan_machine_tick(YanMachine *machine, uint64_t ticks);
/* Raise the PLIC sources driven by platform devices. Called before every step:
 * device lines are level driven, so a source is asserted again while its
 * condition still holds. The PLIC keeps its own enable, priority and threshold
 * arbitration. */
YanStatus yan_machine_sample_devices(YanMachine *machine);
YanStatus yan_machine_step(YanMachine *machine);

#endif
