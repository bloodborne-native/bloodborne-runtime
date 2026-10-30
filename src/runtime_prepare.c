/* The game's executable prepared for the loader, in memory (runtime_prepare.h): the port of
 * scripts/prepare.py's boot.bin, checked byte for byte against it (tools/bb_prepare.c). Every
 * offset is checked before use. */
#include "runtime_prepare.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The executable's image this port runs (scripts/game_check.py's SUPPORTED_IMAGE). */
static const char SUPPORTED_IMAGE[] = "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";
/* The NID of _init_env (SHA-1 of the name and the PS4's salt; scripts/prepare.py's nid()). */
static const char INIT_ENV_NID[] = "bzQExy189ZI";

enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_SCE_DYNLIBDATA = 0x61000000, PT_SCE_RELRO = 0x61000010 };
enum { MAX_IMAGE = 512u << 20 };

/* ---- SHA-256 (FIPS 180-4) ---- */

typedef struct { uint32_t h[8]; uint64_t length; uint8_t block[64]; size_t used; } Sha256;

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(Sha256 *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + SHA256_K[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

void bb_sha256_hex(const void *data, size_t size, char out[65]) {
    Sha256 s = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}, 0};
    const uint8_t *p = data;
    s.length = (uint64_t)size * 8;
    for (; size >= 64; p += 64, size -= 64) sha256_block(&s, p);
    uint8_t last[128] = {0};
    memcpy(last, p, size);
    last[size] = 0x80;
    const size_t blocks = size < 56 ? 1 : 2;
    for (int i = 0; i < 8; ++i) last[blocks * 64 - 1 - i] = (uint8_t)(s.length >> (8 * i));
    for (size_t i = 0; i < blocks; ++i) sha256_block(&s, last + 64 * i);
    for (int i = 0; i < 8; ++i) snprintf(out + 8 * i, 9, "%08x", s.h[i]);
}

/* ---- reading ---- */

typedef struct { const uint8_t *data; size_t size; } Bytes;

static int fail(char *error, size_t error_size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
    return -1;
}

/* Bounds: [offset, offset + size) inside `bytes`. */
static int within(Bytes bytes, uint64_t offset, uint64_t size) {
    return offset <= bytes.size && size <= bytes.size - offset;
}
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }

static int read_file(const char *path, Bytes *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t *data = NULL;
    size_t size = 0, capacity = 0;
    /* A regular file in one allocation of its size: grown by doubling, the 90 MB executable
     * needed a 128 MB block while still holding the 64 MB one, and inside the launcher's iOS app
     * the address space had no such block left. */
    struct stat info;
    if (!fstat(fileno(f), &info) && S_ISREG(info.st_mode) && info.st_size > 0) {
        capacity = (size_t)info.st_size + 1; /* +1: the read that finds the end */
        data = malloc(capacity);
        if (!data) { fclose(f); return -1; }
    }
    for (;;) {
        if (size == capacity) {
            capacity = capacity ? capacity * 2 : 1u << 20;
            uint8_t *grown = realloc(data, capacity);
            if (!grown) { free(data); fclose(f); return -1; }
            data = grown;
        }
        const size_t got = fread(data + size, 1, capacity - size, f);
        size += got;
        if (got == 0) break;
    }
    fclose(f);
    out->data = data, out->size = size;
    return 0;
}

typedef struct { uint32_t type, flags; uint64_t offset, vaddr, filesz, memsz, align; } Phdr;
typedef struct { uint8_t *elf; size_t elf_size; uint64_t entry; Phdr ph[256]; int count; } Elf;

static int read_phdrs(Bytes data, uint64_t base, Elf *out, char *error, size_t error_size) {
    if (!within(data, base, 64)) return fail(error, error_size, "out of bounds: the ELF header");
    const uint8_t *h = data.data + base;
    if (memcmp(h, "\x7f" "ELF\x02\x01\x01", 7) || u16(h + 18) != 62)
        return fail(error, error_size, "expected little-endian x86-64 ELF");
    const uint64_t phoff = u64(h + 32);
    const unsigned phentsize = u16(h + 54), phnum = u16(h + 56);
    if (phentsize != 56 || phnum == 0 || phnum >= 256) return fail(error, error_size, "unsupported program headers");
    if (!within(data, base + phoff, (uint64_t)phnum * 56)) return fail(error, error_size, "out of bounds: program headers");
    out->entry = u64(h + 24);
    out->count = (int)phnum;
    for (unsigned i = 0; i < phnum; ++i) {
        const uint8_t *p = data.data + base + phoff + i * 56;
        out->ph[i] = (Phdr){u32(p), u32(p + 4), u64(p + 8), u64(p + 16), u64(p + 32), u64(p + 40), u64(p + 48)};
    }
    return 0;
}

/* scripts/prepare.py's parse_self: a plaintext SELF (or a plain ELF) as the ELF file it wraps. */
static int parse_self(Bytes data, Elf *out, char *error, size_t error_size) {
    memset(out, 0, sizeof(*out));
    if (!within(data, 0, 4)) return fail(error, error_size, "out of bounds: the file is empty");
    if (!memcmp(data.data, "\x7f" "ELF", 4)) {
        if (read_phdrs(data, 0, out, error, error_size)) return -1;
        const uint8_t *h = data.data;
        uint64_t end = u64(h + 32) + (uint64_t)out->count * 56;
        for (int i = 0; i < out->count; ++i)
            if (out->ph[i].offset + out->ph[i].filesz > end) end = out->ph[i].offset + out->ph[i].filesz;
        if (end > MAX_IMAGE) return fail(error, error_size, "probe image exceeds 512 MiB limit");
        if (!within(data, 0, end)) return fail(error, error_size, "out of bounds: the ELF's segments");
        out->elf = malloc(end ? end : 1);
        if (!out->elf) return fail(error, error_size, "out of memory");
        memcpy(out->elf, data.data, end);
        out->elf_size = end;
        return 0;
    }
    if (memcmp(data.data, "O\x15=\x1d", 4))
        return fail(error, error_size, "expected PS4 SELF or ELF (eboot.bin is neither: an encrypted or damaged dump?)");
    if (!within(data, 24, 2)) return fail(error, error_size, "out of bounds: the SELF header");
    const unsigned count = u16(data.data + 24);
    const uint64_t base = 32 + (uint64_t)count * 32;
    if (read_phdrs(data, base, out, error, error_size)) return -1;
    uint64_t end = 0;
    for (int i = 0; i < out->count; ++i)
        if (out->ph[i].offset + out->ph[i].filesz > end) end = out->ph[i].offset + out->ph[i].filesz;
    if (end > MAX_IMAGE) return fail(error, error_size, "probe image exceeds 512 MiB limit");
    const uint64_t header_end = u64(data.data + base + 32) + (uint64_t)out->count * 56;
    if (header_end > end) end = header_end;
    out->elf = calloc(1, end ? end : 1);
    if (!out->elf) return fail(error, error_size, "out of memory");
    out->elf_size = end;
    memcpy(out->elf, data.data + base, header_end);
    uint64_t covered[257][2] = {{0, header_end}};
    int covered_count = 1;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *s = data.data + 32 + i * 32;
        const uint64_t flags = u64(s), offset = u64(s + 8), size = u64(s + 16), mem = u64(s + 24);
        if (!(flags & 0x800)) continue;
        if (flags & 10) return fail(error, error_size, "encrypted/compressed SELF segment is unsupported");
        const unsigned index = (unsigned)(flags >> 20) & 4095;
        if (index >= (unsigned)out->count) return fail(error, error_size, "invalid segment index");
        const Phdr *p = &out->ph[index];
        if (size != p->filesz || size != mem) return fail(error, error_size, "unsupported blocked segment layout");
        if (!within(data, offset, size)) return fail(error, error_size, "out of bounds: a SELF segment");
        memcpy(out->elf + p->offset, data.data + offset, size);
        if (covered_count < 257) covered[covered_count][0] = p->offset, covered[covered_count++][1] = p->offset + size;
    }
    for (int i = 0; i < out->count; ++i) {
        const Phdr *p = &out->ph[i];
        if (!p->filesz) continue;
        int found = 0;
        for (int c = 0; c < covered_count && !found; ++c)
            found = covered[c][0] <= p->offset && p->offset + p->filesz <= covered[c][1];
        if (!found && (p->type == PT_LOAD || p->type == PT_DYNAMIC || p->type == PT_SCE_DYNLIBDATA || p->type == PT_SCE_RELRO))
            return fail(error, error_size, "required segment %d is unavailable", i);
    }
    return 0;
}

