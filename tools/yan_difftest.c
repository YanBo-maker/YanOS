/* YanCPU differential testing against an external reference model.
 *
 * The reference is a NEMU riscv32 shared object exporting the ysyx difftest
 * API (difftest_init / memcpy / regcpy / exec). Both models start from the
 * same memory image and the same reset state, and after every committed
 * instruction YanCPU's pc and 32 general registers are compared with the
 * reference's. CSR, privilege level and trap semantics are outside what that
 * reference can mirror, so a Guest image for this tool must stay inside the
 * unprivileged RV32IM instruction set.
 */
#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host_file.h"
#include "yan/cpu.h"
#include "yan/difftest.h"
#include "yan/image.h"

/* Exit codes are shared with yan_run and documented in
 * docs/specs/0011-cpu-validation.md. */
enum {
    EXIT_PASS = 0,
    EXIT_MISMATCH = 1,
    EXIT_USAGE = 2,
    EXIT_DUT_TRAP = 3,
    EXIT_NO_TERMINATION = 4,
    EXIT_HOST_ERROR = 5,
    EXIT_GUEST_FAILURE = 6
};

/* Mirrors the layout the reference exports: 32 GPRs followed by pc. */
typedef struct {
    uint32_t regs[YAN_REGISTER_COUNT];
    uint32_t pc;
} RefState;

enum { REF_TO_DUT = 0, REF_TO_REF = 1 };

typedef struct {
    void *handle;
    void (*init)(int);
    void (*copy_memory)(uint32_t, void *, size_t, int);
    void (*copy_registers)(void *, int);
    void (*execute)(uint64_t);
} RefModel;

typedef struct {
    const char *image;
    const char *ref_so;
    const char *trace;
    const char *report;
    uint32_t base;
    uint32_t ram_size;
    uint64_t max_steps;
    uint32_t tohost;
    int tohost_given;
    int check_memory;
} Options;

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s --image FILE --ref-so FILE [--base ADDR] "
            "[--ram BYTES] [--max-steps N] [--tohost ADDR] [--trace FILE] "
            "[--report FILE] [--no-memory-check]\n"
            "exit: 0 pass, 1 mismatch, 2 usage, 3 Guest trap (out of scope), "
            "4 no termination, 5 Host or reference failure, 6 Guest failure\n",
            program);
}

static int number(const char *text, uint64_t *value)
{
    char *end = NULL;
    *value = strtoull(text, &end, 0);
    return end != text && *end == '\0';
}

static int parse_options(int argc, char **argv, Options *options)
{
    *options = (Options){.base = UINT32_C(0x80000000),
        .ram_size = 16U * 1024U * 1024U, .max_steps = UINT64_C(1000000),
        .check_memory = 1};
    for (int i = 1; i < argc; ++i) {
        uint64_t value = 0;
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) options->image = argv[++i];
        else if (strcmp(argv[i], "--ref-so") == 0 && i + 1 < argc) options->ref_so = argv[++i];
        else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) options->trace = argv[++i];
        else if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) options->report = argv[++i];
        else if (strcmp(argv[i], "--no-memory-check") == 0) options->check_memory = 0;
        else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return 0;
            options->base = (uint32_t)value;
        } else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value) || value > SIZE_MAX) return 0;
            options->ram_size = (uint32_t)value;
        } else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &options->max_steps)) return 0;
        } else if (strcmp(argv[i], "--tohost") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return 0;
            options->tohost = (uint32_t)value;
            options->tohost_given = 1;
        } else return 0;
    }
    return options->image != NULL && options->ref_so != NULL &&
           options->ram_size != 0;
}

/* The reference asserts (and therefore aborts the Host) on instructions it
 * does not implement. Report that as a reference failure instead of dying
 * without a diagnosis. */
static void report_abort(int number)
{
    (void)number;
    static const char message[] =
        "yan_difftest: the reference model aborted (unsupported instruction or "
        "a Host assertion). See the reference output above.\n";
    (void)!write(STDERR_FILENO, message, sizeof message - 1);
    _exit(EXIT_HOST_ERROR);
}

/* dlopen hands back a void pointer and ISO C forbids converting one to a
 * function pointer, so the symbol is copied into place instead of cast. */
static void load_symbol(void *handle, const char *name, void *destination,
                        size_t size)
{
    void *symbol = dlsym(handle, name);
    if (symbol == NULL) {
        memset(destination, 0, size);
        return;
    }
    memcpy(destination, &symbol, size);
}

