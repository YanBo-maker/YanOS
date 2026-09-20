#include "yan/image.h"

#include <string.h>

enum {
    ELF32_HEADER_SIZE = 52,
    ELF32_PROGRAM_SIZE = 32,
    ELF32_SECTION_SIZE = 40,
    ELF32_SYMBOL_SIZE = 16,
    ELF32_PROGRAM_LOAD = 1,
    ELF32_SECTION_SYMTAB = 2,
    ELF32_MACHINE_RISCV = 243,
    ELF32_CLASS_32 = 1,
    ELF32_DATA_LITTLE_ENDIAN = 1,
    ELF32_VERSION_CURRENT = 1,
    ELF32_SYMBOL_UNDEFINED = 0
};

/* Images are Host-independent data: every field is assembled from bytes, so
 * struct padding and Host endianness never take part in decoding. */
static int range_fits(size_t size, size_t offset, size_t length)
{
    return offset <= size && length <= size - offset;
}

static int read_u16(const uint8_t *data, size_t size, size_t offset,
                    uint32_t *value)
{
    if (!range_fits(size, offset, 2)) {
        return 0;
    }
    *value = (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8);
    return 1;
}

static int read_u32(const uint8_t *data, size_t size, size_t offset,
                    uint32_t *value)
{
    if (!range_fits(size, offset, 4)) {
        return 0;
    }
    *value = (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) |
             ((uint32_t)data[offset + 2] << 16) |
             ((uint32_t)data[offset + 3] << 24);
    return 1;
}

static int header_is_supported(const uint8_t *data, size_t size,
                               uint32_t *entry, uint32_t *phoff, uint32_t *phnum)
{
    static const uint8_t magic[4] = {0x7f, 'E', 'L', 'F'};
    if (size < ELF32_HEADER_SIZE || memcmp(data, magic, sizeof magic) != 0) {
        return 0;
    }
    if (data[4] != ELF32_CLASS_32 || data[5] != ELF32_DATA_LITTLE_ENDIAN) {
        return 0;
    }
    uint32_t version = 0, machine = 0, phentsize = 0;
    if (!read_u32(data, size, 20, &version) || version != ELF32_VERSION_CURRENT ||
        !read_u16(data, size, 18, &machine) || machine != ELF32_MACHINE_RISCV) {
        return 0;
    }
    if (!read_u32(data, size, 24, entry) || !read_u32(data, size, 28, phoff) ||
        !read_u16(data, size, 42, &phentsize) || phentsize != ELF32_PROGRAM_SIZE ||
        !read_u16(data, size, 44, phnum)) {
        return 0;
    }
    return range_fits(size, *phoff, (size_t)*phnum * ELF32_PROGRAM_SIZE);
}

static int read_program(const uint8_t *data, size_t size, uint32_t phoff,
                        uint32_t index, uint32_t *type, uint32_t *offset,
                        uint32_t *paddr, uint32_t *filesz, uint32_t *memsz)
{
    const size_t at = (size_t)phoff + (size_t)index * ELF32_PROGRAM_SIZE;
    return read_u32(data, size, at + 0, type) &&
           read_u32(data, size, at + 4, offset) &&
           read_u32(data, size, at + 12, paddr) &&
           read_u32(data, size, at + 16, filesz) &&
           read_u32(data, size, at + 20, memsz);
}

/* Validates every loadable segment against RAM before writing any of them. */
static YanStatus plan_segments(const YanRam *ram, uint32_t base,
                               const uint8_t *data, size_t size, uint32_t phoff,
                               uint32_t phnum)
{
    for (uint32_t index = 0; index < phnum; ++index) {
        uint32_t type = 0, offset = 0, paddr = 0, filesz = 0, memsz = 0;
        if (!read_program(data, size, phoff, index, &type, &offset, &paddr,
                          &filesz, &memsz)) {
            return YAN_INVALID_IMAGE;
        }
        if (type != ELF32_PROGRAM_LOAD) {
            continue;
        }
        if (!range_fits(size, offset, filesz) || filesz > memsz) {
            return YAN_INVALID_IMAGE;
        }
        if (paddr < base) {
            return YAN_OUT_OF_BOUNDS;
        }
        /* Subtract base first so a segment above 4 GiB cannot wrap. */
        const uint64_t start = (uint64_t)paddr - base;
        if (start + memsz > ram->size) {
            return YAN_OUT_OF_BOUNDS;
        }
    }
    return YAN_OK;
}

