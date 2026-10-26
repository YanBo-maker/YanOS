#include "platform.h"
#include "block_peek.h"
#include <stdio.h>
#include <string.h>

/* Exercise the production volatile copy helpers without touching MMIO.
 * The oracle uses byte indexing modulo the ring size, independently of the
 * production contiguous-span and word-copy loops. */
_Alignas(uint32_t) static uint8_t ring[8208], expected[8208], input[8208];
_Alignas(uint32_t) static uint8_t output[8208], wanted[8208];

int main(void)
{
    /* Pure header peek exercises the same normalized-copy helper used by the
     * device callers, without consuming a byte or changing ring storage. */
    for (uint32_t alignment=0u;alignment<4u;++alignment)
    for (uint32_t tail=0u;tail<64u;++tail) {
        for (uint32_t i=0u;i<sizeof ring;++i) ring[i]=expected[i]=(uint8_t)(i*19u);
        memset(output,0xa5,sizeof output);memset(wanted,0xa5,sizeof wanted);
        for (uint32_t i=0u;i<16u;++i) wanted[alignment+i]=ring[(tail+i)%64u];
        yan_block_peek_copy(ring,64u,tail+3u*64u,output+alignment);
        if (memcmp(ring,expected,sizeof ring)!=0 || memcmp(output,wanted,sizeof output)!=0) {
            fprintf(stderr,"FAIL 0040 header peek mask/wrap/alignment/guards\n");return 1;
        }
    }
    const uint32_t sizes[] = {64u, 128u, 8192u};
    const uint32_t lengths[] = {0u, 1u, 2u, 3u, 4u, 7u, 15u, 16u, 17u,
        31u, 32u, 33u, 63u, 64u, 65u, 4095u, 4096u, 8191u};
    uint32_t cases = 0u;
    for (uint32_t s = 0u; s < 3u; ++s) {
        uint32_t size = sizes[s];
        for (uint32_t r = 0u; r < 4u; ++r)
        for (uint32_t a = 0u; a < 4u; ++a)
        for (uint32_t p = 0u; p < 64u; ++p) {
            uint32_t pos = p < 32u ? p : size - 64u + p;
            for (uint32_t n = 0u; n < sizeof lengths / sizeof lengths[0]; ++n) {
                uint32_t count = lengths[n];
                if (count >= size) continue;
                for (uint32_t i = 0u; i < sizeof ring; ++i) {
                    ring[i] = expected[i] = (uint8_t)(i * 13u + 7u);
                    input[i] = (uint8_t)(i * 29u + 19u);
                    output[i] = wanted[i] = 0xA5u;
                }
                for (uint32_t i = 0u; i < count; ++i)
                    expected[r + (pos + i) % size] = input[a + i];
                yan_os_ring_copy_in(ring + r, size, pos, input + a, count);
                if (memcmp(ring, expected, sizeof ring) != 0) {
                    fprintf(stderr, "FAIL in size=%u pos=%u n=%u r=%u a=%u\n",
                        size, pos, count, r, a); return 1;
                }
                for (uint32_t i = 0u; i < count; ++i)
                    wanted[a + i] = ring[r + (pos + i) % size];
                yan_os_ring_copy_out(ring + r, size, pos, output + a, count);
                if (memcmp(output, wanted, sizeof output) != 0) {
                    fprintf(stderr, "FAIL out size=%u pos=%u n=%u r=%u a=%u\n",
                        size, pos, count, r, a); return 1;
                }
                ++cases;
            }
        }
    }
    printf("PASS ring copies: %u cases, both directions, wrap/alignment/guards\n", cases);
    return 0;
}
