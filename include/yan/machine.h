#ifndef YAN_MACHINE_H
#define YAN_MACHINE_H

#include "yan/cpu.h"

#define YAN_RAM_BASE UINT32_C(0x80000000)
#define YAN_RAM_SIZE ((size_t)32 * 1024 * 1024)

/* Zero-initialize; do not copy a live Machine (its Bus borrows its RAM). */
typedef struct {
    YanRam ram;
    YanBus bus;
    YanCpu cpu;
} YanMachine;

YanStatus yan_machine_init(YanMachine *machine);
void yan_machine_destroy(YanMachine *machine);
/* Whole-machine reset clears RAM and resets CPU to YAN_RAM_BASE. */
YanStatus yan_machine_reset(YanMachine *machine);
/* Load at RAM offset zero, preserving the tail. An empty image is valid. */
YanStatus yan_machine_load_image(YanMachine *machine, const uint8_t *image,
                                 size_t size);

#endif
