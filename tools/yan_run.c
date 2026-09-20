/* The terminal backend reads standard input with poll()/read(). The POSIX
 * feature level has to be chosen before the first system header is included,
 * which is why it is set here as well as in host_terminal.c; yan_difftest.c
 * picks the same level. */
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_block.h"
#include "host_file.h"
#include "host_terminal.h"
#include "yan/cpu.h"
#include "yan/image.h"
#include "yan/machine.h"

/* host_terminal.c is a standalone translation unit and is listed in the
 * yan_run target; it is not included here. */

/* Between instructions, not on every one: asking standard input for a byte is a
 * syscall, and a byte handed to the device waits there until the Guest reads
 * RXDATA, so a short interval costs nothing. */
#define TERMINAL_POLL_INTERVAL UINT64_C(256)

/* The channel rings live in guest RAM, and the host addresses them by absolute
 * address. 0018 sets the floor: a one-block write request is 16 + 4096 bytes and
 * a ring holds RING_SIZE - 1 of them, so 8192 is the smallest power of two that
 * can carry one block. The two rings are adjacent and need twice that. */
#define DISK_RING_SIZE UINT32_C(8192)
#define DISK_RING_BYTES (2U * DISK_RING_SIZE)
/* Room left below the rings for the image, its stack and its .bss. A Guest that
 * wants the last 16 KiB of the RAM window cannot run with --disk; the
 * alternative is a Guest quietly overrunning the rings. */
#define DISK_RAM_MARGIN (UINT32_C(64) * 1024U)

/* A doorbell write is the guest saying "there is a request". The callback only
 * records that: serving from inside a device access would run the whole protocol
 * in the middle of a Guest store. The service runs between instructions instead,
 * like the terminal's receive poll.
 *
 * Registering a callback is also what makes HOST_READY true. 0014 defines that
 * bit as "a ring is configured and a notify callback is registered", so without
 * this the Guest would see HOST_READY=0 and correctly refuse to touch the
 * rings. */
static void disk_notify(void *context)
{
    *(int *)context = 1;
}

/* Exit codes are shared with yan_difftest and documented in
 * docs/specs/0011-cpu-validation.md. A harness must not read "nonzero" as a
 * single failure: 4 (never terminated), 5 (Host failure), 6 (the Guest
 * reported a failure) and 7 (a signature region was exported) mean different
 * things. */
enum {
    EXIT_PASS = 0,
    EXIT_USAGE = 2,
    EXIT_NO_TERMINATION = 4,
    EXIT_HOST_ERROR = 5,
    EXIT_GUEST_FAILURE = 6,
    EXIT_SIGNATURE = 7
};

typedef struct {
    const char *image;
    const char *trace;
    const char *signature;
    uint32_t base;
    uint32_t ram_size;
    uint64_t max_steps;
    uint32_t tohost;
    int tohost_given;
    int ignore_tohost;
    uint32_t signature_start;
    uint32_t signature_end;
    int terminal;
    uint64_t disk_blocks;
    int disk;
    int help;
} Options;

/* One text, two audiences: a successful `--help` writes it to standard output
 * and exits 0, while a bad command line writes the same text to standard error
 * and exits 2. Keeping a single copy is what stops the two from drifting. */
static void usage(FILE *stream, const char *program)
{
    fprintf(stream, "usage: %s --image FILE [--base ADDR] [--ram BYTES] "
            "[--max-steps N] [--tohost ADDR] [--ignore-tohost] [--terminal] "
            "[--disk BLOCKS] [--trace FILE] [--signature FILE START END]\n"
            "  -h, --help      print this message and exit\n"
            "  --tohost ADDR   stop when the word at ADDR becomes nonzero\n"
            "  --ignore-tohost run to the step limit; use when a Guest writes\n"
            "                  to `tohost` for something other than halting\n"
            "  --terminal      attach the host terminal: UART output goes to\n"
            "                  standard output and standard input feeds the\n"
            "                  UART receiver; without it the UART stays\n"
            "                  unconnected and the Guest sees CONNECTED=0\n"
            "  --disk BLOCKS   attach a memory block device of BLOCKS 4096-byte\n"
            "                  blocks to the host transport channel and serve\n"
            "                  the block protocol on it; the top 16 KiB of the\n"
            "                  RAM window is reserved for the two rings\n"
            "exit: 0 tohost PASS, 2 usage, 4 no termination, 5 Host error, "
            "6 Guest failure, 7 signature exported\n", program);
}