static const Phdr *first_of(const Elf *elf, uint32_t type) {
    for (int i = 0; i < elf->count; ++i)
        if (elf->ph[i].type == type) return &elf->ph[i];
    return NULL;
}

/* The dynamic tags (later ones win, as a Python dict built from them); `stop_at_null`: up to DT_NULL. */
typedef struct { uint64_t tag, value; } Tag;
static uint64_t tag_value(const Tag *tags, size_t count, uint64_t tag, int *found) {
    *found = 0;
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        if (tags[i].tag == tag) value = tags[i].value, *found = 1;
    return value;
}

static int read_tags(const Elf *elf, int stop_at_null, Tag **out, size_t *count, char *error, size_t error_size) {
    const Phdr *dp = first_of(elf, PT_DYNAMIC);
    if (!dp) return fail(error, error_size, "no dynamic segment");
    const Bytes e = {elf->elf, elf->elf_size};
    if (!within(e, dp->offset, dp->filesz)) return fail(error, error_size, "out of bounds: the dynamic segment");
    Tag *tags = malloc((dp->filesz / 16 + 1) * sizeof(*tags));
    if (!tags) return fail(error, error_size, "out of memory");
    size_t n = 0;
    for (uint64_t pos = dp->offset; pos + 16 <= dp->offset + dp->filesz; pos += 16) {
        const uint64_t tag = u64(elf->elf + pos), value = u64(elf->elf + pos + 8);
        if (stop_at_null && tag == 0) break;
        tags[n++] = (Tag){tag, value};
    }
    *out = tags, *count = n;
    return 0;
}

typedef struct { Bytes strings, syms; } SymbolTables;

static int symbol_tables(const Elf *elf, const Tag *tags, size_t count, SymbolTables *out, char *error, size_t error_size) {
    const Phdr *lib = first_of(elf, PT_SCE_DYNLIBDATA);
    if (!lib) return fail(error, error_size, "no SCE dynamic library data");
    const Bytes e = {elf->elf, elf->elf_size};
    if (!within(e, lib->offset, lib->filesz)) return fail(error, error_size, "out of bounds: the dynamic library data");
    const Bytes blob = {elf->elf + lib->offset, lib->filesz};
    int a, b, c, d;
    const uint64_t str_off = tag_value(tags, count, 0x61000035, &a), str_size = tag_value(tags, count, 0x61000037, &b);
    const uint64_t sym_off = tag_value(tags, count, 0x61000039, &c), sym_size = tag_value(tags, count, 0x6100003f, &d);
    if (!a || !b || !c || !d) return fail(error, error_size, "missing symbol table tags");
    if (!within(blob, str_off, str_size) || !within(blob, sym_off, sym_size))
        return fail(error, error_size, "out of bounds: the symbol tables");
    out->strings = (Bytes){blob.data + str_off, str_size};
    out->syms = (Bytes){blob.data + sym_off, sym_size};
    return 0;
}

/* A NUL-terminated name at `offset` of the string table, or NULL. */
static const char *name_at(Bytes strings, uint32_t offset) {
    if (offset >= strings.size) return NULL;
    const void *end = memchr(strings.data + offset, 0, strings.size - offset);
    return end ? (const char *)strings.data + offset : NULL;
}

/* scripts/prepare.py's inspect_libc: whether libc.prx's _init_env is exactly `ret`. */
static int init_env_is_ret(const char *game_dir, int *result, char *error, size_t error_size) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/sce_module/libc.prx", game_dir);
    Bytes data;
    if (read_file(path, &data)) return fail(error, error_size, "cannot read %s", path);
    Elf elf;
    int rc = parse_self(data, &elf, error, error_size);
    free((void *)data.data);
    if (rc) return -1;
    Tag *tags = NULL;
    size_t count = 0;
    SymbolTables tables;
    rc = read_tags(&elf, 0, &tags, &count, error, error_size) || symbol_tables(&elf, tags, count, &tables, error, error_size);
    *result = 0;
    for (size_t pos = 0; !rc && pos + 24 <= tables.syms.size; pos += 24) {
        const uint8_t *s = tables.syms.data + pos;
        const char *name = name_at(tables.strings, u32(s));
        if (!name) { rc = fail(error, error_size, "libc.prx: a symbol name out of bounds"); break; }
        const size_t key = strcspn(name, "#");
        const uint8_t info = s[4];
        const uint16_t section = u16(s + 6);
        const uint64_t value = u64(s + 8), size = u64(s + 16);
        if (key != strlen(INIT_ENV_NID) || strncmp(name, INIT_ENV_NID, key) || !section || (info & 15) != 2) continue;
        for (int i = 0; i < elf.count; ++i) {
            const Phdr *p = &elf.ph[i];
            if (p->type != PT_LOAD || !(p->flags & 1) || value < p->vaddr || value >= p->vaddr + p->filesz) continue;
            const Bytes e = {elf.elf, elf.elf_size};
            const uint64_t at = p->offset + value - p->vaddr;
            if (!within(e, at, size)) { rc = fail(error, error_size, "libc.prx: _init_env out of bounds"); break; }
            *result = size == 1 && elf.elf[at] == 0xc3;
            break;
        }
    }
    free(tags);
    free(elf.elf);
    return rc ? -1 : 0;
}