static int load_ref_model(const char *path, RefModel *model)
{
    /* RTLD_LAZY: the reference links readline through its debugger front end
     * and does not resolve those symbols unless they are used. */
    model->handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (model->handle == NULL) {
        fprintf(stderr, "yan_difftest: dlopen('%s') failed: %s\n", path, dlerror());
        return 0;
    }
    load_symbol(model->handle, "difftest_init", &model->init, sizeof model->init);
    load_symbol(model->handle, "difftest_memcpy", &model->copy_memory,
                sizeof model->copy_memory);
    load_symbol(model->handle, "difftest_regcpy", &model->copy_registers,
                sizeof model->copy_registers);
    load_symbol(model->handle, "difftest_exec", &model->execute,
                sizeof model->execute);
    if (model->init == NULL || model->copy_memory == NULL ||
        model->copy_registers == NULL || model->execute == NULL) {
        fprintf(stderr, "yan_difftest: '%s' does not export the difftest API\n", path);
        dlclose(model->handle);
        model->handle = NULL;
        return 0;
    }
    return 1;
}

static int compare_memory(const RefModel *model, const YanRam *ram, uint32_t base,
                          uint32_t *offset, uint32_t *dut_value, uint32_t *ref_value)
{
    static uint8_t window[65536];
    for (size_t at = 0; at < ram->size; at += sizeof window) {
        const size_t length = ram->size - at < sizeof window ? ram->size - at
                                                             : sizeof window;
        model->copy_memory(base + (uint32_t)at, window, length, REF_TO_DUT);
        if (memcmp(ram->data + at, window, length) == 0) {
            continue;
        }
        for (size_t index = 0; index < length; ++index) {
            if (ram->data[at + index] != window[index]) {
                *offset = (uint32_t)(at + index);
                *dut_value = ram->data[at + index];
                *ref_value = window[index];
                return 0;
            }
        }
    }
    return 1;
}

/* YanCpuState also carries CSR fields the reference does not have, so copy
 * the mirrored fields instead of type punning a different struct layout. */
static YanCpuState ref_as_state(const RefState *ref)
{
    YanCpuState state = {0};
    memcpy(state.regs, ref->regs, sizeof state.regs);
    state.pc = ref->pc;
    return state;
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
    fprintf(trace, "]}\n");
}

