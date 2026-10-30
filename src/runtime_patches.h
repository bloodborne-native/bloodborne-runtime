/* scripts/patches.py in C: patches.bin (BBPATCH2) from the game's patch file, bbport.ini and the
 * player's patch folder, built in memory so the game starts without Python (bloodborne-mac's
 * roadmap: the runtime as a library). The same bytes and messages as the script. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *xml;            /* the built-in patch file (patches/Bloodborne.xml) */
    const char *settings;       /* bbport.ini */
    const char *game_dir;       /* eboot.bin's segments and the debug menu's files */
    const char *patches_dir;    /* the player's patch files (*.xml), or NULL */
    const char *patches_config; /* patches.json (enabled/disabled), or NULL */
    const char *fps;            /* 30, 60, 90 or uncap */
    const char *extra;          /* more patch names separated by ';', or NULL */
    const char *render_res;     /* WxH (the scene's size), or NULL or "" */
    const char *app_version;    /* 01.09 */
} BbPatchOptions;

/* patches.bin's bytes in *BLOB (malloc'd, SIZE bytes). 0 on success; else -1 with the reason in
 * ERROR. Prints the script's "Patches: ..." lines. */
int bb_patches_build(const BbPatchOptions *options, uint8_t **blob, size_t *size, char *error,
                     size_t error_size);

/* patches.py --print-scaled: the render and output sizes when bbport.ini selects an output other
 * than 1080p or an upscaler preset; 0 when the game is unpatched (native 1080p). */
int bb_patches_scaled_sizes(const char *settings, int render[2], int output[2]);