/* ---- the boot image ---- */

void bb_boot_free(BbBoot *boot) {
    free(boot->loads), free(boot->names), free(boot->relocs), free(boot->image);
    free(boot->modules), free(boot->bindings);
    memset(boot, 0, sizeof(*boot));
}

/* [addr, addr + size) inside one loadable segment's memory. */
static int mapped(const Phdr *const *loads, int count, uint64_t addr, uint64_t size) {
    for (int i = 0; i < count; ++i)
        if (loads[i]->vaddr <= addr && addr + size <= loads[i]->vaddr + loads[i]->memsz) return 1;
    return 0;
}

static int push_reloc(BbBoot *boot, size_t *capacity, BbBootReloc reloc) {
    if (boot->reloc_count == *capacity) {
        *capacity = *capacity ? *capacity * 2 : 1024;
        BbBootReloc *grown = realloc(boot->relocs, *capacity * sizeof(*grown));
        if (!grown) return -1;
        boot->relocs = grown;
    }
    boot->relocs[boot->reloc_count++] = reloc;
    return 0;
}

int bb_boot_prepare(const char *game_dir, BbBoot *boot, char *error, size_t error_size) {
    memset(boot, 0, sizeof(*boot));
    char path[4096];
    snprintf(path, sizeof(path), "%s/eboot.bin", game_dir);
    Bytes source;
    if (read_file(path, &source)) return fail(error, error_size, "cannot read %s", path);
    Elf elf;
    int rc = parse_self(source, &elf, error, error_size);
    free((void *)source.data);
    if (rc) return -1;
    Tag *tags = NULL;
    size_t tag_count = 0;
    int32_t *import_of = NULL; /* by symbol index: its import's index, or -1 */
    size_t *reloc_capacity = &boot->reloc_capacity;
    rc = -1;

    /* The loadable segments where they load. */
    const Phdr *loads[256];
    int load_count = 0;
    uint64_t size = 0;
    for (int i = 0; i < elf.count; ++i)
        if (elf.ph[i].type == PT_LOAD || elf.ph[i].type == PT_SCE_RELRO) {
            loads[load_count++] = &elf.ph[i];
            if (elf.ph[i].vaddr + elf.ph[i].memsz > size) size = elf.ph[i].vaddr + elf.ph[i].memsz;
        }
#define MAPPED(addr, n) mapped(loads, load_count, (addr), (n))
    int entry_mapped = 0;
    for (int i = 0; i < load_count; ++i)
        entry_mapped |= loads[i]->vaddr <= elf.entry && elf.entry + 1 <= loads[i]->vaddr + loads[i]->memsz;
    if (size > MAX_IMAGE || !entry_mapped) { fail(error, error_size, "invalid memory image"); goto done; }
    boot->image = calloc(1, size ? size : 1);
    boot->loads = calloc((size_t)load_count, sizeof(*boot->loads));
    if (!boot->image || !boot->loads) { fail(error, error_size, "out of memory"); goto done; }
    boot->size = size, boot->entry = elf.entry, boot->load_count = (size_t)load_count;
    const Bytes e = {elf.elf, elf.elf_size};
    for (int i = 0; i < load_count; ++i) {
        const Phdr *p = loads[i];
        if (p->filesz > p->memsz) { fail(error, error_size, "segment file size exceeds memory size"); goto done; }
        if (!within(e, p->offset, p->filesz)) { fail(error, error_size, "out of bounds: a loadable segment"); goto done; }
        memcpy(boot->image + p->vaddr, elf.elf + p->offset, p->filesz);
        boot->loads[i] = (BbBootLoad){p->vaddr, p->memsz, p->flags};
    }
    /* Only the supported executable runs (scripts/game_check.py); others fail in the game's code. */
    bb_sha256_hex(boot->image, size, boot->image_sha256);
    const char *skip = getenv("BB_SKIP_GAME_CHECK");
    if (!(skip && skip[0] == '1') && strcmp(boot->image_sha256, SUPPORTED_IMAGE)) {
        fail(error, error_size, "this is not Bloodborne 1.09's executable (image %.12s...): only update 1.09 runs",
             boot->image_sha256);
        goto done;
    }

    /* Relocations: base-relative, or against the executable's own or imported symbols. */
    SymbolTables tables;
    if (read_tags(&elf, 1, &tags, &tag_count, error, error_size) ||
        symbol_tables(&elf, tags, tag_count, &tables, error, error_size))
        goto done;
    if (tables.syms.size % 24) { fail(error, error_size, "bad symbol table size"); goto done; }
    const size_t symbol_count = tables.syms.size / 24;
    for (size_t i = 0; i < symbol_count; ++i)
        if (!name_at(tables.strings, u32(tables.syms.data + i * 24))) { fail(error, error_size, "out of bounds: a symbol name"); goto done; }
    import_of = malloc((symbol_count ? symbol_count : 1) * sizeof(*import_of));
    if (!import_of) { fail(error, error_size, "out of memory"); goto done; }
    for (size_t i = 0; i < symbol_count; ++i) import_of[i] = -1;
    static const uint64_t TABLES[2][2] = {{0x61000029, 0x6100002d}, {0x6100002f, 0x61000031}};
    const Phdr *lib = first_of(&elf, PT_SCE_DYNLIBDATA);
    const Bytes blob = {elf.elf + lib->offset, lib->filesz};
    for (int t = 0; t < 2; ++t) {
        int a, b;
        const uint64_t offset = tag_value(tags, tag_count, TABLES[t][0], &a), length = tag_value(tags, tag_count, TABLES[t][1], &b);
        if (!a || !b || !within(blob, offset, length)) { fail(error, error_size, "out of bounds: a relocation table"); goto done; }
        if (length % 24) { fail(error, error_size, "bad relocation table size"); goto done; }
        for (uint64_t pos = offset; pos < offset + length; pos += 24) {
            const uint64_t target = u64(blob.data + pos), info = u64(blob.data + pos + 8);
            const int64_t addend = (int64_t)u64(blob.data + pos + 16);
            const uint32_t kind = (uint32_t)info, sym = (uint32_t)(info >> 32);
            if (!MAPPED(target, 8)) { fail(error, error_size, "relocation outside mapped segments: %#llx", (unsigned long long)target); goto done; }
            if (kind == 8) { /* R_X86_64_RELATIVE */
                if (push_reloc(boot, reloc_capacity, (BbBootReloc){target, 0, addend, 0})) { fail(error, error_size, "out of memory"); goto done; }
            } else if (kind == 1 || kind == 6 || kind == 7) { /* R_X86_64_64, GLOB_DAT, JUMP_SLOT */
                if (sym >= symbol_count) { fail(error, error_size, "bad symbol index"); goto done; }
                const uint8_t *s = tables.syms.data + (size_t)sym * 24;
                const uint16_t section = u16(s + 6);
                const uint64_t value = u64(s + 8);
                if (section == 0xfff1) { /* absolute: written now */
                    const uint64_t v = value + (uint64_t)addend;
                    memcpy(boot->image + target, &v, 8);
                } else if (section) {
                    if (push_reloc(boot, reloc_capacity, (BbBootReloc){target, 0, (int64_t)(value + (uint64_t)addend), 0})) { fail(error, error_size, "out of memory"); goto done; }
                } else {
                    if (import_of[sym] < 0) import_of[sym] = (int32_t)boot->name_count++;
                    const uint64_t import_kind = (s[4] & 15) == 1 ? 2 : 1; /* STT_OBJECT: data */
                    if ((import_kind == 1 && addend) || addend < 0 || addend >= 4096) { fail(error, error_size, "unsupported import addend"); goto done; }
                    if (push_reloc(boot, reloc_capacity, (BbBootReloc){target, import_kind, import_of[sym], addend})) { fail(error, error_size, "out of memory"); goto done; }
                }
            } else {
                fail(error, error_size, "unsupported relocation %u", kind);
                goto done;
            }
        }
    }
    boot->names = calloc(boot->name_count ? boot->name_count : 1, sizeof(*boot->names));
    if (!boot->names) { fail(error, error_size, "out of memory"); goto done; }
    for (size_t i = 0; i < symbol_count; ++i) {
        if (import_of[i] < 0) continue;
        const char *name = name_at(tables.strings, u32(tables.syms.data + i * 24));
        if (strlen(name) >= 128) { fail(error, error_size, "import name too long"); goto done; }
        strcpy(boot->names[import_of[i]], name);
    }
    if (init_env_is_ret(game_dir, &boot->init_env_is_ret, error, error_size)) goto done;
    rc = 0;
done:
#undef MAPPED
    free(tags);
    free(import_of);
    free(elf.elf);
    if (rc) bb_boot_free(boot);
    return rc;
}

