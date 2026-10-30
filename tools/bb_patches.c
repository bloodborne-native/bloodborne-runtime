/* bb-patches: scripts/patches.py's command line on src/runtime_patches.c (OUT/patches.bin and its
 * messages, or --print-scaled), so the C version is checked against the script byte for byte and
 * run.sh can use it in its place. */
#include "../src/runtime_patches.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    BbPatchOptions o = {
        .xml = "patches/Bloodborne.xml",
        .settings = "bbport.ini",
        .game_dir = getenv("BB_GAME_DIR") ? getenv("BB_GAME_DIR") : "../CUSA03173",
        .fps = "uncap",
        .app_version = "01.09",
    };
    const char *out = "out";
    int print_scaled = 0;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--print-scaled")) { print_scaled = 1; continue; }
        if (!v) { fprintf(stderr, "%s: %s needs a value\n", argv[0], a); return 2; }
        if (!strcmp(a, "--xml")) o.xml = v;
        else if (!strcmp(a, "--settings")) o.settings = v;
        else if (!strcmp(a, "--game-dir")) o.game_dir = v;
        else if (!strcmp(a, "--patches-dir")) o.patches_dir = v;
        else if (!strcmp(a, "--patches-config")) o.patches_config = v;
        else if (!strcmp(a, "--fps")) o.fps = v;
        else if (!strcmp(a, "--extra")) o.extra = v;
        else if (!strcmp(a, "--render-res")) o.render_res = v;
        else if (!strcmp(a, "--output-res")) { /* accepted as the script does: the UI stays 1920x1080 */ }
        else if (!strcmp(a, "--app-version")) o.app_version = v;
        else if (!strcmp(a, "--out")) out = v;
        else { fprintf(stderr, "%s: unknown option %s\n", argv[0], a); return 2; }
        ++i;
    }
    if (print_scaled) {
        int render[2], output[2];
        if (bb_patches_scaled_sizes(o.settings, render, output))
            printf("%dx%d %dx%d\n", render[0], render[1], output[0], output[1]);
        return 0;
    }
    uint8_t *blob;
    size_t size;
    char error[1024];
    if (bb_patches_build(&o, &blob, &size, error, sizeof(error))) {
        fprintf(stderr, "Patches: %s\n", error);
        return 1;
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s/patches.bin", out);
    FILE *f = fopen(path, "wb");
    const int ok = f && fwrite(blob, 1, size, f) == size;
    if (f) fclose(f);
    free(blob);
    if (!ok) {
        fprintf(stderr, "%s: cannot write %s\n", argv[0], path);
        return 1;
    }
    return 0;
}