int main(int argc, char **argv)
{
    Options options;
    if (!parse_options(argc, argv, &options)) {
        usage(argv[0]);
        return EXIT_USAGE;
    }
    YanRam ram = {0};
    YanBus bus = {0};
    YanCpu cpu = {0};
    YanImageInfo info = {0};
    RefModel model = {0};
    uint8_t *image = NULL;
    size_t image_size = 0;
    FILE *trace = NULL;
    FILE *report = NULL;
    int result = EXIT_HOST_ERROR;

    (void)signal(SIGABRT, report_abort);
    if (yan_ram_init(&ram, options.ram_size) != YAN_OK ||
        yan_bus_init(&bus, &ram, options.base) != YAN_OK) {
        goto done;
    }
    image = yan_host_read_file(options.image, &image_size);
    if (image == NULL) {
        fprintf(stderr, "yan_difftest: cannot read '%s'\n", options.image);
        goto done;
    }
    if (yan_image_load_elf(&ram, options.base, image, image_size, &info) != YAN_OK) {
        fprintf(stderr, "yan_difftest: '%s' is not a loadable RV32 ELF image\n",
                options.image);
        goto done;
    }
    if (yan_cpu_reset(&cpu, info.entry) != YAN_OK || !load_ref_model(options.ref_so, &model)) {
        goto done;
    }
    if (!options.tohost_given &&
        yan_image_find_symbol(image, image_size, "tohost", &options.tohost) == YAN_OK) {
        options.tohost_given = 1;
    }
    if (options.trace != NULL) {
        trace = fopen(options.trace, "w");
        if (trace == NULL) {
            goto done;
        }
    }
    if (options.report != NULL) {
        report = fopen(options.report, "w");
        if (report == NULL) {
            goto done;
        }
    }

    /* Both models observe exactly the same memory: the whole YanCPU RAM is
     * copied into the reference after its own initialisation. */
    model.init(0);
    model.copy_memory(options.base, ram.data, ram.size, REF_TO_REF);
    RefState ref = {0};
    ref.pc = cpu.pc;
    model.copy_registers(&ref, REF_TO_REF);

    for (uint64_t step = 0; step < options.max_steps; ++step) {
        uint32_t instruction = 0;
        YanCpuState before = {0}, after = {0};
        (void)yan_cpu_snapshot(&cpu, &before);
        (void)yan_cpu_fetch(&cpu, &bus, &instruction);
        const YanStatus status = yan_cpu_step(&cpu, &bus);
        (void)yan_cpu_snapshot(&cpu, &after);
        model.execute(1);
        model.copy_registers(&ref, REF_TO_DUT);
        if (trace != NULL) {
            write_trace(trace, step, &before, instruction, &after, status);
        }

        YanDiffResult diff = {0};
        const YanCpuState ref_state = ref_as_state(&ref);
        (void)yan_difftest_compare(&after, &ref_state, &diff);

        /* A trap is a category this reference model cannot mirror, so it is
         * reported as out of scope rather than as an architectural mismatch.
         * The comparison above still runs, so the report can state whether the
         * states also diverged. The pc printed is the address of the
         * instruction that was executed, not the pc it left behind. */
        if (status == YAN_TRAP) {
            char line[2048];
            if (yan_difftest_report(&diff, &after, &ref_state, step, before.pc,
                                    instruction, line, sizeof line) != 0) {
                if (report != NULL) {
                    fputs(line, report);
                }
                fputs(line, stderr);
            }
            fprintf(stderr,
                    "yan_difftest: the DUT entered a trap at step %" PRIu64
                    " pc = %08" PRIx32 " insn = %08" PRIx32
                    " (mcause = %" PRIu32 ", next pc = %08" PRIx32
                    ", states %s). CSR, trap and privilege semantics are not"
                    " covered by this reference model.\n",
                    step, before.pc, instruction, after.csr.mcause, diff.pc_dut,
                    diff.equal ? "still agreed" : "diverged");
            result = EXIT_DUT_TRAP;
            goto done;
        }

        if (!diff.equal) {
            char line[2048];
            if (yan_difftest_report(&diff, &after, &ref_state, step, before.pc,
                                    instruction, line, sizeof line) != 0) {
                if (report != NULL) {
                    fputs(line, report);
                }
                fputs(line, stderr);
            }
            fprintf(stderr,
                    "yan_difftest: MISMATCH at step %" PRIu64
                    " pc = %08" PRIx32 " insn = %08" PRIx32
                    " (pc_next = %08" PRIx32 ", pc_ref_next = %08" PRIx32
                    ", first differing reg = %" PRIu32
                    ", dut = %08" PRIx32 ", ref = %08" PRIx32 ")\n",
                    step, before.pc, instruction, diff.pc_dut, diff.pc_ref,
                    diff.first_differing_reg, diff.reg_dut, diff.reg_ref);
            result = EXIT_MISMATCH;
            goto done;
        }

        if (status != YAN_OK) {
            fprintf(stderr, "yan_difftest: yan_cpu_step failed with %d\n", (int)status);
            result = EXIT_HOST_ERROR;
            goto done;
        }

        if (options.tohost_given) {
            uint32_t value = 0;
            if (yan_bus_read(&bus, options.tohost, 4, &value).status == YAN_OK &&
                value != 0) {
                if (value == 1) {
                    result = EXIT_PASS;
                } else {
                    fprintf(stderr, "yan_difftest: the Guest reported failure code"
                            " %" PRIu32 " after %" PRIu64 " instructions\n",
                            value, step + 1);
                    result = EXIT_GUEST_FAILURE;
                }
                break;
            }
        }
        if (step + 1 == options.max_steps) {
            fprintf(stderr, "yan_difftest: stopped after %" PRIu64
                    " steps without reaching tohost\n", step + 1);
            result = EXIT_NO_TERMINATION;
        }
    }

    if (options.check_memory && result == EXIT_PASS) {
        uint32_t offset = 0, dut_value = 0, ref_value = 0;
        if (!compare_memory(&model, &ram, options.base, &offset, &dut_value, &ref_value)) {
            fprintf(stderr,
                    "yan_difftest: MEMORY MISMATCH at offset %" PRIu32
                    " (address %08" PRIx32 "): dut = %02" PRIx32
                    ", ref = %02" PRIx32 "\n",
                    offset, options.base + offset, dut_value, ref_value);
            result = EXIT_MISMATCH;
        }
    }

done:
    if (trace != NULL) {
        fclose(trace);
    }
    if (report != NULL) {
        fclose(report);
    }
    if (model.handle != NULL) {
        dlclose(model.handle);
    }
    free(image);
    yan_ram_destroy(&ram);
    return result;
}