static void copy_segments(YanRam *ram, uint32_t base, const uint8_t *data,
                          size_t size, uint32_t phoff, uint32_t phnum)
{
    for (uint32_t index = 0; index < phnum; ++index) {
        uint32_t type = 0, offset = 0, paddr = 0, filesz = 0, memsz = 0;
        if (!read_program(data, size, phoff, index, &type, &offset, &paddr,
                          &filesz, &memsz) || type != ELF32_PROGRAM_LOAD) {
            continue;
        }
        const size_t start = (size_t)(paddr - base);
        memcpy(ram->data + start, data + offset, filesz);
        /* Zero the tail the file does not carry, which is .bss. */
        if (memsz > filesz) {
            memset(ram->data + start + filesz, 0, memsz - filesz);
        }
    }
}

YanStatus yan_image_load_elf(YanRam *ram, uint32_t base, const uint8_t *data,
                             size_t size, YanImageInfo *info)
{
    if (ram == NULL || data == NULL || info == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (ram->data == NULL || ram->size == 0) {
        return YAN_INVALID_STATE;
    }
    uint32_t entry = 0, phoff = 0, phnum = 0;
    if (!header_is_supported(data, size, &entry, &phoff, &phnum)) {
        return YAN_INVALID_IMAGE;
    }
    YanStatus status = plan_segments(ram, base, data, size, phoff, phnum);
    if (status != YAN_OK) {
        return status;
    }
    copy_segments(ram, base, data, size, phoff, phnum);
    info->entry = entry;
    return YAN_OK;
}

static int name_matches(const uint8_t *data, size_t size, size_t names_offset,
                        size_t names_size, uint32_t name_offset, const char *name)
{
    const size_t length = strlen(name) + 1;
    if (name_offset >= names_size || length > names_size - name_offset) {
        return 0;
    }
    const size_t at = names_offset + name_offset;
    return range_fits(size, at, length) &&
           memcmp(data + at, name, length) == 0;
}

/* Symbol lookup walks .symtab so a validation harness can find `tohost`
 * without hard coding an address: test linkers place it anywhere. */
YanStatus yan_image_find_symbol(const uint8_t *data, size_t size,
                                const char *name, uint32_t *address)
{
    if (data == NULL || name == NULL || address == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    uint32_t entry = 0, phoff = 0, phnum = 0;
    if (!header_is_supported(data, size, &entry, &phoff, &phnum)) {
        return YAN_INVALID_IMAGE;
    }
    uint32_t shoff = 0, shentsize = 0, shnum = 0;
    if (!read_u32(data, size, 32, &shoff) ||
        !read_u16(data, size, 46, &shentsize) || shentsize != ELF32_SECTION_SIZE ||
        !read_u16(data, size, 48, &shnum) ||
        !range_fits(size, shoff, (size_t)shnum * ELF32_SECTION_SIZE)) {
        return YAN_INVALID_IMAGE;
    }
    for (uint32_t index = 0; index < shnum; ++index) {
        const size_t at = (size_t)shoff + (size_t)index * ELF32_SECTION_SIZE;
        uint32_t type = 0, offset = 0, length = 0, link = 0, entsize = 0;
        if (!read_u32(data, size, at + 4, &type)) {
            return YAN_INVALID_IMAGE;
        }
        if (type != ELF32_SECTION_SYMTAB) {
            continue;
        }
        if (!read_u32(data, size, at + 16, &offset) ||
            !read_u32(data, size, at + 20, &length) ||
            !read_u32(data, size, at + 24, &link) ||
            !read_u32(data, size, at + 36, &entsize) ||
            entsize != ELF32_SYMBOL_SIZE || link >= shnum ||
            !range_fits(size, offset, length)) {
            return YAN_INVALID_IMAGE;
        }
        const size_t names_at = (size_t)shoff + (size_t)link * ELF32_SECTION_SIZE;
        uint32_t names_offset = 0, names_size = 0;
        if (!read_u32(data, size, names_at + 16, &names_offset) ||
            !read_u32(data, size, names_at + 20, &names_size)) {
            return YAN_INVALID_IMAGE;
        }
        for (size_t symbol = offset;
             symbol + ELF32_SYMBOL_SIZE <= (size_t)offset + length;
             symbol += ELF32_SYMBOL_SIZE) {
            uint32_t name_offset = 0, value = 0, section = 0;
            if (!read_u32(data, size, symbol, &name_offset) ||
                !read_u32(data, size, symbol + 4, &value) ||
                !read_u16(data, size, symbol + 14, &section)) {
                return YAN_INVALID_IMAGE;
            }
            if (section == ELF32_SYMBOL_UNDEFINED ||
                !name_matches(data, size, names_offset, names_size, name_offset,
                              name)) {
                continue;
            }
            *address = value;
            return YAN_OK;
        }
    }
    return YAN_UNMAPPED;
}