static int write64(FILE *f, uint64_t value) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(value >> (8 * i));
    return fwrite(b, 1, 8, f) == 8;
}

int bb_boot_write(const BbBoot *boot, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const int written = bb_boot_write_stream(boot, f);
    return fclose(f) == 0 && !written ? 0 : -1;
}

int bb_boot_write_stream(const BbBoot *boot, FILE *f) {
    uint8_t header[56];
    const uint64_t fields[6] = {boot->size, boot->entry, boot->load_count, boot->reloc_count, boot->name_count,
                                (uint64_t)boot->init_env_is_ret};
    memcpy(header, boot->linked ? "BBPROBE5" : "BBPROBE2", 8);
    for (int i = 0; i < 6; ++i)
        for (int b = 0; b < 8; ++b) header[8 + 8 * i + b] = (uint8_t)(fields[i] >> (8 * b));
    int ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
    if (boot->linked) { /* procparam, the eboot's TLS, the module table, the native bindings */
        ok = ok && write64(f, boot->procparam);
        for (int i = 0; ok && i < 4; ++i) ok = write64(f, boot->main_tls[i]);
        ok = ok && write64(f, boot->module_count);
        for (size_t i = 0; ok && i < boot->module_count; ++i) {
            const struct BbBootModule *m = &boot->modules[i];
            const uint64_t v[7] = {m->base, m->size, m->init, m->tls_address, m->tls_memsz, m->tls_filesz, m->tls_module};
            for (int k = 0; ok && k < 7; ++k) ok = write64(f, v[k]);
        }
        ok = ok && write64(f, boot->binding_count);
        for (size_t i = 0; ok && i < boot->binding_count; ++i)
            ok = write64(f, boot->bindings[i].index) && write64(f, boot->bindings[i].address) && write64(f, boot->bindings[i].kind);
    }
    for (size_t i = 0; ok && i < boot->load_count; ++i) {
        uint8_t load[24];
        const uint64_t v[3] = {boot->loads[i].vaddr, boot->loads[i].memsz, boot->loads[i].flags};
        for (int k = 0; k < 3; ++k)
            for (int b = 0; b < 8; ++b) load[8 * k + b] = (uint8_t)(v[k] >> (8 * b));
        ok = fwrite(load, 1, sizeof(load), f) == sizeof(load);
    }
    for (size_t i = 0; ok && i < boot->name_count; ++i) ok = fwrite(boot->names[i], 1, 128, f) == 128;
    for (size_t i = 0; ok && i < boot->reloc_count; ++i) {
        uint8_t r[32];
        const uint64_t v[4] = {boot->relocs[i].target, boot->relocs[i].kind, (uint64_t)boot->relocs[i].value,
                               (uint64_t)boot->relocs[i].addend};
        for (int k = 0; k < 4; ++k)
            for (int b = 0; b < 8; ++b) r[8 * k + b] = (uint8_t)(v[k] >> (8 * b));
        ok = fwrite(r, 1, sizeof(r), f) == sizeof(r);
    }
    ok = ok && fwrite(boot->image, 1, boot->size, f) == boot->size;
    return ok ? 0 : -1;
}

/* ---- the AppContent profile (scripts/content_profile.py) ---- */

/* param.sfo's integer value for `key`, or 0 when absent; -1 when the file is damaged. */
static int sfo_int(Bytes sfo, const char *key, uint32_t *value) {
    *value = 0;
    if (!within(sfo, 0, 20) || memcmp(sfo.data, "\0PSF", 4)) return -1;
    const uint32_t keys = u32(sfo.data + 8), values = u32(sfo.data + 12), count = u32(sfo.data + 16);
    for (uint32_t i = 0; i < count; ++i) {
        if (!within(sfo, 20 + (uint64_t)i * 16, 16)) return -1;
        const uint8_t *entry = sfo.data + 20 + i * 16;
        const uint32_t name = keys + u16(entry), size = u32(entry + 4), offset = values + u32(entry + 12);
        const uint16_t format = u16(entry + 2);
        if (name >= sfo.size || !memchr(sfo.data + name, 0, sfo.size - name)) return -1;
        if (strcmp((const char *)sfo.data + name, key)) continue;
        if (format != 0x404 || size != 4 || !within(sfo, offset, 4)) return -1; /* not an integer */
        *value = u32(sfo.data + offset);
    }
    return 0;
}

