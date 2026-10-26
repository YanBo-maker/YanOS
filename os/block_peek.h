#ifndef YAN_OS_BLOCK_PEEK_H
#define YAN_OS_BLOCK_PEEK_H

#include "platform.h"

/* Call only after observing a complete header. Copying is read-only: the
 * existing pop remains the sole writer of the consumer position. */
static inline void yan_block_peek_copy(const volatile uint8_t *ring,
                                       uint32_t size, uint32_t tail,
                                       uint8_t header[16])
{
    yan_os_ring_copy_out(ring, size, tail & (size - 1u), header, 16u);
}

static inline void yan_block_peek_header(uint8_t header[16])
{
    const uint32_t size = yan_os_transport_ring_size();
    const volatile uint8_t *ring = (const volatile uint8_t *)(uintptr_t)
        (yan_os_transport_ring_base() + size);
    const uint32_t tail = yan_os_transport_h2g_tail();
    yan_block_peek_copy(ring, size, tail, header);
}

#endif
