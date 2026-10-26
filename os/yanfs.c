/* YanFS core: formatting, mount validation, directory operations and reads.
 *
 * The contract is os/yanfs.h and docs/specs/0021-yanfs.md. Three properties
 * decide whether the rest of the filesystem may trust a device:
 *
 *   detection, not repair  mount reads block 0 into scratch and accepts it
 *   only after every field, the CRC and every allocation invariant holds. A
 *   rejected block is never copied into the metadata cache, so a caller that
 *   ignores the result still cannot observe a partial directory.
 *
 *   fail closed  an I/O or protocol error faults the instance. Every later
 *   file operation and mount then returns FAULTED until unmount clears the
 *   local state; a cache filled from a device the backend already called
 *   failed is never handed out again.
 *
 *   check before writing  every argument, name, range and address-overlap test
 *   runs before the first block reaches the medium. A create that only turns
 *   out to be a duplicate, a full directory or a space problem performs no
 *   callback at all.
 *
 * Little-endian conversion is field by field; no C struct is overlaid on the
 * block, so neither padding nor host endianness can reach the wire format.
 * Every variable-length scan is bounded by the on-disk field width, so a name
 * without a terminator is rejected instead of searched past the entry.
 *
 * Address checks are pure uintptr_t arithmetic, evaluated in the target's own
 * pointer domain. Comparing pointers that do not point into the same object
 * would be undefined, forming an end pointer by adding past the object is
 * undefined as well, and on a 32-bit Guest widening the ends to uint64_t would
 * hide a range that already left the address space. Ranges are therefore built
 * as uintptr values and a range that would pass UINTPTR_MAX is rejected as an
 * overlap. No alias check reads or writes through the suspect pointer: a caller
 * output that overlaps the context is rejected before anything is stored
 * through it. The approved dual-output rule (read out/read_bytes, list
 * info/cursor) uses the same helper before any output write. */
#include "yanfs.h"

#include <stddef.h>

#include "memory.h"
#include "sort.h"

/* 0026 source identity allocator seed. The process-global allocator keeps its
 * last handed-out identity in a file-static uint64 counter that starts from
 * this value, so the first identity is SEED + 1: the production default 0 gives
 * 1, 2, 3, ... and UINT64_MAX is the last cacheable identity, after which the
 * next allocation returns 0 with cacheable=false and no later allocation wraps
 * back to a small value. The near-exhaustion test build overrides the value so
 * one run crosses UINT64_MAX. */
#ifndef YAN_FS_SOURCE_TOKEN_SEED
#define YAN_FS_SOURCE_TOKEN_SEED UINT64_C(0)
#endif

/* The process-global allocator. It starts from the seed above and hands out
 * SEED + 1, SEED + 2, ... as long as the value is below UINT64_MAX. UINT64_MAX
 * itself is still a valid, cacheable identity; the *next* allocation sets the
 * exhausted flag, returns token 0 with cacheable=false, and every later
 * allocation keeps returning 0/false. There is deliberately no reset and no
 * setter: the only way to cross the ceiling is the separate test build that
 * overrides the seed. */
static uint64_t source_serial = YAN_FS_SOURCE_TOKEN_SEED;
static bool source_exhausted = false;

static void source_identity_allocate(YanFs *fs)
{
    if (source_exhausted || source_serial == UINT64_MAX) {
        source_exhausted = true;
        fs->source_token = 0u;
        fs->source_cacheable = false;
        return;
    }
    ++source_serial;
    fs->source_token = source_serial;
    fs->source_cacheable = true;
}

/* Block 0 offsets, from the layout table in 0021. */
#define FS_MAGIC_OFFSET 0u
#define FS_MAGIC_SIZE 8u
#define FS_VERSION_OFFSET 8u
#define FS_BLOCK_SIZE_OFFSET 12u
#define FS_CAPACITY_OFFSET 16u
#define FS_ENTRY_CAPACITY_OFFSET 20u
#define FS_RESERVED_OFFSET 24u
#define FS_RESERVED_END 60u
#define FS_CRC_OFFSET 60u

/* Directory-entry offsets inside the 64-byte entry. */
#define FS_ENTRY_NAME_AREA 32u
#define FS_ENTRY_SIZE_OFFSET 32u
#define FS_ENTRY_START_OFFSET 36u
#define FS_ENTRY_COUNT_OFFSET 40u
#define FS_ENTRY_RESERVED_OFFSET 44u

#define FS_CRC_POLYNOMIAL UINT32_C(0xedb88320)

static uint32_t load_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void store_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

/* CRC over a whole directory with one block optionally substituted. Block 0
 * offsets 60..63 always read as zero, which is where the single stored CRC
 * lives; the other D-1 blocks contribute every byte. */
static uint32_t directory_crc_with_block(const uint8_t *directory,
                                         uint32_t directory_blocks,
                                         uint32_t replaced_block,
                                         const uint8_t *replacement)
{
    uint32_t crc = UINT32_C(0xffffffff);
    for (uint32_t block = 0; block < directory_blocks; ++block) {
        const uint8_t *base = block == replaced_block
                                  ? replacement
                                  : directory + (size_t)block * YAN_FS_BLOCK_SIZE;
        for (uint32_t offset = 0; offset < YAN_FS_BLOCK_SIZE; ++offset) {
            uint32_t byte = base[offset];
            if (block == 0u && offset >= FS_CRC_OFFSET &&
                offset < FS_CRC_OFFSET + 4u) {
                byte = 0;
            }
            crc ^= byte;
            for (uint32_t bit = 0; bit < 8; ++bit) {
                if ((crc & UINT32_C(1)) != 0) {
                    crc = (crc >> 1) ^ FS_CRC_POLYNOMIAL;
                } else {
                    crc >>= 1;
                }
            }
        }
    }
    return crc ^ UINT32_C(0xffffffff);
}

uint32_t yan_fs_directory_crc(const uint8_t *directory,
                              uint32_t directory_blocks)
{
    return directory_crc_with_block(directory, directory_blocks, UINT32_MAX, NULL);
}

uint32_t yan_fs_metadata_crc(const uint8_t block[4096])
{
    return yan_fs_directory_crc(block, 1u);
}

YanFsResult yan_fs_format_directory(uint8_t *out, uint32_t directory_blocks,
                                    uint32_t capacity_blocks)
{
    static const uint8_t magic[FS_MAGIC_SIZE] = {
        (uint8_t)'Y', (uint8_t)'A', (uint8_t)'N', (uint8_t)'F',
        (uint8_t)'S', (uint8_t)'0', (uint8_t)'1', 0u
    };
    /* D=1 keeps 0021's capacity >= 1 rule: a one-block v1 image is still a
     * valid empty file system. A v2 directory needs at least one data block,
     * so D>1 requires capacity_blocks > D. */
    if (out == NULL || directory_blocks < 1u ||
        directory_blocks > YAN_FS_MAX_DIR_BLOCKS ||
        capacity_blocks < directory_blocks ||
        (directory_blocks > 1u && capacity_blocks == directory_blocks)) {
        return YAN_FS_INVALID;
    }
    uint32_t bytes = directory_blocks * YAN_FS_BLOCK_SIZE;
    for (uint32_t i = 0; i < bytes; ++i) {
        out[i] = 0;
    }
    for (uint32_t i = 0; i < FS_MAGIC_SIZE; ++i) {
        out[FS_MAGIC_OFFSET + i] = magic[i];
    }
    store_le32(out + FS_VERSION_OFFSET,
               directory_blocks == 1u ? YAN_FS_VERSION : YAN_FS_VERSION_2);
    store_le32(out + FS_BLOCK_SIZE_OFFSET, YAN_FS_BLOCK_SIZE);
    store_le32(out + FS_CAPACITY_OFFSET, capacity_blocks);
    store_le32(out + FS_ENTRY_CAPACITY_OFFSET,
               directory_blocks * (YAN_FS_BLOCK_SIZE / YAN_FS_ENTRY_SIZE) - 1u);
    if (directory_blocks > 1u) {
        store_le32(out + FS_RESERVED_OFFSET, directory_blocks);
    }
    store_le32(out + FS_CRC_OFFSET,
               yan_fs_directory_crc(out, directory_blocks));
    return YAN_FS_OK;
}