int bb_content_profile(const char *game_dir, int full, BbContentProfile *out, char *error, size_t error_size) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", game_dir);
    Bytes sfo;
    if (read_file(path, &sfo)) return fail(error, error_size, "cannot read %s", path);
    out->sku = full ? 3 : 1;
    int rc = 0;
    for (int i = 0; i < 4 && !rc; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "USER_DEFINED_PARAM_%d", i + 1);
        if (sfo_int(sfo, key, &out->user_params[i])) rc = fail(error, error_size, "invalid user-defined parameter");
    }
    free((void *)sfo.data);
    return rc;
}

int bb_content_write(const BbContentProfile *profile, const char *path) {
    uint8_t bytes[28];
    memcpy(bytes, "BBCONT01", 8);
    const uint32_t fields[5] = {profile->sku, profile->user_params[0], profile->user_params[1],
                                profile->user_params[2], profile->user_params[3]};
    for (int i = 0; i < 5; ++i)
        for (int b = 0; b < 4; ++b) bytes[8 + 4 * i + b] = (uint8_t)(fields[i] >> (8 * b));
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const int ok = fwrite(bytes, 1, sizeof(bytes), f) == sizeof(bytes);
    return fclose(f) == 0 && ok ? 0 : -1;
}

/* ---- linking the game's system modules (scripts/link_modules.py) ---- */

/* An insertion-ordered map of strings to values (a Python dict's semantics: a key keeps its first
 * position, a later put updates its value). */
typedef struct { char *key; int64_t value; } MapEntry;
typedef struct { MapEntry *entries; size_t count, capacity; uint32_t *slots; size_t slot_count; } Map;

static uint64_t hash_string(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}

static void map_free(Map *m) {
    for (size_t i = 0; i < m->count; ++i) free(m->entries[i].key);
    free(m->entries), free(m->slots);
    memset(m, 0, sizeof(*m));
}

static int64_t map_find(const Map *m, const char *key) {
    if (!m->slot_count) return -1;
    for (size_t i = hash_string(key) & (m->slot_count - 1);; i = (i + 1) & (m->slot_count - 1)) {
        const uint32_t slot = m->slots[i];
        if (!slot) return -1;
        if (!strcmp(m->entries[slot - 1].key, key)) return (int64_t)slot - 1;
    }
}

static int map_rehash(Map *m, size_t slots) {
    uint32_t *fresh = calloc(slots, sizeof(*fresh));
    if (!fresh) return -1;
    free(m->slots);
    m->slots = fresh, m->slot_count = slots;
    for (size_t e = 0; e < m->count; ++e) {
        size_t i = hash_string(m->entries[e].key) & (slots - 1);
        while (m->slots[i]) i = (i + 1) & (slots - 1);
        m->slots[i] = (uint32_t)e + 1;
    }
    return 0;
}

/* Sets `key` to `value`; returns the entry's position, or -1 when out of memory. */
static int64_t map_put(Map *m, const char *key, int64_t value) {
    const int64_t found = map_find(m, key);
    if (found >= 0) { m->entries[found].value = value; return found; }
    if (m->count == m->capacity) {
        m->capacity = m->capacity ? m->capacity * 2 : 64;
        MapEntry *grown = realloc(m->entries, m->capacity * sizeof(*grown));
        if (!grown) return -1;
        m->entries = grown;
    }
    char *copy = strdup(key);
    if (!copy) return -1;
    m->entries[m->count++] = (MapEntry){copy, value};
    if (m->count * 2 > m->slot_count && map_rehash(m, m->slot_count ? m->slot_count * 2 : 128)) return -1;
    if (m->count * 2 <= m->slot_count && map_find(m, key) < 0 && map_rehash(m, m->slot_count)) return -1;
    return (int64_t)m->count - 1;
}

/* A library's or module's identity: its name and version (version << 48 | string offset). */
typedef struct { char key[12]; const char *name; unsigned version; } IdEntry;

typedef struct {
    const char *name; /* "" for none */
    char *identity;   /* "nid\x1flibrary\x1eversion\x1fmodule\x1eversion", NULL when not NID#lib#mod */
    const char *library; /* the identity's library name (for the NID#library spelling) */
    uint8_t type, binding;
    uint16_t section;
    uint64_t value, size;
} Symbol;

typedef struct {
    Elf elf;
    Tag *tags; /* up to DT_NULL */
    size_t tag_count;
    IdEntry *libraries, *modules;
    size_t library_count, module_count;
    Symbol *symbols;
    size_t symbol_count;
    struct { uint64_t target; uint32_t kind, sym; int64_t addend; } *relocs;
    size_t reloc_count;
} Module;

/* The PS4's 64-character spelling of a library or module number (link_libc.py's encode_id). */
static void encode_id(uint64_t value, char out[12]) {
    static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    char reversed[12];
    int n = 0;
    do { reversed[n++] = ALPHABET[value & 63]; value >>= 6; } while (value && n < 11);
    for (int i = 0; i < n; ++i) out[i] = reversed[n - 1 - i];
    out[n] = 0;
}

static void module_free(Module *m) {
    for (size_t i = 0; i < m->symbol_count; ++i) free(m->symbols[i].identity);
    free(m->elf.elf), free(m->tags), free(m->libraries), free(m->modules), free(m->symbols), free(m->relocs);
    memset(m, 0, sizeof(*m));
}

static const IdEntry *find_id(const IdEntry *table, size_t count, const char *key, size_t key_length) {
    for (size_t i = 0; i < count; ++i)
        if (strlen(table[i].key) == key_length && !strncmp(table[i].key, key, key_length)) return &table[i];
    return NULL;
}

/* Adds key -> identity to `table`; a key seen before must name the same identity. */
static int add_id(IdEntry **table, size_t *count, const char *key, const char *name, unsigned version) {
    const IdEntry *seen = find_id(*table, *count, key, strlen(key));
    if (seen) return strcmp(seen->name, name) || seen->version != version ? -1 : 0;
    IdEntry *grown = realloc(*table, (*count + 1) * sizeof(**table));
    if (!grown) return -1;
    *table = grown;
    IdEntry *entry = &(*table)[(*count)++];
    snprintf(entry->key, sizeof(entry->key), "%s", key);
    entry->name = name, entry->version = version;
    return 0;
}

/* The identity key of NID#library#module, or NULL (plain names are not linkable); -1 in `bad`
 * when its library or module is unknown. */
