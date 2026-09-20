/* Random RV32IM program generator for differential testing.
 *
 * Every emitted program is safe by construction: it touches only registers and
 * a fixed scratch buffer, keeps all accesses aligned, branches only forward
 * inside the same basic block, and finishes by writing to `tohost`. YanCPU and
 * a reference model can therefore execute it on identical instruction
 * boundaries. The Host rand() is deliberately not used: the same seed must
 * produce the same program everywhere.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SCRATCH_BYTES = 256, MAX_OPS = 4096 };

typedef struct {
    uint64_t state;
} Random;

static uint32_t next_random(Random *random)
{
    uint64_t value = random->state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    random->state = value;
    return (uint32_t)((value * UINT64_C(2685821657736338717)) >> 32);
}

static uint32_t below(Random *random, uint32_t bound)
{
    return next_random(random) % bound;
}

/* x8 holds the scratch base and x2 is the stack pointer; both stay reserved. */
static const unsigned pool[] = {5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17,
                                18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                                29, 30, 31};
enum { POOL_SIZE = (int)(sizeof pool / sizeof pool[0]) };

static const char *const register_type_operations[] = {
    "add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and",
    "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"
};

static const char *const immediate_operations[] = {
    "addi", "slti", "sltiu", "xori", "ori", "andi"
};

static const char *const shift_operations[] = {"slli", "srli", "srai"};
static const char *const branch_operations[] = {"beq", "bne", "blt", "bge",
                                                "bltu", "bgeu"};

static unsigned pick(Random *random)
{
    return pool[below(random, POOL_SIZE)];
}

static void emit_register_operation(Random *random)
{
    const char *operation =
        register_type_operations[below(random, (uint32_t)(sizeof register_type_operations /
                                                          sizeof register_type_operations[0]))];
    printf("  %s x%u, x%u, x%u\n", operation, pick(random), pick(random),
           pick(random));
}

static void emit_immediate_operation(Random *random)
{
    const int immediate = (int)below(random, 4096) - 2048;
    const char *operation =
        immediate_operations[below(random, (uint32_t)(sizeof immediate_operations /
                                                      sizeof immediate_operations[0]))];
    printf("  %s x%u, x%u, %d\n", operation, pick(random), pick(random), immediate);
}

static void emit_shift_operation(Random *random)
{
    const char *operation = shift_operations[below(random, 3)];
    printf("  %s x%u, x%u, %u\n", operation, pick(random), pick(random),
           below(random, 32));
}

static void emit_upper_immediate(Random *random)
{
    printf("  %s x%u, %u\n", below(random, 2) == 0 ? "lui" : "auipc",
           pick(random), below(random, 1u << 20));
}

static void emit_memory_operation(Random *random, unsigned *labels)
{
    static const char *const loads[] = {"lb", "lh", "lw", "lbu", "lhu"};
    static const char *const stores[] = {"sb", "sh", "sw"};
    static const unsigned widths[] = {1, 2, 4, 1, 2};
    const uint32_t choice = below(random, 8);
    if (choice < 5) {
        const unsigned width = widths[choice];
        const unsigned offset = below(random, SCRATCH_BYTES / width) * width;
        printf("  %s x%u, %u(x8)\n", loads[choice], pick(random), offset);
    } else {
        const unsigned index = choice - 5;
        const unsigned width = (unsigned)(1u << index);
        const unsigned offset = below(random, SCRATCH_BYTES / width) * width;
        printf("  %s x%u, %u(x8)\n", stores[index], pick(random), offset);
    }
    (void)labels;
}

/* A forward branch skips a short, still valid, instruction window. */
static void emit_branch(Random *random, unsigned *label)
{
    const char *operation = branch_operations[below(random, 6)];
    const unsigned skip = 1 + below(random, 3);
    const unsigned index = (*label)++;
    printf("  %s x%u, x%u, .Lskip%u\n", operation, pick(random), pick(random),
           index);
    for (unsigned at = 0; at < skip; ++at) {
        emit_register_operation(random);
    }
    if (below(random, 4) == 0) {
        printf("  jal x1, .Lskip%u\n", index);
        for (unsigned at = 0; at < 1 + below(random, 2); ++at) {
            emit_register_operation(random);
        }
    }
    if (below(random, 6) == 0) {
        printf("  la x6, .Lskip%u\n  jalr x1, x6, 0\n", index);
        emit_register_operation(random);
    }
    printf(".Lskip%u:\n", index);
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s [--seed N] [--ops N] [--output FILE]\n", program);
}

int main(int argc, char **argv)
{
    uint64_t seed = 1;
    uint64_t operations = 128;
    const char *output = NULL;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--seed") == 0 && index + 1 < argc) {
            seed = strtoull(argv[++index], NULL, 0);
        } else if (strcmp(argv[index], "--ops") == 0 && index + 1 < argc) {
            operations = strtoull(argv[++index], NULL, 0);
        } else if (strcmp(argv[index], "--output") == 0 && index + 1 < argc) {
            output = argv[++index];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (operations == 0 || operations > MAX_OPS) {
        usage(argv[0]);
        return 2;
    }
    if (output != NULL && freopen(output, "w", stdout) == NULL) {
        fprintf(stderr, "yan_gen: cannot write '%s'\n", output);
        return 2;
    }

    Random random = {.state = seed * UINT64_C(6364136223846793005) + 1442695040888963407};
    if (random.state == 0) {
        random.state = UINT64_C(0x9e3779b97f4a7c15);
    }

    printf("# generated by yan_gen --seed %" PRIu64 " --ops %" PRIu64 "\n",
           seed, operations);
    printf("  .section .text.start, \"ax\"\n  .globl _start\n_start:\n");
    printf("  la x8, scratch\n");
    for (unsigned index = 0; index < POOL_SIZE; ++index) {
        printf("  li x%u, 0x%08" PRIx32 "\n", pool[index], next_random(&random));
    }

    unsigned labels = 0;
    for (uint64_t at = 0; at < operations; ++at) {
        switch (below(&random, 10)) {
        case 0:
        case 1:
        case 2:
            emit_register_operation(&random);
            break;
        case 3:
        case 4:
            emit_immediate_operation(&random);
            break;
        case 5:
            emit_shift_operation(&random);
            break;
        case 6:
            emit_upper_immediate(&random);
            break;
        case 7:
        case 8:
            emit_memory_operation(&random, &labels);
            break;
        default:
            emit_branch(&random, &labels);
            break;
        }
    }

    printf("  la x5, tohost\n  li x6, 1\n  sw x6, 0(x5)\n.Lend:\n  j .Lend\n");
    /* The signature region is what a reference model and the DUT compare, so
     * the generated program exposes the same symbols a test suite uses. */
    printf("  .section .bss\n  .align 4\n"
           "  .globl begin_signature\nbegin_signature:\nscratch:\n  .space %u\n"
           "  .align 4\n  .globl end_signature\nend_signature:\n", SCRATCH_BYTES);
    printf("  .section .tohost, \"aw\"\n  .align 2\n  .globl tohost\ntohost:\n"
           "  .word 0\n");
    return 0;
}
