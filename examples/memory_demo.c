#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "yan/machine.h"

int main(void)
{
    YanMachine machine = {0};
    const uint8_t image[] = {0x93, 0x02, 0x70, 0x00};
    uint32_t word = 0;
    YanBusResult rejected;
    int exit_code = EXIT_FAILURE;

    if (yan_machine_init(&machine) != YAN_OK) {
        fputs("Cannot initialize machine.\n", stderr);
        return EXIT_FAILURE;
    }
    if (yan_machine_load_image(&machine, image, sizeof image) != YAN_OK ||
        yan_bus_read(&machine.bus, YAN_RAM_BASE, 4, &word).status != YAN_OK ||
        word != UINT32_C(0x00700293)) {
        goto cleanup;
    }
    printf("RAM: 0x%08" PRIx32 " (%zu bytes)\n", YAN_RAM_BASE, machine.ram.size);
    printf("Word: 0x%08" PRIx32 "\nBytes:", word);
    for (uint32_t i = 0; i < sizeof image; ++i) {
        uint32_t byte = 0;
        if (yan_bus_read(&machine.bus, YAN_RAM_BASE + i, 1, &byte).status != YAN_OK ||
            byte != image[i]) {
            goto cleanup;
        }
        printf(" %02" PRIx32, byte);
    }
    putchar('\n');

    rejected = yan_bus_write(&machine.bus, YAN_RAM_BASE + 1, 4, 0);
    if (rejected.status != YAN_UNALIGNED ||
        yan_bus_read(&machine.bus, YAN_RAM_BASE, 4, &word).status != YAN_OK ||
        word != UINT32_C(0x00700293)) {
        goto cleanup;
    }
    puts("Unaligned write: rejected; RAM unchanged.");
    rejected = yan_bus_read(&machine.bus, UINT32_C(0x10000000), 4, &word);
    if (rejected.status != YAN_UNMAPPED || word != UINT32_C(0x00700293)) {
        goto cleanup;
    }
    puts("Unmapped read: rejected; output unchanged.");
    exit_code = EXIT_SUCCESS;

cleanup:
    if (exit_code != EXIT_SUCCESS) {
        fputs("Memory demonstration failed.\n", stderr);
    }
    yan_machine_destroy(&machine);
    return exit_code;
}