static int number(const char *text, uint64_t *value)
{
    char *end = NULL;
    *value = strtoull(text, &end, 0);
    return end != text && *end == '\0';
}

static int parse_options(int argc, char **argv, Options *options)
{
    *options = (Options){.base = UINT32_C(0x80000000), .ram_size = 16U * 1024U * 1024U,
        .max_steps = UINT64_C(1000000)};
    for (int i = 1; i < argc; ++i) {
        uint64_t value = 0;
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) options->image = argv[++i];
        else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) options->trace = argv[++i];
        else if (strcmp(argv[i], "--signature") == 0 && i + 3 < argc) {
            options->signature = argv[++i];
            if (!number(argv[++i], &value)) return 0;
            options->signature_start = (uint32_t)value;
            if (!number(argv[++i], &value)) return 0;
            options->signature_end = (uint32_t)value;
        } else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return 0;
            options->base = (uint32_t)value;
        } else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value) || value > SIZE_MAX) return 0;
            options->ram_size = (uint32_t)value;
        } else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &options->max_steps)) return 0;
        } else if (strcmp(argv[i], "--ignore-tohost") == 0) {
            options->ignore_tohost = 1;
        } else if (strcmp(argv[i], "--terminal") == 0) {
            options->terminal = 1;
        } else if (strcmp(argv[i], "--disk") == 0 && i + 1 < argc) {
            /* A device of zero blocks answers every request as out of range,
             * which is a test of the error path and never a useful disk, so the
             * command line rejects it rather than passing it through. */
            if (!number(argv[++i], &options->disk_blocks) ||
                options->disk_blocks == 0 ||
                options->disk_blocks > SIZE_MAX / YAN_HOST_BLOCK_BLOCK_SIZE) {
                return 0;
            }
            options->disk = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            options->help = 1;
        } else if (strcmp(argv[i], "--tohost") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return 0;
            options->tohost = (uint32_t)value;
            options->tohost_given = 1;
        } else return 0;
    }
    /* Help stands on its own: it needs no image, and asking for it is not a
     * usage error. An unknown option is still rejected above. */
    return options->help ||
           (options->image != NULL && options->ram_size != 0);
}

static int dump_signature(const Options *options, const YanRam *ram)
{
    if (options->signature == NULL) {
        return 1;
    }
    if (options->signature_start < options->base ||
        options->signature_end < options->signature_start ||
        (uint64_t)(options->signature_end - options->base) > ram->size ||
        options->signature_end - options->base >
            ram->size - (options->signature_start - options->base)) {
        return 0;
    }
    FILE *file = fopen(options->signature, "wb");
    if (file == NULL) {
        return 0;
    }
    const size_t offset = options->signature_start - options->base;
    const size_t length = options->signature_end - options->signature_start;
    const int result = fwrite(ram->data + offset, 1, length, file) == length;
    fclose(file);
    return result;
}

static void write_trace(FILE *trace, uint64_t step, const YanCpuState *before,
                        uint32_t instruction, const YanCpuState *after,
                        YanStatus status)
{
    fprintf(trace, "{\"step\":%" PRIu64 ",\"pc\":%" PRIu32 ",\"insn\":%" PRIu32
            ",\"next_pc\":%" PRIu32 ",\"status\":%d,\"regs\":[",
            step, before->pc, instruction, after->pc, (int)status);
    for (size_t i = 0; i < YAN_REGISTER_COUNT; ++i) {
        fprintf(trace, "%s%" PRIu32, i == 0 ? "" : ",", after->regs[i]);
    }
    fprintf(trace, "],\"mstatus\":%" PRIu32 ",\"mepc\":%" PRIu32
            ",\"mcause\":%" PRIu32 ",\"mtval\":%" PRIu32 "}\n",
            after->csr.mstatus, after->csr.mepc, after->csr.mcause, after->csr.mtval);
}