static char *identity_of(const Module *m, const char *name, const char **library, int *bad) {
    *bad = 0;
    const char *first = strchr(name, '#');
    const char *second = first ? strchr(first + 1, '#') : NULL;
    if (!first || !second || strchr(second + 1, '#')) return NULL;
    const IdEntry *lib = find_id(m->libraries, m->library_count, first + 1, (size_t)(second - first - 1));
    const IdEntry *mod = find_id(m->modules, m->module_count, second + 1, strlen(second + 1));
    if (!lib || !mod) { *bad = 1; return NULL; }
    const size_t length = (size_t)(first - name) + strlen(lib->name) + strlen(mod->name) + 32;
    char *key = malloc(length);
    if (!key) { *bad = 1; return NULL; }
    snprintf(key, length, "%.*s\x1f%s\x1e%u\x1f%s\x1e%u", (int)(first - name), name, lib->name, lib->version,
             mod->name, mod->version);
    if (library) *library = lib->name;
    return key;
}

static int read_module(const char *path, Module *m, char *error, size_t error_size) {
    memset(m, 0, sizeof(*m));
    Bytes data;
    if (read_file(path, &data)) return fail(error, error_size, "cannot read %s", path);
    int rc = parse_self(data, &m->elf, error, error_size);
    free((void *)data.data);
    SymbolTables tables;
    if (rc || read_tags(&m->elf, 1, &m->tags, &m->tag_count, error, error_size) ||
        symbol_tables(&m->elf, m->tags, m->tag_count, &tables, error, error_size)) {
        module_free(m);
        return -1;
    }
    for (size_t i = 0; i < m->tag_count; ++i) {
        const uint64_t tag = m->tags[i].tag, value = m->tags[i].value;
        const int library = tag == 0x61000013 || tag == 0x61000015, module = tag == 0x6100000d || tag == 0x6100000f;
        if (!library && !module) continue;
        const char *name = name_at(tables.strings, (uint32_t)value);
        char key[12];
        encode_id(value >> 48, key);
        if (!name || add_id(library ? &m->libraries : &m->modules, library ? &m->library_count : &m->module_count,
                            key, name, (unsigned)(value >> 32) & 65535)) {
            module_free(m);
            return fail(error, error_size, "%s: conflicting module/library IDs", path);
        }
    }
    m->symbol_count = tables.syms.size / 24;
    m->symbols = calloc(m->symbol_count ? m->symbol_count : 1, sizeof(*m->symbols));
    if (!m->symbols) { module_free(m); return fail(error, error_size, "out of memory"); }
    for (size_t i = 0; i < m->symbol_count; ++i) {
        const uint8_t *s = tables.syms.data + i * 24;
        Symbol *sym = &m->symbols[i];
        sym->name = name_at(tables.strings, u32(s));
        if (!sym->name) { module_free(m); return fail(error, error_size, "%s: a symbol name out of bounds", path); }
        sym->type = s[4] & 15, sym->binding = s[4] >> 4, sym->section = u16(s + 6);
        sym->value = u64(s + 8), sym->size = u64(s + 16);
        int bad = 0;
        if (*sym->name) sym->identity = identity_of(m, sym->name, &sym->library, &bad);
        if (bad) { module_free(m); return fail(error, error_size, "%s: a symbol of an unknown library or module", path); }
    }
    const Phdr *lib = first_of(&m->elf, PT_SCE_DYNLIBDATA);
    const Bytes blob = {m->elf.elf + lib->offset, lib->filesz};
    static const uint64_t TABLES[2][2] = {{0x61000029, 0x6100002d}, {0x6100002f, 0x61000031}};
    for (int t = 0; t < 2; ++t) {
        int a, b;
        const uint64_t offset = tag_value(m->tags, m->tag_count, TABLES[t][0], &a);
        const uint64_t length = tag_value(m->tags, m->tag_count, TABLES[t][1], &b);
        if (!a || !b || !within(blob, offset, length)) { module_free(m); return fail(error, error_size, "%s: a relocation table out of bounds", path); }
        void *grown = realloc(m->relocs, (m->reloc_count + length / 24 + 1) * sizeof(*m->relocs));
        if (!grown) { module_free(m); return fail(error, error_size, "out of memory"); }
        m->relocs = grown;
        for (uint64_t pos = offset; pos + 24 <= offset + length; pos += 24) {
            const uint64_t info = u64(blob.data + pos + 8);
            m->relocs[m->reloc_count].target = u64(blob.data + pos);
            m->relocs[m->reloc_count].kind = (uint32_t)info;
            m->relocs[m->reloc_count].sym = (uint32_t)(info >> 32);
            m->relocs[m->reloc_count++].addend = (int64_t)u64(blob.data + pos + 16);
        }
    }
    return 0;
}

/* Initial-exec `mov rax, fs:[0]` to `mov rax, gs:[disp32]` in a module's executable segments,
 * each displacement with a relocation of kind 3 (the guest thread pointer's slot). */
static int patch_fs_loads(BbBoot *boot, const Elf *elf, uint64_t base, size_t *patched) {
    static const uint8_t FS_LOAD[9] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    for (int i = 0; i < elf->count; ++i) {
        const Phdr *p = &elf->ph[i];
        if (p->type != PT_LOAD || !(p->flags & 1)) continue;
        const uint64_t start = base + p->vaddr, end = base + p->vaddr + p->filesz;
        if (end > boot->size) return -1;
        for (uint64_t at = start; at + sizeof(FS_LOAD) <= end;) {
            const uint8_t *hit = memchr(boot->image + at, 0x64, end - at);
            if (!hit) break;
            at = (uint64_t)(hit - boot->image);
            if (at + sizeof(FS_LOAD) > end) break;
            if (memcmp(hit, FS_LOAD, sizeof(FS_LOAD))) { ++at; continue; }
            boot->image[at] = 0x65;
            if (push_reloc(boot, &boot->reloc_capacity, (BbBootReloc){at + 5, 3, 0, 0})) return -1;
            ++*patched;
            at += sizeof(FS_LOAD);
        }
    }
    return 0;
}

static int add_name(BbBoot *boot, const char *name) {
    char (*grown)[128] = realloc(boot->names, (boot->name_count + 1) * sizeof(*grown));
    if (!grown) return -1;
    boot->names = grown;
    snprintf(boot->names[boot->name_count++], 128, "%s", name);
    return 0;
}