YanFsResult yan_fs_format_metadata(uint8_t out[4096], uint32_t capacity_blocks)
{
    return yan_fs_format_directory(out, 1u, capacity_blocks);
}

/* The allowed set is [A-Za-z0-9._-]; comparison is case sensitive, and the two
 * special names "." and ".." are refused even though their bytes are legal. */
static bool name_char_allowed(uint8_t byte)
{
    return (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z') ||
           (byte >= (uint8_t)'a' && byte <= (uint8_t)'z') ||
           (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') ||
           byte == (uint8_t)'.' || byte == (uint8_t)'_' || byte == (uint8_t)'-';
}

static bool name_bytes_allowed(const uint8_t *name, uint32_t length)
{
    if (length < 1u || length > YAN_FS_NAME_MAX) {
        return false;
    }
    if (length == 1u && name[0] == (uint8_t)'.') {
        return false;
    }
    if (length == 2u && name[0] == (uint8_t)'.' && name[1] == (uint8_t)'.') {
        return false;
    }
    for (uint32_t i = 0; i < length; ++i) {
        if (!name_char_allowed(name[i])) {
            return false;
        }
    }
    return true;
}

/* The stored name always has a terminating zero inside its 32-byte area for a
 * valid entry; the scan is bounded so a malformed one cannot run away. */
static uint32_t entry_name_length(const uint8_t *entry)
{
    uint32_t length = 0;
    while (length < FS_ENTRY_NAME_AREA && entry[length] != 0u) {
        ++length;
    }
    return length;
}

static bool context_overlap(const YanFs *fs, const void *pointer, uint64_t length);
static bool protected_overlap(const YanFs *fs, const void *pointer, uint64_t length);

/* True when the single byte at name[index] may be read. It must not run past the
 * pointer domain and must not overlap the context. The check is per byte on
 * purpose: a legal short name whose terminator sits immediately before the
 * context is still accepted, while a name that starts inside the context is
 * rejected before the first read instead of scanning past the object. */
static bool name_byte_readable(const YanFs *fs, const char *name, uint32_t index)
{
    uintptr_t begin = (uintptr_t)(const void *)name;
    if ((uint64_t)index > (uint64_t)(UINTPTR_MAX - begin)) {
        return false;
    }
    return !protected_overlap(fs, (const void *)(begin + (uintptr_t)index), 1u);
}

/* Validates a caller-supplied name and reports its length. Every byte the scan
 * touches is checked before it is read, and the scan stops after 32 bytes, so a
 * name without a terminator is rejected rather than walked past the entry. */
static bool caller_name_allowed(const YanFs *fs, const char *name, uint32_t *length)
{
    bool span_safe = !protected_overlap(fs, name, FS_ENTRY_NAME_AREA);
    uint32_t size = 0;
    while (size < FS_ENTRY_NAME_AREA) {
        if (!span_safe && !name_byte_readable(fs, name, size)) {
            return false;
        }
        if (name[size] == '\0') {
            break;
        }
        ++size;
    }
    if (size == FS_ENTRY_NAME_AREA) {
        return false;
    }
    if (!name_bytes_allowed((const uint8_t *)name, size)) {
        return false;
    }
    *length = size;
    return true;
}

/* True when the two byte ranges intersect. Everything is kept in the real
 * uintptr_t domain of the target: a range that would run past UINTPTR_MAX is
 * reported as overlapping, because on a 32-bit target widening both ends to
 * uint64_t would let begin + length look fine while the range has already left
 * the address space. The test neither compares unrelated objects' pointers nor
 * forms an end pointer with pointer arithmetic, and it reads nothing through
 * either range. Complexity of the arithmetic is a compare and an add; no
 * variable 64-bit shift is emitted, which matters on a freestanding Guest. */
static bool ranges_overlap(uintptr_t first_begin, uint64_t first_length,
                           uintptr_t second_begin, uint64_t second_length)
{
    if (first_length == 0u || second_length == 0u) {
        return false;
    }
    if (first_length > (uint64_t)(UINTPTR_MAX - first_begin) ||
        second_length > (uint64_t)(UINTPTR_MAX - second_begin)) {
        return true;
    }
    uintptr_t first_end = first_begin + (uintptr_t)first_length;
    uintptr_t second_end = second_begin + (uintptr_t)second_length;
    return first_begin < second_end && second_begin < first_end;
}

/* True when [pointer, pointer + length) intersects the YanFs object. */
static bool context_overlap(const YanFs *fs, const void *pointer, uint64_t length)
{
    uintptr_t context_begin = (uintptr_t)(const void *)fs;
    uintptr_t context_end = context_begin + (uintptr_t)sizeof(YanFs);
    if (context_end < context_begin) {
        context_end = UINTPTR_MAX; /* saturate if the object sits at the top */
    }
    return ranges_overlap(context_begin, (uint64_t)(context_end - context_begin),
                          (uintptr_t)pointer, length);
}

/* 0036: a caller-owned directory is live cache, so every input and output that
 * could alias it is rejected explicitly; it may sit outside the YanFs object. */
static bool directory_overlap(const YanFs *fs, const void *pointer, uint64_t length)
{
    if (fs->directory == NULL || fs->directory_blocks == 0u) {
        return false;
    }
    return ranges_overlap((uintptr_t)(const void *)fs->directory,
                          (uint64_t)fs->directory_blocks * YAN_FS_BLOCK_SIZE,
                          (uintptr_t)pointer, length);
}

static bool protected_overlap(const YanFs *fs, const void *pointer, uint64_t length)
{
    return context_overlap(fs, pointer, length) ||
           directory_overlap(fs, pointer, length) ||
           (fs->name_order != NULL &&
            ranges_overlap((uintptr_t)(const void *)fs->name_order,
                           (uint64_t)fs->entry_capacity * sizeof(uint16_t),
                           (uintptr_t)pointer, length));
}

/* A name is read byte by byte up to its terminator; the bytes actually touched
 * are name[0 .. length] inclusive. */
static bool name_overlaps_protected(const YanFs *fs, const char *name,
                                    uint32_t length)
{
    return protected_overlap(fs, (const void *)name, (uint64_t)length + 1u);
}

static bool entry_is_empty(const uint8_t *entry)
{
    for (uint32_t i = 0; i < YAN_FS_ENTRY_SIZE; ++i) {
        if (entry[i] != 0u) {
            return false;
        }
    }
    return true;
}

/* The directory is one contiguous buffer, so slot s starts at byte 64+64s.
 * Slot 63 is the first entry of directory block 1, and so on. */
static uint32_t entry_offset(uint32_t slot)
{
    return YAN_FS_HEADER_SIZE + slot * YAN_FS_ENTRY_SIZE;
}

static uint32_t entry_block(uint32_t slot)
{
    return entry_offset(slot) / YAN_FS_BLOCK_SIZE;
}

static const uint8_t *entry_at(const uint8_t *block, uint32_t slot)
{
    return block + entry_offset(slot);
}

/* ASCII byte order over stored names, the order the directory index and the
 * service name tables have always used. A name that is a prefix sorts first. */
static int compare_entry_names(const uint8_t *left, const uint8_t *right)
{
    for (uint32_t i = 0u; i < FS_ENTRY_NAME_AREA; ++i) {
        if (left[i] != right[i]) {
            return left[i] < right[i] ? -1 : 1;
        }
        if (left[i] == 0u) {
            return 0;
        }
    }
    return 0;
}

static int compare_entry_to_name(const uint8_t *entry, const char *name,
                                  uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)name;
    for (uint32_t i = 0u; i < length; ++i) {
        if (entry[i] != bytes[i]) {
            return entry[i] < bytes[i] ? -1 : 1;
        }
    }
    return entry[length] == 0u ? 0 : 1;
}

/* Mount sorting sees bare directory pointers, so both comparators recover the
 * slot from the index itself. */
static bool name_order_before(const void *context, uint16_t left, uint16_t right)
{
    const uint8_t *directory = context;
    return compare_entry_names(entry_at(directory, left),
                               entry_at(directory, right)) < 0;
}

static bool extent_order_before(const void *context, uint16_t left, uint16_t right)
{
    const uint8_t *directory = context;
    uint32_t left_start =
        load_le32(entry_at(directory, left) + FS_ENTRY_START_OFFSET);
    uint32_t right_start =
        load_le32(entry_at(directory, right) + FS_ENTRY_START_OFFSET);
    return left_start < right_start;
}

/* Fills name_order with every non-empty slot and leaves it sorted by name.
 * The caller owns the table and sized it to entry_capacity at init. */
static void rebuild_name_order(YanFs *fs)
{
    uint32_t count = 0;
    for (uint32_t slot = 0; slot < fs->entry_capacity; ++slot) {
        if (entry_at(fs->directory, slot)[0] != 0u) {
            fs->name_order[count++] = (uint16_t)slot;
        }
    }
    yan_sort_index(fs->name_order, count, name_order_before, fs->directory);
    fs->name_order_count = count;
}

/* Entry view inside a single scratch block copied from directory block k. */
static uint8_t *entry_in_block(uint8_t *block, uint32_t slot)
{
    return block + entry_offset(slot) % YAN_FS_BLOCK_SIZE;
}

/* Copies the block that holds slot into scratch, ready for one entry edit. */
static void load_entry_block(YanFs *fs, uint32_t slot)
{
    const uint8_t *source =
        fs->directory + (size_t)entry_block(slot) * YAN_FS_BLOCK_SIZE;
    memcpy(fs->scratch, source, YAN_FS_BLOCK_SIZE);
}

/* Checks a whole directory buffer after mount has read all D blocks. The CRC
 * covers every block; extents must start at or after block D, the first data
 * block. 0036-R8 replaces the pairwise scans with two sorted neighbour passes:
 * live extents are ordered by start and live names by name, so an overlap or a
 * duplicate is adjacent. It leaves name_order sorted by name, ready for
 * find_entry; the table is caller-owned and never grows the stack frame. */
static YanFsResult validate_directory(const uint8_t *directory,
                                      uint32_t directory_blocks,
                                      uint32_t entry_capacity,
                                      uint32_t capacity_blocks,
                                      uint16_t *name_order)
{
    if (load_le32(directory + FS_CRC_OFFSET) !=
        yan_fs_directory_crc(directory, directory_blocks)) {
        return YAN_FS_CORRUPT;
    }

    for (uint32_t slot = 0; slot < entry_capacity; ++slot) {
        const uint8_t *entry = entry_at(directory, slot);
        uint32_t length = entry_name_length(entry);
        if (length == FS_ENTRY_NAME_AREA) {
            return YAN_FS_CORRUPT; /* no terminator inside the name area */
        }
        if (length == 0u) {
            /* An unused slot is 64 zero bytes. A zero first name byte with any
             * other byte set is damage, not an empty slot. */
            if (!entry_is_empty(entry)) {
                return YAN_FS_CORRUPT;
            }
            continue;
        }
        if (!name_bytes_allowed(entry, length)) {
            return YAN_FS_CORRUPT;
        }
        for (uint32_t i = length; i < FS_ENTRY_NAME_AREA; ++i) {
            if (entry[i] != 0u) {
                return YAN_FS_CORRUPT; /* padding after the terminator */
            }
        }
        for (uint32_t i = FS_ENTRY_RESERVED_OFFSET; i < YAN_FS_ENTRY_SIZE; ++i) {
            if (entry[i] != 0u) {
                return YAN_FS_CORRUPT;
            }
        }
        uint32_t size_bytes = load_le32(entry + FS_ENTRY_SIZE_OFFSET);
        uint32_t start_block = load_le32(entry + FS_ENTRY_START_OFFSET);
        uint32_t block_count = load_le32(entry + FS_ENTRY_COUNT_OFFSET);
        if (size_bytes == 0u) {
            if (start_block != 0u || block_count != 0u) {
                return YAN_FS_CORRUPT;
            }
            continue; /* an empty file owns no extent */
        }
        uint32_t needed = size_bytes / YAN_FS_BLOCK_SIZE;
        if ((size_bytes % YAN_FS_BLOCK_SIZE) != 0u) {
            ++needed;
        }
        if (block_count != needed) {
            return YAN_FS_CORRUPT;
        }
        /* 0036: an extent starts after the whole directory, not at block 1. */
        if (start_block < directory_blocks || start_block >= capacity_blocks) {
            return YAN_FS_CORRUPT;
        }
        if (block_count > capacity_blocks - start_block) {
            return YAN_FS_CORRUPT;
        }
    }

    /* Extents are disjoint exactly when no start sorts inside an earlier
     * interval. Touching intervals (previous end == next start) are legal. */
    uint32_t ordered = 0u;
    for (uint32_t slot = 0; slot < entry_capacity; ++slot) {
        const uint8_t *entry = entry_at(directory, slot);
        if (entry[0] != 0u &&
            load_le32(entry + FS_ENTRY_SIZE_OFFSET) != 0u) {
            name_order[ordered++] = (uint16_t)slot;
        }
    }
    yan_sort_index(name_order, ordered, extent_order_before, directory);
    for (uint32_t i = 1u; i < ordered; ++i) {
        const uint8_t *previous = entry_at(directory, name_order[i - 1u]);
        const uint8_t *current = entry_at(directory, name_order[i]);
        uint32_t previous_end =
            load_le32(previous + FS_ENTRY_START_OFFSET) +
            load_le32(previous + FS_ENTRY_COUNT_OFFSET);
        if (load_le32(current + FS_ENTRY_START_OFFSET) < previous_end) {
            return YAN_FS_CORRUPT; /* extents must not overlap */
        }
    }

    /* Reuse the same table for names and leave it sorted for find_entry. */
    ordered = 0u;
    for (uint32_t slot = 0; slot < entry_capacity; ++slot) {
        if (entry_at(directory, slot)[0] != 0u) {
            name_order[ordered++] = (uint16_t)slot;
        }
    }
    yan_sort_index(name_order, ordered, name_order_before, directory);
    for (uint32_t i = 1u; i < ordered; ++i) {
        if (compare_entry_names(entry_at(directory, name_order[i - 1u]),
                                entry_at(directory, name_order[i])) == 0) {
            return YAN_FS_CORRUPT; /* names are unique */
        }
    }
    return YAN_FS_OK;
}

/* Data blocks a logical length occupies. The ceiling is computed without
 * forming length + 4095, so UINT32_MAX rounds up to 1048576 and never wraps. */
static uint32_t blocks_needed(uint32_t length)
{
    uint32_t count = length / YAN_FS_BLOCK_SIZE;
    if ((length % YAN_FS_BLOCK_SIZE) != 0u) {
        ++count;
    }
    return count;
}

/* Borrow the name table only inside the synchronous busy window. Restore it
 * on every outcome, before any device callback can observe the filesystem. */
static bool allocate_extent(YanFs *fs, uint32_t needed, uint32_t *start_out)
{
    uint32_t count = 0u;
    for (uint32_t slot = 0; slot < fs->entry_capacity; ++slot) {
        const uint8_t *entry = entry_at(fs->directory, slot);
        if (entry[0] != 0u && load_le32(entry + FS_ENTRY_SIZE_OFFSET) != 0u) {
            fs->name_order[count++] = (uint16_t)slot;
        }
    }
    yan_sort_index(fs->name_order, count, extent_order_before, fs->directory);
    uint32_t candidate = fs->directory_blocks;
    for (uint32_t i = 0u; i < count; ++i) {
        const uint8_t *entry = entry_at(fs->directory, fs->name_order[i]);
        uint32_t start = load_le32(entry + FS_ENTRY_START_OFFSET);
        if (start - candidate >= needed) {
            break;
        }
        candidate = start + load_le32(entry + FS_ENTRY_COUNT_OFFSET);
    }
    bool found = needed <= fs->capacity_blocks - candidate;
    rebuild_name_order(fs);
    if (found) {
        *start_out = candidate;
    }
    return found;
}

static bool find_entry(const YanFs *fs, const char *name, uint32_t length,
                       uint32_t *slot_out)
{
    uint32_t low = 0u;
    uint32_t high = fs->name_order_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        const uint8_t *entry = entry_at(fs->directory,
                                        fs->name_order[middle]);
        int order = compare_entry_to_name(entry, name, length);
        if (order < 0) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    if (low < fs->name_order_count) {
        uint32_t slot = fs->name_order[low];
        if (compare_entry_to_name(entry_at(fs->directory, slot), name,
                                  length) == 0) {
            *slot_out = slot;
            return true;
        }
    }
    return false;
}

static bool find_free_slot(const YanFs *fs, uint32_t *slot_out)
{
    for (uint32_t slot = 0; slot < fs->entry_capacity; ++slot) {
        if (entry_is_empty(entry_at(fs->directory, slot))) {
            *slot_out = slot;
            return true;
        }
    }
    return false;
}

/* Writes one directory entry into a block image. name points at either the
 * caller's name or an entry already in the metadata cache; both are kept out of
 * the destination, so the copy never overlaps. */
static void entry_set(uint8_t *block, uint32_t slot, const uint8_t *name,
                      uint32_t length, uint32_t size_bytes, uint32_t start_block,
                      uint32_t block_count)
{
    uint8_t *entry = entry_in_block(block, slot);
    memset(entry, 0, YAN_FS_ENTRY_SIZE);
    memcpy(entry, name, length);
    store_le32(entry + FS_ENTRY_SIZE_OFFSET, size_bytes);
    store_le32(entry + FS_ENTRY_START_OFFSET, start_block);
    store_le32(entry + FS_ENTRY_COUNT_OFFSET, block_count);
}

static void entry_clear(uint8_t *block, uint32_t slot)
{
    uint8_t *entry = entry_in_block(block, slot);
    memset(entry, 0, YAN_FS_ENTRY_SIZE);
}

static YanFsResult fault_io(YanFs *fs, YanFsIoResult io)
{
    fs->state = YAN_FS_STATE_FAULTED;
    /* 0026: really entering FAULTED is a source change, so it gets a fresh
     * identity. A rejected mount (CORRUPT/UNSUPPORTED) never reaches here. */
    source_identity_allocate(fs);
    return io == YAN_FS_IO_ERROR ? YAN_FS_IO : YAN_FS_PROTOCOL;
}

/* Writes length bytes to the contiguous extent at start_block, one whole block
 * at a time. The last block is zero-filled past the logical end. The caller has
 * already checked the inputs, so this is the first point where the medium is
 * touched. */
static YanFsIoResult write_payload(YanFs *fs, const uint8_t *bytes,
                                   uint32_t length, uint32_t start_block)
{
    uint32_t blocks = blocks_needed(length);
    for (uint32_t i = 0; i < blocks; ++i) {
        uint32_t offset = i * YAN_FS_BLOCK_SIZE;
        uint32_t remaining = length - offset;
        uint32_t chunk = remaining < YAN_FS_BLOCK_SIZE ? remaining
                                                       : YAN_FS_BLOCK_SIZE;
        memcpy(fs->scratch, bytes + offset, chunk);
        if (chunk < YAN_FS_BLOCK_SIZE) {
            memset(fs->scratch + chunk, 0, YAN_FS_BLOCK_SIZE - chunk);
        }
        YanFsIoResult io =
            fs->io.write_block(fs->io.context, start_block + i, fs->scratch);
        if (io != YAN_FS_IO_OK) {
            return io;
        }
    }
    return YAN_FS_IO_OK;
}

/* Zeroes the whole live directory cache. A rejected mount and every init or
 * unmount use this, so no stale entry survives a failed publication. */
static void clear_directory(YanFs *fs)
{
    if (fs->directory == NULL) {
        return;
    }
    uint32_t bytes = fs->directory_blocks * YAN_FS_BLOCK_SIZE;
    memset(fs->directory, 0, bytes);
}

/* Publishes the edited scratch block. Block 0 needs one write, exactly as v1
 * always did. A slot after block 0 first becomes durable, then the CRC block is
 * rewritten; a failed write faults the instance and never allocates a new
 * source identity. The cache is updated only after each write succeeds. */
static YanFsResult commit_entry(YanFs *fs, uint32_t slot)
{
    uint32_t block = entry_block(slot);
    YanFsResult result = YAN_FS_OK;
    if (block == 0u) {
        store_le32(fs->scratch + FS_CRC_OFFSET,
                   directory_crc_with_block(fs->directory,
                                            fs->directory_blocks, 0u,
                                            fs->scratch));
        YanFsIoResult io = fs->io.write_block(fs->io.context, 0, fs->scratch);
        if (io != YAN_FS_IO_OK) {
            result = fault_io(fs, io);
        } else {
            memcpy(fs->directory, fs->scratch, YAN_FS_BLOCK_SIZE);
        }
    } else {
        YanFsIoResult io =
            fs->io.write_block(fs->io.context, block, fs->scratch);
        if (io != YAN_FS_IO_OK) {
            result = fault_io(fs, io);
        } else {
            uint8_t *cache = fs->directory + (size_t)block * YAN_FS_BLOCK_SIZE;
            memcpy(cache, fs->scratch, YAN_FS_BLOCK_SIZE);
            memcpy(fs->scratch, fs->directory, YAN_FS_BLOCK_SIZE);
            store_le32(fs->scratch + FS_CRC_OFFSET,
                       directory_crc_with_block(fs->directory,
                                                fs->directory_blocks, 0u,
                                                fs->scratch));
            io = fs->io.write_block(fs->io.context, 0, fs->scratch);
            if (io != YAN_FS_IO_OK) {
                result = fault_io(fs, io);
            } else {
                memcpy(fs->directory, fs->scratch, YAN_FS_BLOCK_SIZE);
            }
        }
    }
    /* 0036-R8: find_entry follows the published cache, so the index is rebuilt
     * after every commit, including a failed one whose entry-block write may
     * already be reflected in the cache. */
    rebuild_name_order(fs);
    if (result != YAN_FS_OK) {
        return result;
    }
    /* 0026: a successfully published directory is a source change. Every
     * create/replace/remove/rename/copy reaches this single publish point. */
    source_identity_allocate(fs);
    return YAN_FS_OK;
}

YanFsResult yan_fs_init_directory(YanFs *fs, YanFsBlockIo io,
                                  uint8_t *directory,
                                  uint32_t directory_blocks,
                                  uint16_t *name_order)
{
    if (fs == NULL) {
        return YAN_FS_INVALID;
    }
    if (fs->initialized) {
        /* Re-init may clear an idle, unmounted instance, but not one that is
         * mid-operation and not the record of a mount or a fault. */
        if (fs->busy) {
            return YAN_FS_BUSY;
        }
        if (fs->state != YAN_FS_UNMOUNTED) {
            return YAN_FS_INVALID;
        }
    }
    if (io.capacity == NULL || io.read_block == NULL || io.write_block == NULL) {
        return YAN_FS_INVALID;
    }
    if (directory == NULL || directory_blocks < 1u ||
        directory_blocks > YAN_FS_MAX_DIR_BLOCKS || name_order == NULL) {
        return YAN_FS_INVALID;
    }
    if (context_overlap(fs, directory,
                        (uint64_t)directory_blocks * YAN_FS_BLOCK_SIZE) ||
        context_overlap(fs, name_order,
                        (uint64_t)(directory_blocks *
                                   (YAN_FS_BLOCK_SIZE / YAN_FS_ENTRY_SIZE) - 1u) *
                            sizeof(uint16_t)) ||
        ranges_overlap((uintptr_t)(const void *)directory,
                       (uint64_t)directory_blocks * YAN_FS_BLOCK_SIZE,
                       (uintptr_t)(const void *)name_order,
                       (uint64_t)(directory_blocks *
                                  (YAN_FS_BLOCK_SIZE / YAN_FS_ENTRY_SIZE) - 1u) *
                           sizeof(uint16_t))) {
        return YAN_FS_INVALID;
    }
    fs->io = io;
    fs->initialized = true;
    fs->state = YAN_FS_UNMOUNTED;
    fs->busy = false;
    fs->capacity_blocks = 0;
    fs->directory = directory;
    fs->directory_blocks = directory_blocks;
    fs->entry_capacity =
        directory_blocks * (YAN_FS_BLOCK_SIZE / YAN_FS_ENTRY_SIZE) - 1u;
    fs->name_order = name_order;
    fs->name_order_count = 0u;
    /* 0026: a successful init is a source change and takes a fresh identity. */
    source_identity_allocate(fs);
    memset(fs->scratch, 0, YAN_FS_BLOCK_SIZE);
    clear_directory(fs);
    return YAN_FS_OK;
}

/* 0036 issue #72: one set of rules for the directory header. Probe and mount
 * both call this, so magic, version, D and N cannot drift apart. Version 1
 * always means D=1 and N=63; version 2 reads D from the reserved word and
 * requires 1..64 and N=64*D-1. Everything else is CORRUPT except an unknown
 * version, which stays UNSUPPORTED. */
static YanFsResult validate_header(const uint8_t header[YAN_FS_BLOCK_SIZE],
                                   uint32_t *version_out,
                                   uint32_t *directory_blocks_out)
{
    static const uint8_t magic[FS_MAGIC_SIZE] = {
        (uint8_t)'Y', (uint8_t)'A', (uint8_t)'N', (uint8_t)'F',
        (uint8_t)'S', (uint8_t)'0', (uint8_t)'1', 0u
    };
    for (uint32_t i = 0; i < FS_MAGIC_SIZE; ++i) {
        if (header[FS_MAGIC_OFFSET + i] != magic[i]) {
            return YAN_FS_CORRUPT;
        }
    }
    uint32_t version = load_le32(header + FS_VERSION_OFFSET);
    uint32_t blocks = 1u;
    if (version == YAN_FS_VERSION) {
        blocks = 1u;
    } else if (version == YAN_FS_VERSION_2) {
        blocks = load_le32(header + FS_RESERVED_OFFSET);
        if (blocks < 1u || blocks > YAN_FS_MAX_DIR_BLOCKS) {
            return YAN_FS_CORRUPT;
        }
    } else {
        return YAN_FS_UNSUPPORTED;
    }
    if (load_le32(header + FS_ENTRY_CAPACITY_OFFSET) !=
        blocks * (YAN_FS_BLOCK_SIZE / YAN_FS_ENTRY_SIZE) - 1u) {
        return YAN_FS_CORRUPT;
    }
    *version_out = version;
    *directory_blocks_out = blocks;
    return YAN_FS_OK;
}

YanFsResult yan_fs_probe(const YanFsBlockIo *io, uint8_t scratch[4096],
                         uint32_t *directory_blocks)
{
    if (io == NULL || io->read_block == NULL || scratch == NULL ||
        directory_blocks == NULL) {
        return YAN_FS_INVALID;
    }
    YanFsIoResult io_result = io->read_block(io->context, 0u, scratch);
    if (io_result != YAN_FS_IO_OK) {
        return io_result == YAN_FS_IO_ERROR ? YAN_FS_IO : YAN_FS_PROTOCOL;
    }
    uint32_t version = 0u;
    uint32_t blocks = 0u;
    YanFsResult result = validate_header(scratch, &version, &blocks);
    if (result != YAN_FS_OK) {
        return result;
    }
    *directory_blocks = blocks;
    return YAN_FS_OK;
}

YanFsResult yan_fs_mount(YanFs *fs)
{
    if (fs == NULL) {
        return YAN_FS_INVALID;
    }
    if (!fs->initialized) {
        return YAN_FS_INVALID;
    }
    if (fs->busy) {
        return YAN_FS_BUSY;
    }
    if (fs->state == YAN_FS_STATE_FAULTED) {
        return YAN_FS_FAULTED;
    }
    if (fs->state == YAN_FS_MOUNTED) {
        return YAN_FS_INVALID; /* no implicit unmount */
    }

    /* busy spans the whole operation, including every callback, so a reentrant
     * call from inside one of them sees BUSY rather than a half-filled cache. */
    fs->busy = true;
    clear_directory(fs);
    fs->name_order_count = 0u;
    YanFsResult result = YAN_FS_CORRUPT;
    uint64_t device_blocks = 0;
    YanFsIoResult io_result = fs->io.capacity(fs->io.context, &device_blocks);
    if (io_result != YAN_FS_IO_OK) {
        /* Unknown callback values are protocol violations, not data damage. */
        result = io_result == YAN_FS_IO_ERROR ? YAN_FS_IO : YAN_FS_PROTOCOL;
        fs->state = YAN_FS_STATE_FAULTED;
        source_identity_allocate(fs);
    } else if (device_blocks < 1u || device_blocks > (uint64_t)UINT32_MAX) {
        result = YAN_FS_UNSUPPORTED;
    } else {
        uint32_t capacity_blocks = (uint32_t)device_blocks;
        io_result = fs->io.read_block(fs->io.context, 0, fs->scratch);
        if (io_result != YAN_FS_IO_OK) {
            result = io_result == YAN_FS_IO_ERROR ? YAN_FS_IO : YAN_FS_PROTOCOL;
            fs->state = YAN_FS_STATE_FAULTED;
            source_identity_allocate(fs);
        } else {
            uint32_t version = 0u;
            uint32_t image_blocks = 0u;
            uint32_t image_entries = 0u;
            result = validate_header(fs->scratch, &version, &image_blocks);
            if (result != YAN_FS_OK) {
                goto done;
            }
            image_entries =
                image_blocks * (YAN_FS_BLOCK_SIZE / YAN_FS_ENTRY_SIZE) - 1u;
            if (load_le32(fs->scratch + FS_BLOCK_SIZE_OFFSET) !=
                YAN_FS_BLOCK_SIZE) {
                result = YAN_FS_CORRUPT;
                goto done;
            }
            if (load_le32(fs->scratch + FS_CAPACITY_OFFSET) != capacity_blocks) {
                result = YAN_FS_CORRUPT;
                goto done;
            }
            if (version == YAN_FS_VERSION) {
                for (uint32_t i = FS_RESERVED_OFFSET; i < FS_RESERVED_END; ++i) {
                    if (fs->scratch[i] != 0u) {
                        result = YAN_FS_CORRUPT;
                        goto done;
                    }
                }
            } else {
                for (uint32_t i = FS_RESERVED_OFFSET + 4u; i < FS_RESERVED_END;
                     ++i) {
                    if (fs->scratch[i] != 0u) {
                        result = YAN_FS_CORRUPT;
                        goto done;
                    }
                }
                if (capacity_blocks <= image_blocks) {
                    result = YAN_FS_CORRUPT;
                    goto done;
                }
            }
            if (image_blocks != fs->directory_blocks) {
                /* 0036-R3's rule arrives early: the mount capacity is fixed at
                 * init time, so a different D is not a usable image here. */
                result = YAN_FS_INVALID;
                goto done;
            }
            memcpy(fs->directory, fs->scratch, YAN_FS_BLOCK_SIZE);
            for (uint32_t block = 1u; block < image_blocks; ++block) {
                uint8_t *out = fs->directory +
                               (size_t)block * YAN_FS_BLOCK_SIZE;
                io_result = fs->io.read_block(fs->io.context, block, out);
                if (io_result != YAN_FS_IO_OK) {
                    result = io_result == YAN_FS_IO_ERROR ? YAN_FS_IO
                                                          : YAN_FS_PROTOCOL;
                    fs->state = YAN_FS_STATE_FAULTED;
                    source_identity_allocate(fs);
                    goto done;
                }
            }
            result = validate_directory(fs->directory, image_blocks,
                                        image_entries, capacity_blocks,
                                        fs->name_order);
            if (result == YAN_FS_OK) {
                /* validate_directory leaves name_order sorted but reports only
                 * success; counting the live names here is a single O(N) pass,
                 * cheaper than sorting a second time. */
                uint32_t live = 0u;
                for (uint32_t slot = 0; slot < image_entries; ++slot) {
                    if (entry_at(fs->directory, slot)[0] != 0u) {
                        ++live;
                    }
                }
                fs->name_order_count = live;
                fs->capacity_blocks = capacity_blocks;
                fs->state = YAN_FS_MOUNTED;
                source_identity_allocate(fs);
            }
        }
    }
done:
    if (result != YAN_FS_OK) {
        clear_directory(fs);
        fs->name_order_count = 0u;
    }
    fs->busy = false;
    return result;
}

YanFsResult yan_fs_unmount(YanFs *fs)
{
    if (fs == NULL) {
        return YAN_FS_INVALID;
    }
    if (!fs->initialized) {
        return YAN_FS_INVALID;
    }
    if (fs->busy) {
        return YAN_FS_BUSY;
    }
    /* Allowed from UNMOUNTED, MOUNTED and FAULTED, and it does no I/O: it only
     * drops the local view. io and initialized survive. */
    fs->state = YAN_FS_UNMOUNTED;
    fs->capacity_blocks = 0;
    fs->name_order_count = 0u;
    clear_directory(fs);
    memset(fs->scratch, 0, YAN_FS_BLOCK_SIZE);
    /* 0026: every successful unmount, including a repeated one, is a source
     * change and takes a fresh identity. */
    source_identity_allocate(fs);
    return YAN_FS_OK;
}

/* 0026 pure observation of the in-memory source identity. The order is fixed by
 * the spec: context + initialized, then BUSY, then the output holder's range,
 * overflow and context alias. It is deliberately not operation_guard: UNMOUNTED
 * and FAULTED are valid observations, not errors, and no path here touches the
 * device, allocates a token or changes any field. The range check reuses
 * context_overlap, so a holder that would run past UINTPTR_MAX is rejected in
 * the target's own pointer domain and *out is never written on an error. */
YanFsResult yan_fs_source(const YanFs *fs, YanFsSource *out)
{
    if (fs == NULL) {
        return YAN_FS_INVALID;
    }
    if (!fs->initialized) {
        return YAN_FS_INVALID;
    }
    if (fs->busy) {
        return YAN_FS_BUSY;
    }
    if (out == NULL) {
        return YAN_FS_INVALID;
    }
    if (protected_overlap(fs, out, (uint64_t)sizeof(YanFsSource))) {
        return YAN_FS_INVALID;
    }
    out->state = fs->state;
    out->token = fs->source_token;
    out->cacheable = fs->source_cacheable && !source_exhausted;
    return YAN_FS_OK;
}

/* The shared prefix of the file-facing operations: context, then busy, then
 * the fault record, then the mount state. */
static YanFsResult operation_guard(const YanFs *fs)
{
    if (fs == NULL) {
        return YAN_FS_INVALID;
    }
    if (!fs->initialized) {
        return YAN_FS_INVALID;
    }
    if (fs->busy) {
        return YAN_FS_BUSY;
    }
    if (fs->state == YAN_FS_STATE_FAULTED) {
        return YAN_FS_FAULTED;
    }
    if (fs->state != YAN_FS_MOUNTED) {
        return YAN_FS_NOT_MOUNTED;
    }
    return YAN_FS_OK;
}

/* Inputs and allocation are checked here, before a single callback. busy is
 * already set by the public wrapper, so a callback that reenters sees BUSY. */
static YanFsResult create_locked(YanFs *fs, const char *name,
                                 const uint8_t *bytes, uint32_t length)
{
    uint32_t name_length = 0;
    if (name == NULL || !caller_name_allowed(fs, name, &name_length) ||
        name_overlaps_protected(fs, name, name_length)) {
        return YAN_FS_INVALID;
    }
    if (length > 0u &&
        (bytes == NULL || protected_overlap(fs, bytes, (uint64_t)length))) {
        return YAN_FS_INVALID;
    }
    uint32_t existing = 0;
    if (find_entry(fs, name, name_length, &existing)) {
        return YAN_FS_EXISTS;
    }
    uint32_t slot = 0;
    if (!find_free_slot(fs, &slot)) {
        return YAN_FS_DIRECTORY_FULL;
    }
    uint32_t needed = blocks_needed(length);
    uint32_t start_block = 0;
    if (needed > 0u && !allocate_extent(fs, needed, &start_block)) {
        return YAN_FS_NOSPACE;
    }

    if (needed > 0u) {
        YanFsIoResult io = write_payload(fs, bytes, length, start_block);
        if (io != YAN_FS_IO_OK) {
            return fault_io(fs, io);
        }
    }
    load_entry_block(fs, slot);
    entry_set(fs->scratch, slot, (const uint8_t *)name, name_length, length,
              start_block, needed);
    return commit_entry(fs, slot);
}

YanFsResult yan_fs_create(YanFs *fs, const char *name, const uint8_t *bytes,
                          uint32_t length)
{
    YanFsResult state = operation_guard(fs);
    if (state != YAN_FS_OK) {
        return state;
    }
    fs->busy = true;
    YanFsResult result = create_locked(fs, name, bytes, length);
    fs->busy = false;
    return result;
}

static YanFsResult replace_locked(YanFs *fs, const char *name,
                                  const uint8_t *bytes, uint32_t length)
{
    uint32_t name_length = 0;
    if (name == NULL || !caller_name_allowed(fs, name, &name_length) ||
        name_overlaps_protected(fs, name, name_length)) {
        return YAN_FS_INVALID;
    }
    if (length > 0u &&
        (bytes == NULL || protected_overlap(fs, bytes, (uint64_t)length))) {
        return YAN_FS_INVALID;
    }
    uint32_t slot = 0;
    if (!find_entry(fs, name, name_length, &slot)) {
        return YAN_FS_NOT_FOUND;
    }
    const uint8_t *previous = entry_at(fs->directory, slot);
    uint32_t previous_name_length = entry_name_length(previous);
    uint32_t needed = blocks_needed(length);
    uint32_t start_block = 0;
    /* The old extent is part of the current directory, so it is in the in-use
     * set and cannot be borrowed: the new data needs its own room. */
    if (needed > 0u && !allocate_extent(fs, needed, &start_block)) {
        return YAN_FS_NOSPACE;
    }

    if (needed > 0u) {
        YanFsIoResult io = write_payload(fs, bytes, length, start_block);
        if (io != YAN_FS_IO_OK) {
            return fault_io(fs, io);
        }
    }
    load_entry_block(fs, slot);
    /* The slot and the stored name survive; only size and extent change. */
    entry_set(fs->scratch, slot, previous, previous_name_length, length,
              start_block, needed);
    return commit_entry(fs, slot);
}

YanFsResult yan_fs_replace(YanFs *fs, const char *name, const uint8_t *bytes,
                           uint32_t length)
{
    YanFsResult state = operation_guard(fs);
    if (state != YAN_FS_OK) {
        return state;
    }
    fs->busy = true;
    YanFsResult result = replace_locked(fs, name, bytes, length);
    fs->busy = false;
    return result;
}

static YanFsResult remove_locked(YanFs *fs, const char *name)
{
    uint32_t name_length = 0;
    if (name == NULL || !caller_name_allowed(fs, name, &name_length) ||
        name_overlaps_protected(fs, name, name_length)) {
        return YAN_FS_INVALID;
    }
    uint32_t slot = 0;
    if (!find_entry(fs, name, name_length, &slot)) {
        return YAN_FS_NOT_FOUND;
    }
    load_entry_block(fs, slot);
    entry_clear(fs->scratch, slot);
    /* One metadata write is the whole operation; the data blocks are left as
     * they are and simply stop being referenced. */
    return commit_entry(fs, slot);
}

YanFsResult yan_fs_remove(YanFs *fs, const char *name)
{
    YanFsResult state = operation_guard(fs);
    if (state != YAN_FS_OK) {
        return state;
    }
    fs->busy = true;
    YanFsResult result = remove_locked(fs, name);
    fs->busy = false;
    return result;
}

YanFsResult yan_fs_read(YanFs *fs, const char *name, uint32_t offset,
                        uint8_t *out, uint32_t length, uint32_t *read_bytes)
{
    if (fs == NULL) {
        return YAN_FS_INVALID;
    }
    /* 0021 file-API order: context/initialized -> busy -> FAULTED -> MOUNTED ->
     * this operation's other parameters. The state guard therefore runs before
     * the count pointer is judged, so BUSY, FAULTED and NOT_MOUNTED win over an
     * invalid read_bytes. On a state error a valid, non-context count is cleared
     * to zero, while an invalid one (NULL, a context alias, or a range leaving
     * uintptr) is never written and the original state is returned. */
    YanFsResult state = operation_guard(fs);
    const bool count_writable =
        read_bytes != NULL &&
        !protected_overlap(fs, read_bytes, (uint64_t)sizeof(uint32_t));
    if (state != YAN_FS_OK) {
        if (count_writable) {
            *read_bytes = 0u;
        }
        return state;
    }
    /* Healthy instance: an invalid count pointer is now an ordinary parameter
     * error and is not written. */
    if (!count_writable) {
        return YAN_FS_INVALID;
    }

    /* 0021 forbids context aliases and the read out/read_bytes overlap, but a
     * caller may legally share writable memory between name and read_bytes. The
     * name is therefore parsed and copied into a bounded 32-byte buffer before
     * the count is cleared; clearing first could destroy a name that has not
     * been looked up yet. The copy also covers name/out sharing: the lookup
     * uses the copy, so writing file bytes into out cannot disturb it. */
    char name_copy[YAN_FS_NAME_MAX + 1u];
    uint32_t name_length = 0;
    bool name_valid = false;
    if (name != NULL) {
        name_valid = caller_name_allowed(fs, name, &name_length) &&
                     !name_overlaps_protected(fs, name, name_length);
        if (name_valid) {
            for (uint32_t i = 0; i < name_length; ++i) {
                name_copy[i] = name[i];
            }
            name_copy[name_length] = '\0';
        }
    }
    *read_bytes = 0u;
    if (!name_valid) {
        return YAN_FS_INVALID;
    }

    if (length > 0u) {
        if (out == NULL || protected_overlap(fs, out, (uint64_t)length)) {
            return YAN_FS_INVALID;
        }
        /* Approved 2026-10-03 supplement: the file output range and the
         * four-byte read_bytes must not overlap. read_bytes was already cleared
         * on this parameter error, so overlapping out bytes may have changed;
         * no file content is returned. */
        if (ranges_overlap((uintptr_t)out, (uint64_t)length,
                           (uintptr_t)read_bytes, (uint64_t)sizeof(uint32_t))) {
            return YAN_FS_INVALID;
        }
    }

    uint32_t slot = 0;
    if (!find_entry(fs, name_copy, name_length, &slot)) {
        return YAN_FS_NOT_FOUND;
    }
    const uint8_t *entry = entry_at(fs->directory, slot);
    uint32_t size_bytes = load_le32(entry + FS_ENTRY_SIZE_OFFSET);
    if (offset >= size_bytes || length == 0u) {
        return YAN_FS_OK;
    }
    uint32_t remaining = size_bytes - offset;
    uint32_t want = length < remaining ? length : remaining;
    uint32_t start_block = load_le32(entry + FS_ENTRY_START_OFFSET);
    uint32_t within = offset % YAN_FS_BLOCK_SIZE;
    uint32_t block_index = offset / YAN_FS_BLOCK_SIZE;

    fs->busy = true;
    uint32_t done = 0u;
    while (done < want) {
        uint32_t chunk = YAN_FS_BLOCK_SIZE - within;
        if (chunk > want - done) {
            chunk = want - done;
        }
        YanFsIoResult io = fs->io.read_block(fs->io.context,
                                             start_block + block_index,
                                             fs->scratch);
        if (io != YAN_FS_IO_OK) {
            /* Only whole blocks already confirmed reach earlier parts of out;
             * the failing block's bytes are not copied anywhere. */
            fs->busy = false;
            *read_bytes = done;
            return fault_io(fs, io);
        }
        memcpy(out + done, fs->scratch + within, chunk);
        done += chunk;
        within = 0u;
        ++block_index;
    }
    fs->busy = false;
    *read_bytes = done;
    return YAN_FS_OK;
}

YanFsResult yan_fs_list(YanFs *fs, uint32_t *cursor, YanFsInfo *out)
{
    YanFsResult state = operation_guard(fs);
    if (state != YAN_FS_OK) {
        return state;
    }
    if (cursor == NULL || out == NULL) {
        return YAN_FS_INVALID;
    }
    /* The alias test comes before *cursor is read and long before it is
     * written; arithmetic on the addresses is the only thing that happens. */
    if (protected_overlap(fs, cursor, (uint64_t)sizeof(uint32_t)) ||
        protected_overlap(fs, out, (uint64_t)sizeof(YanFsInfo))) {
        return YAN_FS_INVALID;
    }
    /* Approved 2026-10-03 supplement: the whole YanFsInfo output and the
     * four-byte cursor must not overlap. This is checked before *cursor is read
     * and before either output is written, so both stay untouched. */
    if (ranges_overlap((uintptr_t)cursor, (uint64_t)sizeof(uint32_t),
                       (uintptr_t)out, (uint64_t)sizeof(YanFsInfo))) {
        return YAN_FS_INVALID;
    }
    uint32_t slot = *cursor;
    if (slot > fs->entry_capacity) {
        return YAN_FS_INVALID;
    }
    while (slot < fs->entry_capacity) {
        const uint8_t *entry = entry_at(fs->directory, slot);
        if (entry[0] != 0u) {
            memcpy(out->name, entry, FS_ENTRY_NAME_AREA);
            out->size_bytes = load_le32(entry + FS_ENTRY_SIZE_OFFSET);
            *cursor = slot + 1u;
            return YAN_FS_OK;
        }
        ++slot;
    }
    /* Reaching the end advances the cursor to its final value and leaves the
     * output untouched. */
    *cursor = fs->entry_capacity;
    return YAN_FS_END;
}

YanFsResult yan_fs_stat(YanFs *fs, const char *name, YanFsInfo *out)
{
    YanFsResult state = operation_guard(fs);
    if (state != YAN_FS_OK) {
        return state;
    }
    if (name == NULL || out == NULL) {
        return YAN_FS_INVALID;
    }
    uint32_t name_length = 0;
    if (!caller_name_allowed(fs, name, &name_length) ||
        name_overlaps_protected(fs, name, name_length)) {
        return YAN_FS_INVALID;
    }
    if (protected_overlap(fs, out, (uint64_t)sizeof(YanFsInfo))) {
        return YAN_FS_INVALID;
    }
    uint32_t slot = 0;
    if (!find_entry(fs, name, name_length, &slot)) {
        return YAN_FS_NOT_FOUND;
    }
    const uint8_t *entry = entry_at(fs->directory, slot);
    memcpy(out->name, entry, FS_ENTRY_NAME_AREA);
    out->size_bytes = load_le32(entry + FS_ENTRY_SIZE_OFFSET);
    return YAN_FS_OK;
}

/* 0024 rename: move the stored name of an existing file inside its own
 * physical slot. Both caller names are validated before the source is looked
 * up, so a missing source with an invalid companion name is INVALID rather
 * than NOT_FOUND. size, start, count, every other slot and every data block
 * survive; only the name field and the block CRC change. A self rename returns
 * before the first callback, and the only callback of a real rename is the
 * single block-0 write inside commit_entry. */
static YanFsResult rename_locked(YanFs *fs, const char *old_name,
                                 const char *new_name)
{
    uint32_t old_length = 0;
    uint32_t new_length = 0;
    if (old_name == NULL ||
        !caller_name_allowed(fs, old_name, &old_length) ||
        name_overlaps_protected(fs, old_name, old_length) ||
        new_name == NULL ||
        !caller_name_allowed(fs, new_name, &new_length) ||
        name_overlaps_protected(fs, new_name, new_length)) {
        return YAN_FS_INVALID;
    }

    uint32_t slot = 0;
    if (!find_entry(fs, old_name, old_length, &slot)) {
        return YAN_FS_NOT_FOUND;
    }
    uint32_t target = 0;
    if (find_entry(fs, new_name, new_length, &target)) {
        if (target == slot) {
            return YAN_FS_OK; /* the same name is a no-op, not a write */
        }
        return YAN_FS_EXISTS;
    }

    /* Read the surviving fields from the cache before scratch is reused. */
    const uint8_t *entry = entry_at(fs->directory, slot);
    uint32_t size_bytes = load_le32(entry + FS_ENTRY_SIZE_OFFSET);
    uint32_t start_block = load_le32(entry + FS_ENTRY_START_OFFSET);
    uint32_t block_count = load_le32(entry + FS_ENTRY_COUNT_OFFSET);

    load_entry_block(fs, slot);
    entry_set(fs->scratch, slot, (const uint8_t *)new_name, new_length,
              size_bytes, start_block, block_count);
    return commit_entry(fs, slot);
}

/* 0024 copy: keep the source slot and its extent, and build an independent file
 * in the first empty slot on the lowest contiguous free extent. The source
 * description comes from the metadata cache before scratch is used; every
 * source block is read into scratch and written to its target before the next
 * read; only the bytes past the logical end of the final block are cleared.
 * commit_entry is reached only after every data write succeeded, so the
 * cache is published only once block 0 has been written. An empty source skips
 * the data loop entirely. */
static YanFsResult copy_locked(YanFs *fs, const char *source_name,
                               const char *destination_name)
{
    uint32_t source_length = 0;
    uint32_t destination_length = 0;
    if (source_name == NULL ||
        !caller_name_allowed(fs, source_name, &source_length) ||
        name_overlaps_protected(fs, source_name, source_length) ||
        destination_name == NULL ||
        !caller_name_allowed(fs, destination_name, &destination_length) ||
        name_overlaps_protected(fs, destination_name, destination_length)) {
        return YAN_FS_INVALID;
    }

    uint32_t source_slot = 0;
    if (!find_entry(fs, source_name, source_length, &source_slot)) {
        return YAN_FS_NOT_FOUND;
    }
    uint32_t destination_slot = 0;
    if (find_entry(fs, destination_name, destination_length, &destination_slot)) {
        return YAN_FS_EXISTS; /* the same name, or any existing file */
    }

    /* Capture the source description from the cache before the data loop
     * overwrites scratch on every block. block_count is the exact extent a
     * mounted, validated directory recorded for this size. */
    const uint8_t *source_entry = entry_at(fs->directory, source_slot);
    uint32_t size_bytes = load_le32(source_entry + FS_ENTRY_SIZE_OFFSET);
    uint32_t start_block = load_le32(source_entry + FS_ENTRY_START_OFFSET);
    uint32_t block_count = load_le32(source_entry + FS_ENTRY_COUNT_OFFSET);

    uint32_t slot = 0;
    if (!find_free_slot(fs, &slot)) {
        return YAN_FS_DIRECTORY_FULL;
    }
    uint32_t destination_start = 0;
    if (block_count > 0u &&
        !allocate_extent(fs, block_count, &destination_start)) {
        return YAN_FS_NOSPACE;
    }

    /* Index/count arithmetic, not length + 4095 and not an accumulating
     * done += 4096: block_index stays below the validated block_count, whose
     * ceiling is 1048576 for UINT32_MAX, so every product fits in uint32_t. */
    for (uint32_t block_index = 0; block_index < block_count; ++block_index) {
        uint32_t offset = block_index * YAN_FS_BLOCK_SIZE;
        uint32_t remaining = size_bytes - offset;
        uint32_t chunk = remaining < YAN_FS_BLOCK_SIZE ? remaining
                                                       : YAN_FS_BLOCK_SIZE;
        YanFsIoResult io = fs->io.read_block(fs->io.context,
                                             start_block + block_index,
                                             fs->scratch);
        if (io != YAN_FS_IO_OK) {
            return fault_io(fs, io);
        }
        /* Only the final logical tail is cleared; a full final block is data. */
        for (uint32_t j = chunk; j < YAN_FS_BLOCK_SIZE; ++j) {
            fs->scratch[j] = 0u;
        }
        io = fs->io.write_block(fs->io.context,
                                destination_start + block_index, fs->scratch);
        if (io != YAN_FS_IO_OK) {
            return fault_io(fs, io);
        }
    }

    load_entry_block(fs, slot);
    entry_set(fs->scratch, slot, (const uint8_t *)destination_name,
              destination_length, size_bytes, destination_start, block_count);
    return commit_entry(fs, slot);
}

/* Both public entry points hold busy across the whole call, so a device
 * callback that reenters either primitive (or any other file operation) sees
 * BUSY, and busy is cleared on every exit path. */
YanFsResult yan_fs_rename(YanFs *fs, const char *old_name, const char *new_name)
{
    YanFsResult state = operation_guard(fs);
    if (state != YAN_FS_OK) {
        return state;
    }
    fs->busy = true;
    YanFsResult result = rename_locked(fs, old_name, new_name);
    fs->busy = false;
    return result;
}

YanFsResult yan_fs_copy(YanFs *fs, const char *source_name,
                        const char *destination_name)
{
    YanFsResult state = operation_guard(fs);
    if (state != YAN_FS_OK) {
        return state;
    }
    fs->busy = true;
    YanFsResult result = copy_locked(fs, source_name, destination_name);
    fs->busy = false;
    return result;
}

#ifdef YAN_FS_TEST_HOOKS
/* 0036-R8-AC2: the production lookup and the index it follows, exposed only to
 * the oracle suite that compiles this source with YAN_FS_TEST_HOOKS. */
bool yan_fs_test_find_entry(const YanFs *fs, const char *name, uint32_t length,
                            uint32_t *slot_out)
{
    return find_entry(fs, name, length, slot_out);
}

uint32_t yan_fs_test_name_order_count(const YanFs *fs)
{
    return fs->name_order_count;
}

uint32_t yan_fs_test_name_order_at(const YanFs *fs, uint32_t index)
{
    return fs->name_order[index];
}
#endif