int main(int argc, char **argv)
{
    Options options;
    if (!parse_options(argc, argv, &options)) {
        usage(stderr, argv[0]);
        return EXIT_USAGE;
    }
    if (options.help) {
        usage(stdout, argv[0]);
        return EXIT_PASS;
    }
    /* The platform is driven through its own entry point. Assembling a Bus and
     * a CPU here would skip the device sampling yan_machine_step() performs, and
     * a UART receive interrupt could then never reach the Guest; the geometry
     * options are passed in instead of being copied out of a hand-built Bus. */
    YanMachine machine = {0};
    YanHostTerminal terminal = {0};
    YanHostBlock block = {0};
    YanImageInfo info = {0};
    uint8_t *image = NULL;
    uint8_t *disk_storage = NULL;
    size_t image_size = 0;
    FILE *trace = NULL;
    int doorbell_rung = 0;
    int disk_attached = 0;
    int result = EXIT_HOST_ERROR;
    int stopped = 0;

    /* The CLINT, PLIC, UART and transport windows are mapped for every run, and
     * mtime advances one tick per executed instruction. The differential tester
     * deliberately does not do either: its reference model carries no device or
     * CSR state. */
    if (yan_machine_init_with(&machine, options.base, options.ram_size) != YAN_OK) {
        goto done;
    }
    /* The device is attached before the image is loaded, so the Guest never runs
     * in a window where the channel is mapped but unconfigured. */
    if (options.disk) {
        if (options.ram_size < DISK_RING_BYTES + DISK_RAM_MARGIN) {
            fprintf(stderr, "yan_run: --disk reserves the top %u bytes of the RAM "
                    "window for the channel rings and needs at least %u\n",
                    (unsigned)DISK_RING_BYTES,
                    (unsigned)(DISK_RING_BYTES + DISK_RAM_MARGIN));
            goto done;
        }
        const uint32_t ring_base = options.base + options.ram_size - DISK_RING_BYTES;
        if (yan_transport_configure(&machine.transport, ring_base, DISK_RING_SIZE,
                                    options.base, options.ram_size) != YAN_OK) {
            fprintf(stderr, "yan_run: cannot place the channel rings at %08" PRIx32
                    "\n", ring_base);
            goto done;
        }
        yan_transport_set_notify(&machine.transport, disk_notify, &doorbell_rung);
        disk_storage = calloc((size_t)options.disk_blocks, YAN_HOST_BLOCK_BLOCK_SIZE);
        if (disk_storage == NULL ||
            yan_host_block_init(&block, disk_storage, options.disk_blocks) != YAN_OK) {
            fprintf(stderr, "yan_run: cannot allocate %" PRIu64
                    " blocks of backing store\n", options.disk_blocks);
            goto done;
        }
        disk_attached = 1;
    }
    /* A backend is host configuration; without one the UART reports CONNECTED=0
     * and refuses both directions with YAN_UNAVAILABLE, which is the headless
     * path of docs/specs/0015-uart-device.md, not an undecoded window. It stays
     * silent either way. */
    if (options.terminal) {
        yan_host_terminal_init(&terminal);
        const YanUartTerminal backend = yan_host_terminal_backend(&terminal);
        if (yan_uart_set_terminal(&machine.uart, &backend) != YAN_OK) {
            /* The backend has both callbacks, so this cannot happen; failing
             * loudly beats running a Guest that will never see TX_READY. */
            fprintf(stderr, "yan_run: cannot attach the host terminal\n");
            goto done;
        }
    }
    image = yan_host_read_file(options.image, &image_size);
    if (image == NULL) {
        fprintf(stderr, "yan_run: cannot read '%s'\n", options.image);
        goto done;
    }
    if (yan_image_load_elf(&machine.ram, options.base, image, image_size,
                           &info) != YAN_OK) {
        fprintf(stderr, "yan_run: '%s' is not a loadable RV32 ELF image\n",
                options.image);
        goto done;
    }
    if (yan_cpu_reset(&machine.cpu, info.entry) != YAN_OK) {
        goto done;
    }
    /* An explicit address wins; otherwise the symbol decides, because test
     * linkers do not place `tohost` at a fixed address. `--ignore-tohost`
     * skips this entirely: some frameworks use that same word as a console
     * register, so its first nonzero value is not a halt request. */
    if (!options.ignore_tohost && !options.tohost_given &&
        yan_image_find_symbol(image, image_size, "tohost", &options.tohost) == YAN_OK) {
        options.tohost_given = 1;
    }
    if (options.trace != NULL) {
        trace = fopen(options.trace, "w");
        if (trace == NULL) {
            goto done;
        }
    }
    for (uint64_t step = 0; step < options.max_steps; ++step) {
        uint32_t instruction = 0;
        YanCpuState before = {0}, after = {0};
        /* Feed the receiver between instructions, but not on every one: the
         * byte waits in the device until the Guest reads RXDATA, and an empty
         * standard input is answered without blocking or a diagnostic. */
        if (options.terminal && step % TERMINAL_POLL_INTERVAL == 0) {
            (void)yan_host_terminal_poll_rx(&terminal, &machine.uart);
        }
        /* Served before the step, not after: publishing a response asserts the
         * channel's interrupt line, and the step that samples the device lines
         * is the step that can deliver it. */
        if (disk_attached && doorbell_rung) {
            doorbell_rung = 0;
            (void)yan_host_block_service(&block, &machine.transport, &machine.ram,
                                         options.base);
        }
        /* Read the word this step will execute, before the step runs it. The
         * read is a plain bus read with no side effects, and it keeps both the
         * trace's `insn` field and the "cannot fetch" diagnostic; advancing the
         * machine stays yan_machine_step()'s job. */
        if (yan_cpu_snapshot(&machine.cpu, &before) != YAN_OK ||
            yan_cpu_fetch(&machine.cpu, &machine.bus, &instruction).status != YAN_OK) {
            fprintf(stderr, "yan_run: cannot fetch at pc = %08" PRIx32 "\n",
                    machine.cpu.pc);
            goto done;
        }
        /* One cycle: publish the device interrupt lines, advance the timer and
         * execute. An interrupt raised by this cycle is visible to this same
         * step, exactly as the platform's own step defines it. */
        const YanStatus status = yan_machine_step(&machine);
        (void)yan_cpu_snapshot(&machine.cpu, &after);
        if (trace != NULL) {
            write_trace(trace, step, &before, instruction, &after, status);
        }
        if (!options.ignore_tohost && options.tohost_given) {
            uint32_t value = 0;
            if (yan_bus_read(&machine.bus, options.tohost, 4, &value).status == YAN_OK &&
                value != 0) {
                if (value != 1) {
                    fprintf(stderr, "yan_run: the Guest reported failure code"
                            " %" PRIu32 " after %" PRIu64 " instructions\n",
                            value, step + 1);
                }
                result = value == 1 ? EXIT_PASS : EXIT_GUEST_FAILURE;
                stopped = 1;
                break;
            }
        }
    }
    if (!stopped && result == EXIT_HOST_ERROR) {
        result = options.signature != NULL ? EXIT_SIGNATURE : EXIT_NO_TERMINATION;
        if (options.ignore_tohost) {
            fprintf(stderr, "yan_run: ran the full %" PRIu64
                    " steps with tohost polling disabled\n", options.max_steps);
        } else {
            fprintf(stderr, "yan_run: stopped after %" PRIu64
                    " steps without reaching tohost\n", options.max_steps);
        }
    }
    if (!dump_signature(&options, &machine.ram)) {
        fprintf(stderr, "yan_run: cannot export the signature region\n");
        result = EXIT_HOST_ERROR;
    }
done:
    if (trace != NULL) {
        fclose(trace);
    }
    free(image);
    free(disk_storage);
    yan_machine_destroy(&machine);
    return result;
}
