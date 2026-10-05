/* CTest fixture: writer and reader are separate ELF images and processes. */
#include "block.h"
#include "guest.h"
#include "platform.h"

#ifndef PERSIST_WRITE
#define PERSIST_WRITE 0
#endif
#ifndef PERSIST_LBA
#define PERSIST_LBA 3
#endif

static uint8_t data[4096];

static void check(int condition, uint32_t code)
{
    if (!condition) {
        const char *message = ":FAIL: persistent block Guest assertion\n";
        while (*message) {
            (void)yan_os_uart_put((uint8_t)*message++);
        }
        guest_finish(UINT32_C(0x80000000) | code);
    }
}

static uint8_t pattern(uint32_t i)
{
    return (uint8_t)((i * 17U + (i >> 8) * 29U + PERSIST_LBA * 41U) & 255U);
}

static void exchange(uint8_t op, uint32_t lba, uint32_t count,
                     uint8_t status, uint32_t expected_length)
{
    YanOsBlockRequest request = {op, 0, 0x1234, lba, count};
    YanOsBlockResponse response = {0};
    uint32_t length = 0;
    check(yan_os_block_submit(&request, data) == 0, 1);
    int result = YAN_OS_BLOCK_AGAIN;
    for (unsigned poll = 0; poll < 10000 && result == YAN_OS_BLOCK_AGAIN; ++poll) {
        result = yan_os_block_take(&response, data, sizeof data, &length);
    }
    check(result == 0, 2);
    check(response.op == op && response.tag == 0x1234 && response.lba == lba, 3);
    check(response.status == status, 4);
    check(response.count == (status == 0 ? count : 0), 5);
    check(length == expected_length, 6);
}

int main(void)
{
    exchange(YAN_OS_BLOCK_OP_CAPACITY, 0, 0, 0, 8);
    check(yan_os_block_count_from_bytes(data) == 8, 7);
#if PERSIST_WRITE
    for (uint32_t i = 0; i < sizeof data; ++i) data[i] = pattern(i);
    exchange(YAN_OS_BLOCK_OP_WRITE, PERSIST_LBA, 1, 0, 0);
#else
    exchange(YAN_OS_BLOCK_OP_READ, PERSIST_LBA, 1, 0, sizeof data);
    for (uint32_t i = 0; i < sizeof data; ++i) check(data[i] == pattern(i), 8);
#endif
    /* Rejected writes must preserve length and all neighbouring blocks. */
    exchange(YAN_OS_BLOCK_OP_WRITE, 8, 1, YAN_OS_BLOCK_STATUS_INVALID, 0);
    exchange(YAN_OS_BLOCK_OP_READ, 8, 1, YAN_OS_BLOCK_STATUS_INVALID, 0);
    guest_finish(1);
    return 0;
}
