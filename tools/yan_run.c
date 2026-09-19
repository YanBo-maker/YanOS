#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yan/cpu.h"

enum { ELF_PT_LOAD = 1, ELF_MACHINE_RISCV = 243 };

typedef struct {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf32Header;

typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} Elf32Program;

typedef struct {
    const char *image;
    const char *trace;
    const char *signature;
    uint32_t base;
    uint32_t ram_size;
    uint64_t max_steps;
    uint32_t tohost;
    uint32_t signature_start;
    uint32_t signature_end;
} Options;

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s --image FILE [--base ADDR] [--ram BYTES] "
            "[--max-steps N] [--tohost ADDR] [--trace FILE] "
            "[--signature FILE START END]\n", program);
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
        } else if (strcmp(argv[i], "--tohost") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return 0;
            options->tohost = (uint32_t)value;
        } else return 0;
    }
    return options->image != NULL && options->ram_size != 0;
}

static int load_elf(const Options *options, YanRam *ram, uint32_t *entry)
{
    FILE *file = fopen(options->image, "rb");
    if (file == NULL) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 0; }
    unsigned char *data = malloc((size_t)length);
    if (data == NULL || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data); fclose(file); return 0;
    }
    fclose(file);
    if ((size_t)length < sizeof(Elf32Header)) { free(data); return 0; }
    const Elf32Header *header = (const Elf32Header *)data;
    if (memcmp(header->ident, "\177ELF", 4) != 0 || header->ident[4] != 1 ||
        header->ident[5] != 1 || header->machine != ELF_MACHINE_RISCV ||
        header->phentsize != sizeof(Elf32Program)) { free(data); return 0; }
    if ((uint64_t)header->phoff + (uint64_t)header->phnum * header->phentsize > (uint64_t)length) {
        free(data); return 0;
    }
    for (uint16_t i = 0; i < header->phnum; ++i) {
        const Elf32Program *segment = (const Elf32Program *)(data + header->phoff +
            (uint32_t)i * header->phentsize);
        if (segment->type != ELF_PT_LOAD || segment->filesz > segment->memsz ||
            (uint64_t)segment->offset + segment->filesz > (uint64_t)length ||
            segment->paddr < options->base ||
            (uint64_t)(segment->paddr - options->base) + segment->memsz > ram->size) {
            if (segment->type == ELF_PT_LOAD) { free(data); return 0; }
            continue;
        }
        size_t offset = segment->paddr - options->base;
        memcpy(ram->data + offset, data + segment->offset, segment->filesz);
    }
    *entry = header->entry;
    free(data);
    return 1;
}

static int dump_signature(const Options *options, const YanRam *ram)
{
    if (options->signature == NULL) return 1;
    if (options->signature_start < options->base || options->signature_end < options->signature_start ||
        (uint64_t)(options->signature_end - options->base) > ram->size ||
        options->signature_end - options->base > ram->size - (options->signature_start - options->base)) return 0;
    FILE *file = fopen(options->signature, "wb");
    if (file == NULL) return 0;
    size_t offset = options->signature_start - options->base;
    size_t length = options->signature_end - options->signature_start;
    int result = fwrite(ram->data + offset, 1, length, file) == length;
    fclose(file);
    return result;
}

int main(int argc, char **argv)
{
    Options options;
    if (!parse_options(argc, argv, &options)) { usage(argv[0]); return 2; }
    YanRam ram = {0};
    YanBus bus = {0};
    YanCpu cpu = {0};
    uint32_t entry = 0;
    int result = 1;
    FILE *trace = NULL;
    if (yan_ram_init(&ram, options.ram_size) != YAN_OK ||
        yan_bus_init(&bus, &ram, options.base) != YAN_OK ||
        !load_elf(&options, &ram, &entry) || yan_cpu_reset(&cpu, entry) != YAN_OK) goto done;
    if (options.trace != NULL) trace = fopen(options.trace, "w");
    if (options.trace != NULL && trace == NULL) goto done;
    for (uint64_t step = 0; step < options.max_steps; ++step) {
        uint32_t instruction = 0;
        YanCpuState before = {0}, after = {0};
        if (yan_cpu_snapshot(&cpu, &before) != YAN_OK || yan_cpu_fetch(&cpu, &bus, &instruction).status != YAN_OK) break;
        YanStatus status = yan_cpu_step(&cpu, &bus);
        (void)yan_cpu_snapshot(&cpu, &after);
        if (trace != NULL) {
            fprintf(trace, "{\"step\":%" PRIu64 ",\"pc\":%" PRIu32 ",\"insn\":%" PRIu32 ",\"next_pc\":%" PRIu32 ",\"status\":%d,\"regs\":[",
                    step, before.pc, instruction, after.pc, status);
            for (size_t i = 0; i < YAN_REGISTER_COUNT; ++i) {
                fprintf(trace, "%s%" PRIu32, i == 0 ? "" : ",", after.regs[i]);
            }
            fprintf(trace, "],\"mstatus\":%" PRIu32 ",\"mepc\":%" PRIu32
                    ",\"mcause\":%" PRIu32 ",\"mtval\":%" PRIu32 "}\n",
                    after.csr.mstatus, after.csr.mepc, after.csr.mcause, after.csr.mtval);
        }
        if (options.tohost != 0) {
            uint32_t value = 0;
            if (yan_bus_read(&bus, options.tohost, 4, &value).status == YAN_OK && value != 0) {
                result = value == 1 ? 0 : 1;
                goto done;
            }
        }
    }
    result = dump_signature(&options, &ram) ? 1 : 2;
done:
    if (trace != NULL) fclose(trace);
    yan_ram_destroy(&ram);
    return result;
}
