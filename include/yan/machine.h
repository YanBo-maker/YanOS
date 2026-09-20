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

/* Zero-initialize; do not copy a live Machine (its Bus borrows its RAM). */
typedef struct {
    YanRam ram;
    YanBus bus;
    YanCpu cpu;
    YanClint clint;
    YanPlic plic;
    YanUart uart;
    YanTransport transport;
} YanMachine;

YanStatus yan_machine_init(YanMachine *machine);
void yan_machine_destroy(YanMachine *machine);
/* Whole-machine reset clears RAM and resets CPU to YAN_RAM_BASE. Devices keep
 * their host configuration (UART terminal sink, transport ring placement) and
 * lose their run state. */
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
