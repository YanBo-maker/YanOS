#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "yan/machine.h"

int main(void)
{
    YanMachine machine = {0};
    const uint8_t image[] = {0x93, 0x02, 0x70, 0x00};
    uint32_t instruction = 0;
    uint32_t x5 = UINT32_MAX;
    int exit_code = EXIT_FAILURE;

    if (yan_machine_init(&machine) != YAN_OK) {
        fputs("Cannot initialize machine.\n", stderr);
        return EXIT_FAILURE;
    }
    if (yan_machine_load_image(&machine, image, sizeof image) != YAN_OK) {
        goto cleanup;
    }
    printf("PC before fetch: 0x%08" PRIx32 "\n", machine.cpu.pc);
    if (yan_cpu_fetch(&machine.cpu, &machine.bus, &instruction).status != YAN_OK ||
        instruction != UINT32_C(0x00700293) || machine.cpu.pc != YAN_RAM_BASE ||
        yan_cpu_read_reg(&machine.cpu, 5, &x5) != YAN_OK || x5 != 0) {
        goto cleanup;
    }
    printf("Instruction word: 0x%08" PRIx32 "\n", instruction);
    printf("PC after fetch:  0x%08" PRIx32 "\nx5 after fetch:  %" PRIu32 "\n",
           machine.cpu.pc, x5);

    if (yan_cpu_write_reg(&machine.cpu, 5, 99) != YAN_OK ||
        yan_cpu_reset(&machine.cpu, YAN_RAM_BASE) != YAN_OK ||
        yan_cpu_read_reg(&machine.cpu, 5, &x5) != YAN_OK || x5 != 0 ||
        yan_cpu_fetch(&machine.cpu, &machine.bus, &instruction).status != YAN_OK ||
        instruction != UINT32_C(0x00700293)) {
        goto cleanup;
    }
    printf("After CPU reset:     RAM word = 0x%08" PRIx32 "\n", instruction);
    if (yan_machine_reset(&machine) != YAN_OK ||
        yan_cpu_fetch(&machine.cpu, &machine.bus, &instruction).status != YAN_OK ||
        instruction != 0 || machine.cpu.pc != YAN_RAM_BASE) {
        goto cleanup;
    }
    printf("After Machine reset: RAM word = 0x%08" PRIx32 "\n", instruction);
    exit_code = EXIT_SUCCESS;

cleanup:
    if (exit_code != EXIT_SUCCESS) {
        fputs("Fetch demonstration failed.\n", stderr);
    }
    yan_machine_destroy(&machine);
    return exit_code;
}