int bb_boot_link(const char *game_dir, BbBoot *boot, char *error, size_t error_size) {
    static const char *const MODULES[] = {"libc.prx", "libSceFios2.prx"};
    char path[4096];
    snprintf(path, sizeof(path), "%s/eboot.bin", game_dir);
    Module main;
    if (read_module(path, &main, error, error_size)) return -1;
    Map identities = {0}, exports = {0}, by_nid = {0};
    struct Export { uint64_t address, kind; } *export_list = NULL;
    struct Nid { size_t count; uint64_t address, kind; } *nid_list = NULL;
    size_t export_count = 0, nid_count = 0, fs_patched = 0;
    int rc = -1;
    Module m = {0};

    /* The executable's imports by identity (a name without one counts as the empty identity). */
    for (size_t i = 0; i < boot->name_count; ++i) {
        int bad = 0;
        char *key = identity_of(&main, boot->names[i], NULL, &bad);
        if (bad) { fail(error, error_size, "eboot.bin: an import of an unknown library or module"); goto done; }
        const int64_t put = map_put(&identities, key ? key : "", (int64_t)i);
        free(key);
        if (put < 0) { fail(error, error_size, "out of memory"); goto done; }
    }
    const uint64_t executable_size = boot->size;
    uint64_t base = (boot->size + 65535) & ~(uint64_t)65535;
    if (patch_fs_loads(boot, &main.elf, 0, &fs_patched)) { fail(error, error_size, "eboot.bin: fs loads"); goto done; }
    uint64_t tls_module = 2;
    /* The image grows once, to its linked size: grown module by module, each realloc held the old
     * and the new image (about 90 MB each) at once, room the iPhone launcher does not have. */
    uint64_t image_capacity = 0;
    {
        uint64_t end = base, next = base;
        for (size_t f = 0; f < sizeof(MODULES) / sizeof(*MODULES); ++f) {
            snprintf(path, sizeof(path), "%s/sce_module/%s", game_dir, MODULES[f]);
            if (read_module(path, &m, error, error_size)) goto done;
            uint64_t modsize = 0;
            for (int i = 0; i < m.elf.count; ++i)
                if ((m.elf.ph[i].type == PT_LOAD || m.elf.ph[i].type == PT_SCE_RELRO) &&
                    m.elf.ph[i].vaddr + m.elf.ph[i].memsz > modsize)
                    modsize = m.elf.ph[i].vaddr + m.elf.ph[i].memsz;
            module_free(&m);
            end = next + modsize;
            next = (next + modsize + 65535) & ~(uint64_t)65535;
        }
        if (end > MAX_IMAGE) { fail(error, error_size, "linked image exceeds probe limit"); goto done; }
        uint8_t *grown = realloc(boot->image, end);
        if (!grown) goto oom;
        boot->image = grown, image_capacity = end;
    }
    for (size_t f = 0; f < sizeof(MODULES) / sizeof(*MODULES); ++f) {
        const char *file = MODULES[f];
        snprintf(path, sizeof(path), "%s/sce_module/%s", game_dir, file);
        if (read_module(path, &m, error, error_size)) goto done;
        const Phdr *loads[256];
        int load_count = 0;
        uint64_t modsize = 0;
        for (int i = 0; i < m.elf.count; ++i)
            if (m.elf.ph[i].type == PT_LOAD || m.elf.ph[i].type == PT_SCE_RELRO) {
                loads[load_count++] = &m.elf.ph[i];
                if (m.elf.ph[i].vaddr + m.elf.ph[i].memsz > modsize) modsize = m.elf.ph[i].vaddr + m.elf.ph[i].memsz;
            }
        if (base + modsize > image_capacity) { fail(error, error_size, "%s: the linked image's size changed", file); goto done; }
        BbBootLoad *more = realloc(boot->loads, (boot->load_count + (size_t)load_count) * sizeof(*more));
        if (!more) { fail(error, error_size, "out of memory"); goto done; }
        boot->loads = more;
        memset(boot->image + boot->size, 0, base + modsize - boot->size);
        boot->size = base + modsize;
        const Bytes e = {m.elf.elf, m.elf.elf_size};
        for (int i = 0; i < load_count; ++i) {
            const Phdr *p = loads[i];
            if (p->filesz > p->memsz || !within(e, p->offset, p->filesz)) { fail(error, error_size, "%s: invalid segment size", file); goto done; }
            memcpy(boot->image + base + p->vaddr, m.elf.elf + p->offset, p->filesz);
            boot->loads[boot->load_count++] = (BbBootLoad){base + p->vaddr, p->memsz, p->flags};
        }
        const Phdr *tls = first_of(&m.elf, 7);
        uint64_t module_id = 0;
        if (tls) {
            if (tls->filesz > tls->memsz || !mapped(loads, load_count, tls->vaddr, tls->memsz)) { fail(error, error_size, "%s: unsupported TLS layout", file); goto done; }
            module_id = tls_module++;
        }
        for (size_t r = 0; r < m.reloc_count; ++r) {
            uint64_t target = m.relocs[r].target;
            const uint32_t kind = m.relocs[r].kind, symid = m.relocs[r].sym;
            const int64_t addend = m.relocs[r].addend;
            if (!mapped(loads, load_count, target, 8)) { fail(error, error_size, "%s: relocation outside image %#llx", file, (unsigned long long)target); goto done; }
            target += base;
            if (kind == 8) {
                if (push_reloc(boot, &boot->reloc_capacity, (BbBootReloc){target, 0, (int64_t)base + addend, 0})) goto oom;
                continue;
            }
            if (kind != 16 && kind != 1 && kind != 6 && kind != 7) { fail(error, error_size, "%s: unsupported relocation type %u", file, kind); goto done; }
            if (symid >= m.symbol_count) { fail(error, error_size, "%s: bad symbol index", file); goto done; }
            const Symbol *sym = &m.symbols[symid];
            if (kind == 16) { /* the module's own TLS block: its module number */
                if (*sym->name || sym->type != 3 || addend || !module_id) { fail(error, error_size, "%s: unsupported external TLS relocation", file); goto done; }
                memcpy(boot->image + target, &module_id, 8);
            } else if (sym->section) {
                if (sym->section == 0xfff1) {
                    const uint64_t v = sym->value + (uint64_t)addend;
                    memcpy(boot->image + target, &v, 8);
                } else if (push_reloc(boot, &boot->reloc_capacity, (BbBootReloc){target, 0, (int64_t)(base + sym->value + (uint64_t)addend), 0})) {
                    goto oom;
                }
            } else if (!sym->identity && sym->binding == 2) {
                memset(boot->image + target, 0, 8); /* weak undefined (module_start/stop) */
            } else {
                if (!sym->identity) { fail(error, error_size, "%s: an import without an identity", file); goto done; }
                const uint64_t import_kind = sym->type == 1 ? 2 : 1;
                if ((import_kind == 1 && addend) || addend < 0 || addend >= 4096) { fail(error, error_size, "%s: unsupported import addend", file); goto done; }
                int64_t index = map_find(&identities, sym->identity);
                if (index >= 0) {
                    index = identities.entries[index].value;
                } else {
                    /* The executable's spelling when its tables know the library and module (the
                     * host's tables use it), else NID#library. */
                    const char *nid_end = strchr(sym->name, '#');
                    const char *lib_key = NULL, *mod_key = NULL;
                    char lib_identity[160], mod_identity[160];
                    const char *first = strchr(sym->identity, '\x1f'), *second = first ? strchr(first + 1, '\x1f') : NULL;
                    snprintf(lib_identity, sizeof(lib_identity), "%.*s", second ? (int)(second - first - 1) : 0, first ? first + 1 : "");
                    snprintf(mod_identity, sizeof(mod_identity), "%s", second ? second + 1 : "");
                    for (size_t k = 0; k < main.library_count; ++k) { /* the last key with that identity */
                        char id[160];
                        snprintf(id, sizeof(id), "%s\x1e%u", main.libraries[k].name, main.libraries[k].version);
                        if (!strcmp(id, lib_identity)) lib_key = main.libraries[k].key;
                    }
                    for (size_t k = 0; k < main.module_count; ++k) {
                        char id[160];
                        snprintf(id, sizeof(id), "%s\x1e%u", main.modules[k].name, main.modules[k].version);
                        if (!strcmp(id, mod_identity)) mod_key = main.modules[k].key;
                    }
                    char name[256];
                    if (lib_key && mod_key)
                        snprintf(name, sizeof(name), "%.*s#%s#%s", (int)(nid_end - sym->name), sym->name, lib_key, mod_key);
                    else
                        snprintf(name, sizeof(name), "%.*s#%s", (int)(nid_end - sym->name), sym->name, sym->library);
                    index = (int64_t)boot->name_count;
                    if (add_name(boot, name) || map_put(&identities, sym->identity, index) < 0) goto oom;
                }
                if (push_reloc(boot, &boot->reloc_capacity, (BbBootReloc){target, import_kind, index, addend})) goto oom;
            }
        }
        for (size_t i = 0; i < m.symbol_count; ++i) { /* the module's exports */
            const Symbol *s = &m.symbols[i];
            if (!s->section || (s->binding != 1 && s->binding != 2) || !s->identity || (s->type != 1 && s->type != 2)) continue;
            if (!mapped(loads, load_count, s->value, s->size ? s->size : 1)) { fail(error, error_size, "%s: export outside load segments", file); goto done; }
            if (map_find(&exports, s->identity) >= 0) { fail(error, error_size, "%s: ambiguous export", file); goto done; }
            void *grown_exports = realloc(export_list, (export_count + 1) * sizeof(*export_list));
            if (!grown_exports) goto oom;
            export_list = grown_exports;
            export_list[export_count] = (struct Export){base + s->value, s->type == 1 ? 2 : 1};
            if (map_put(&exports, s->identity, (int64_t)export_count++) < 0) goto oom;
            char nid_key[256];
            snprintf(nid_key, sizeof(nid_key), "%.*s\x1f%s", (int)(strchr(s->name, '#') - s->name), s->name, s->library);
            const int64_t at = map_find(&by_nid, nid_key);
            if (at >= 0) {
                nid_list[by_nid.entries[at].value].count++;
            } else {
                void *grown_nids = realloc(nid_list, (nid_count + 1) * sizeof(*nid_list));
                if (!grown_nids) goto oom;
                nid_list = grown_nids;
                nid_list[nid_count] = (struct Nid){1, base + s->value, s->type == 1 ? 2 : 1};
                if (map_put(&by_nid, nid_key, (int64_t)nid_count++) < 0) goto oom;
            }
        }
        if (patch_fs_loads(boot, &m.elf, base, &fs_patched)) { fail(error, error_size, "%s: fs loads", file); goto done; }
        int has_init = 0;
        const uint64_t init = tag_value(m.tags, m.tag_count, 12, &has_init);
        struct BbBootModule *table = realloc(boot->modules, (boot->module_count + 1) * sizeof(*table));
        if (!table) goto oom;
        boot->modules = table;
        boot->modules[boot->module_count++] = (struct BbBootModule){
            base, modsize, base + (has_init ? init : 0), tls ? base + tls->vaddr : 0, tls ? tls->memsz : 0,
            tls ? tls->filesz : 0, module_id};
        base = (base + modsize + 65535) & ~(uint64_t)65535;
        module_free(&m);
    }

    /* The imports a module exports natively; libSceLibcInternal's are libc.prx's same NID. */
    for (size_t i = 0; i < identities.count; ++i) {
        const char *key = identities.entries[i].key;
        int64_t found = map_find(&exports, key);
        struct Export binding = found >= 0 ? export_list[exports.entries[found].value] : (struct Export){0, 0};
        if (found < 0 && *key) {
            const char *first = strchr(key, '\x1f');
            if (first && !strncmp(first + 1, "libSceLibcInternal\x1e", 19)) {
                char nid_key[256];
                snprintf(nid_key, sizeof(nid_key), "%.*s\x1flibc", (int)(first - key), key);
                const int64_t at = map_find(&by_nid, nid_key);
                if (at >= 0 && nid_list[by_nid.entries[at].value].count == 1) {
                    found = at;
                    binding = (struct Export){nid_list[by_nid.entries[at].value].address, nid_list[by_nid.entries[at].value].kind};
                }
            }
        }
        if (found < 0) continue;
        struct BbBootBinding *grown = realloc(boot->bindings, (boot->binding_count + 1) * sizeof(*grown));
        if (!grown) goto oom;
        boot->bindings = grown;
        boot->bindings[boot->binding_count++] = (struct BbBootBinding){(uint64_t)identities.entries[i].value, binding.address, binding.kind};
    }
    const Phdr *main_tls = first_of(&main.elf, 7);
    if (main_tls && (main_tls->filesz > main_tls->memsz || main_tls->memsz > (1u << 20) ||
                     main_tls->vaddr + main_tls->filesz > executable_size)) {
        fail(error, error_size, "unsupported eboot TLS layout");
        goto done;
    }
    if (main_tls) {
        boot->main_tls[0] = main_tls->vaddr, boot->main_tls[1] = main_tls->filesz;
        boot->main_tls[2] = main_tls->memsz, boot->main_tls[3] = main_tls->align;
    }
    const Phdr *procparam = first_of(&main.elf, 0x61000001);
    if (!procparam) { fail(error, error_size, "eboot.bin: no process parameters"); goto done; }
    boot->procparam = procparam->vaddr;
    boot->linked = 1;
    rc = 0;
    goto done;
oom:
    fail(error, error_size, "out of memory");
done:
    module_free(&m);
    module_free(&main);
    map_free(&identities), map_free(&exports), map_free(&by_nid);
    free(export_list), free(nid_list);
    return rc;
}
