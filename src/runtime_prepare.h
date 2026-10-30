/* The game's executable prepared for the loader, in memory: what scripts/prepare.py writes as
 * boot.bin (BBPROBE2), without Python, so a launcher on any device can start the game itself
 * (bloodborne-mac's roadmap: the runtime as a library). */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct { uint64_t vaddr, memsz, flags; } BbBootLoad;
/* kind 0: a base-relative value; 1: a function import (value: its index); 2: a data import. */
typedef struct { uint64_t target, kind; int64_t value, addend; } BbBootReloc;

typedef struct {
    uint64_t size, entry;
    int init_env_is_ret; /* libc.prx's _init_env is a bare ret (checked, never assumed) */
    BbBootLoad *loads;
    size_t load_count;
    char (*names)[128]; /* import names, NID#library#module, in first-use order */
    size_t name_count;
    BbBootReloc *relocs;
    size_t reloc_count;
    uint8_t *image; /* size bytes: the loadable segments where they load */
    char image_sha256[65];
    size_t reloc_capacity;
    /* After bb_boot_link (BBPROBE5): the modules linked after the executable, the imports they
     * resolve natively, the executable's process parameters and TLS. */
    int linked;
    uint64_t procparam, main_tls[4]; /* main_tls: vaddr, filesz, memsz, align */
    struct BbBootModule { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } *modules;
    size_t module_count;
    struct BbBootBinding { uint64_t index, address, kind; } *bindings;
    size_t binding_count;
} BbBoot;

/* Reads GAME_DIR/eboot.bin (a plaintext SELF or ELF) and sce_module/libc.prx. 0 on success; else
 * -1 with the reason in `error`. Only the supported executable passes (its image's SHA-256),
 * unless BB_SKIP_GAME_CHECK=1. */
int bb_boot_prepare(const char *game_dir, BbBoot *boot, char *error, size_t error_size);
/* Links GAME_DIR/sce_module's libc.prx and libSceFios2.prx after the executable, as
 * scripts/link_modules.py does: boot-linked.bin's contents. 0 on success. */
int bb_boot_link(const char *game_dir, BbBoot *boot, char *error, size_t error_size);
/* boot.bin (BBPROBE2) as scripts/prepare.py writes it, or boot-linked.bin (BBPROBE5) as
 * scripts/link_modules.py writes it once linked. 0 on success. */
int bb_boot_write(const BbBoot *boot, const char *path);
/* The same, to an open stream (the game reads it from memory: probe.c's `-`). */
#include <stdio.h>
int bb_boot_write_stream(const BbBoot *boot, FILE *f);
void bb_boot_free(BbBoot *boot);
/* The AppContent profile scripts/content_profile.py writes as content.bin: the SKU (3: full, 1:
 * trial) and the four USER_DEFINED_PARAM values of GAME_DIR/sce_sys/param.sfo. 0 on success. */
typedef struct { uint32_t sku, user_params[4]; } BbContentProfile;
int bb_content_profile(const char *game_dir, int full, BbContentProfile *out, char *error, size_t error_size);
int bb_content_write(const BbContentProfile *profile, const char *path);
/* GAME_DIR/eboot.bin's loadable (PT_LOAD) segments as [vaddr, vaddr + memsz), at most MAX:
 * what patches may write to (runtime_patches.c). 0 on success. */
int bb_eboot_segments(const char *game_dir, uint64_t (*segments)[2], size_t max, size_t *count,
                      char *error, size_t error_size);
/* The SHA-256 of `size` bytes as 64 hexadecimal digits. */
void bb_sha256_hex(const void *data, size_t size, char out[65]);
