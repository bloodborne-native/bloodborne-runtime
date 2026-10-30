/* scripts/patches.py in C (runtime_patches.h): the same patch selection, rules, bytes and
 * messages. Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's image
 * places eboot vaddr 0 at image offset 0. Only literal writes (bytes, bytes16/32/64, float32/64,
 * utf8, utf16). */
#include "runtime_patches.h"
#include "runtime_prepare.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define EBOOT_BASE UINT64_C(0x400000)
static const int OUTPUT_W = 1920, OUTPUT_H = 1080;
static const double PRESET_SCALES[] = {1.0, 1.5, 1.7, 2.0, 3.0};
#define RESOLUTION_TEMPLATE "Resolution Patch 1280x720 (16:9)"
#define INTEL_TONEMAP "Intel Black Tonemap Fix"
#define SKIP_NETWORK_CHOICE "Skip Online/Offline Choice"
#define GX_HEAP "Larger GX Per-Frame Command Heap"
#define DEBUG_MENU "Restore Debug Menu (READ NOTES)"
#define LARGER_HEAP "Increased Graphics Heap Sizes"
/* The scene's and the UI's width and height, in the resolution patch's instructions. */
#define SCENE_WIDTH (UINT64_C(0x02196A6B) - EBOOT_BASE)
#define SCENE_HEIGHT (UINT64_C(0x02196A7A) - EBOOT_BASE)
#define UI_WIDTH (UINT64_C(0x02358554) - EBOOT_BASE)
#define UI_HEIGHT (UINT64_C(0x0235855D) - EBOOT_BASE)

/* The frame-rate presets' patches (BB_FRAME_HOOKS=0 only: above 30 FPS hooks in the game's code
 * set the rate, gpu/shim/bbport_frame_rate.cpp). */
static const struct { const char *fps, *names[2]; } FPS_PRESETS[] = {
    {"30", {NULL, NULL}},
    {"60", {"60 FPS++", NULL}},
    {"90", {"90 FPS++", "Sprint Fix (High FPS)"}},
    {"uncap", {"Uncap FPS++", "Sprint Fix (High FPS)"}},
};
/* Effect switches (bbport.ini): the patch when the key is 0, the patch when it is 1. */
static const struct { const char *key, *off, *on; int hot; } EFFECTS[] = {
    {"effect_chromatic_aberration", "Disable Chromatic Aberration", NULL, 1},
    {"effect_dof", "Disable DoF", NULL, 1},
    {"effect_motion_blur", "Disable Motion Blur (perf increase)", NULL, 1},
    {"effect_ssao", "Disable SSAO", NULL, 1},
    {"effect_game_aa", "Disable AA", NULL, 1},
    {"effect_dynamic_shadows", "Disable Dynamic Light Shadows (perf increase)", NULL, 1},
    {"effect_ssr", NULL, "Enable Screen Space Reflections (READ NOTE)", 0},
    {"effect_bloom", "Disable Bloom", NULL, 1},
    {"skip_intro", NULL, "Skip Intro", 0},
    {"debug_camera", NULL, "Restore Debug Camera", 0},
    {"debug_menu", NULL, DEBUG_MENU, 0},
};
static const struct { const char *value, *name; } MODEL_LOD[] = {
    {"-2", "Model LOD -2 (Highest)"}, {"1", "Model LOD 1 (Lower)"}, {"2", "Model LOD 2 (Lowest)"},
};
/* What the patched game reads for the debug menu, under dvdroot_ps4. */
static const char *const DEBUG_MENU_FONTS[] = {"adhoc/font/DbgFont14h.ccm", "adhoc/font/DbgFont14h.tpf"};
static const char *const DEBUG_MENU_SHADERS[] = {"adhoc/FontShader/debugFont_vs.vpo", "adhoc/FontShader/debugFont_ps.ppo"};
/* The retail releases: one 1.09 executable (scripts/game_check.py). */
static const char *const BLOODBORNE_IDS[] = {"CUSA00900", "CUSA00207", "CUSA00208", "CUSA00299", "CUSA01363",
                                             "CUSA03179", "CUSA03173", "CUSA03014", "CUSA03023"};

#define COUNT(a) (sizeof(a) / sizeof(*(a)))

static int fail(char *error, size_t error_size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
    return -1;
}

static const char *env_or(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return value ? value : fallback;
}

/* A whole file, NUL-terminated; NULL if it cannot be read. */
static char *read_text(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat info;
    if (fstat(fileno(f), &info) || info.st_size < 0) { fclose(f); return NULL; }
    char *text = malloc((size_t)info.st_size + 1);
    const size_t got = text ? fread(text, 1, (size_t)info.st_size, f) : 0;
    fclose(f);
    if (!text) return NULL;
    text[got] = 0;
    if (size) *size = got;
    return text;
}

/* ---- names: a list of strings in order (Python's list) ---- */
typedef struct { char **items; size_t count; } Names;
static int names_add(Names *n, const char *s, size_t length) {
    char **grown = realloc(n->items, (n->count + 1) * sizeof(*grown));
    if (!grown) return -1;
    n->items = grown;
    if (!(n->items[n->count] = strndup(s, length))) return -1;
    ++n->count;
    return 0;
}
static int names_has(const Names *n, const char *s) {
    for (size_t i = 0; i < n->count; ++i)
        if (!strcmp(n->items[i], s)) return 1;
    return 0;
}
static void names_free(Names *n) {
    for (size_t i = 0; i < n->count; ++i) free(n->items[i]);
    free(n->items);
    memset(n, 0, sizeof(*n));
}

/* ---- bbport.ini: key=value lines, '#' comments, later lines win ---- */
typedef struct { char *key, *value; } Setting;
typedef struct { Setting *items; size_t count; } Settings;
static char *strip_copy(const char *s, size_t length) {
    while (length && isspace((unsigned char)*s)) ++s, --length;
    while (length && isspace((unsigned char)s[length - 1])) --length;
    return strndup(s, length);
}
static void settings_read(const char *path, Settings *out) {
    memset(out, 0, sizeof(*out));
    char *text = path ? read_text(path, NULL) : NULL;
    if (!text) return;
    for (char *line = text; *line;) {
        char *end = line + strcspn(line, "\r\n");
        const char *equals = memchr(line, '=', (size_t)(end - line));
        if (equals && line[0] != '#') {
            char *key = strip_copy(line, (size_t)(equals - line)), *value = strip_copy(equals + 1, (size_t)(end - equals - 1));
            size_t i = 0;
            while (i < out->count && strcmp(out->items[i].key, key)) ++i;
            if (i < out->count) {
                free(out->items[i].value), free(key);
                out->items[i].value = value;
            } else {
                Setting *grown = realloc(out->items, (out->count + 1) * sizeof(*grown));
                if (grown) out->items = grown, out->items[out->count++] = (Setting){key, value};
            }
        }
        line = end;
        if (*line == '\r') ++line;
        if (*line == '\n') ++line;
    }
    free(text);
}
static const char *setting(const Settings *s, const char *key) {
    for (size_t i = 0; i < s->count; ++i)
        if (!strcmp(s->items[i].key, key)) return s->items[i].value;
    return NULL;
}
static void settings_free(Settings *s) {
    for (size_t i = 0; i < s->count; ++i) free(s->items[i].key), free(s->items[i].value);
    free(s->items);
}

/* ---- Python's int(text, 0): an optional sign, 0x/0o/0b, '_' between digits ---- */
static int parse_int0(const char *text, int *negative, uint64_t *magnitude) {
    if (!text) return -1;
    while (isspace((unsigned char)*text)) ++text;
    *negative = 0;
    if (*text == '+' || *text == '-') *negative = *text++ == '-';
    int base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) base = 16, text += 2;
    else if (text[0] == '0' && (text[1] == 'o' || text[1] == 'O')) base = 8, text += 2;
    else if (text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) base = 2, text += 2;
    uint64_t value = 0;
    int digits = 0, leading_zero = base == 10 && text[0] == '0', nonzero = 0;
    for (;; ++text) {
        if (*text == '_' && (base != 10 || digits) && text[1] && text[1] != '_') {
            if (base == 10 && !digits) return -1;
            continue;
        }
        int d;
        if (*text >= '0' && *text <= '9') d = *text - '0';
        else if (*text >= 'a' && *text <= 'f') d = *text - 'a' + 10;
        else if (*text >= 'A' && *text <= 'F') d = *text - 'A' + 10;
        else break;
        if (d >= base) return -1;
        if (value > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) return -1;
        value = value * (uint64_t)base + (uint64_t)d;
        nonzero |= d != 0;
        ++digits;
    }
    while (isspace((unsigned char)*text)) ++text;
    if (!digits || *text) return -1;
    if (leading_zero && nonzero) return -1; /* "010": Python refuses it */
    *magnitude = value;
    return 0;
}
static int parse_int(const char *text, long *out) {
    int negative;
    uint64_t magnitude;
    if (parse_int0(text, &negative, &magnitude) || magnitude > (uint64_t)LONG_MAX) return -1;
    *out = negative ? -(long)magnitude : (long)magnitude;
    return 0;
}
/* int(text) in base 10 (bbport.ini's numbers). */
static int parse_decimal(const char *text, long *out) {
    if (!text) return -1;
    while (isspace((unsigned char)*text)) ++text;
    if ((text[0] == '0' && (text[1] == 'x' || text[1] == 'X' || text[1] == 'o' || text[1] == 'b')) ) return -1;
    return parse_int(text, out);
}

/* ---- the patch files: a small XML scanner for Metadata, Line and ID elements ---- */
typedef struct { char *names[24], *values[24]; int count; } Attrs;
typedef struct { Attrs attrs; Attrs *lines; size_t line_count; } Meta;
typedef struct { Meta *metas; size_t count; char **ids; size_t id_count; } PatchFile;

static const char *attr(const Attrs *a, const char *name) {
    for (int i = 0; i < a->count; ++i)
        if (!strcmp(a->names[i], name)) return a->values[i];
    return NULL;
}
static void attrs_free(Attrs *a) {
    for (int i = 0; i < a->count; ++i) free(a->names[i]), free(a->values[i]);
    a->count = 0;
}
static void patch_file_free(PatchFile *f) {
    for (size_t i = 0; i < f->count; ++i) {
        attrs_free(&f->metas[i].attrs);
        for (size_t l = 0; l < f->metas[i].line_count; ++l) attrs_free(&f->metas[i].lines[l]);
        free(f->metas[i].lines);
    }
    free(f->metas);
    for (size_t i = 0; i < f->id_count; ++i) free(f->ids[i]);
    free(f->ids);
    memset(f, 0, sizeof(*f));
}
static void put_utf8(char **out, unsigned long code) {
    char *o = *out;
    if (code < 0x80) *o++ = (char)code;
    else if (code < 0x800) *o++ = (char)(0xC0 | code >> 6), *o++ = (char)(0x80 | (code & 0x3F));
    else if (code < 0x10000) *o++ = (char)(0xE0 | code >> 12), *o++ = (char)(0x80 | ((code >> 6) & 0x3F)), *o++ = (char)(0x80 | (code & 0x3F));
    else *o++ = (char)(0xF0 | code >> 18), *o++ = (char)(0x80 | ((code >> 12) & 0x3F)), *o++ = (char)(0x80 | ((code >> 6) & 0x3F)), *o++ = (char)(0x80 | (code & 0x3F));
    *out = o;
}
/* Character data with its entities decoded; an attribute's whitespace becomes spaces (XML's
 * attribute value normalization, as ElementTree does). NULL for an unknown entity. */
static char *decode(const char *s, size_t length, int attribute) {
    char *out = malloc(length * 4 + 1), *o = out;
    if (!out) return NULL;
    for (size_t i = 0; i < length;) {
        if (s[i] == '&') {
            const char *end = memchr(s + i, ';', length - i);
            if (!end) { free(out); return NULL; }
            const size_t n = (size_t)(end - (s + i)) - 1;
            const char *name = s + i + 1;
            if (n == 3 && !memcmp(name, "amp", 3)) *o++ = '&';
            else if (n == 2 && !memcmp(name, "lt", 2)) *o++ = '<';
            else if (n == 2 && !memcmp(name, "gt", 2)) *o++ = '>';
            else if (n == 4 && !memcmp(name, "quot", 4)) *o++ = '"';
            else if (n == 4 && !memcmp(name, "apos", 4)) *o++ = '\'';
            else if (n > 1 && name[0] == '#') {
                char digits[16];
                if (n > sizeof(digits)) { free(out); return NULL; }
                memcpy(digits, name + 1, n - 1), digits[n - 1] = 0;
                char *rest;
                const unsigned long code = digits[0] == 'x' ? strtoul(digits + 1, &rest, 16) : strtoul(digits, &rest, 10);
                if (*rest || !code || code > 0x10FFFF) { free(out); return NULL; }
                put_utf8(&o, code);
            } else {
                free(out);
                return NULL;
            }
            i += n + 2;
            continue;
        }
        if (s[i] == '\r' && i + 1 < length && s[i + 1] == '\n') { ++i; continue; }
        *o++ = attribute && (s[i] == '\t' || s[i] == '\n' || s[i] == '\r') ? ' ' : s[i];
        ++i;
    }
    *o = 0;
    return out;
}
static int is_name_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == ':'; }

/* Parses TEXT into its Metadata elements (with every Line below each) and its ID texts. */
static int parse_patch_xml(const char *text, PatchFile *out, char *error, size_t error_size) {
    memset(out, 0, sizeof(*out));
    Meta *current = NULL;
    int depth = 0, meta_depth = -1;
    const char *id_text = NULL; /* an open ID element's text start */
    for (const char *p = text; (p = strchr(p, '<'));) {
        if (id_text && p == id_text) id_text = NULL; /* <ID></ID>: no text (ElementTree's None) */
        if (id_text) {
            char *raw = decode(id_text, (size_t)(p - id_text), 0);
            if (!raw) goto malformed;
            char *stripped = strip_copy(raw, strlen(raw));
            free(raw);
            char **grown = realloc(out->ids, (out->id_count + 1) * sizeof(*grown));
            if (!grown || !stripped) { free(stripped); goto malformed; }
            out->ids = grown, out->ids[out->id_count++] = stripped;
            id_text = NULL;
        }
        if (!strncmp(p, "<!--", 4)) {
            const char *end = strstr(p + 4, "-->");
            if (!end) goto malformed;
            p = end + 3;
            continue;
        }
        if (p[1] == '?' || p[1] == '!') {
            const char *end = strstr(p, p[1] == '?' ? "?>" : ">");
            if (!end) goto malformed;
            p = end + (p[1] == '?' ? 2 : 1);
            continue;
        }
        if (p[1] == '/') { /* an end tag */
            const char *name = p + 2, *end = name;
            while (is_name_char(*end)) ++end;
            const char *close = strchr(end, '>');
            if (!close || depth == 0) goto malformed;
            --depth;
            if (depth == meta_depth && (size_t)(end - name) == 8 && !memcmp(name, "Metadata", 8)) current = NULL, meta_depth = -1;
            p = close + 1;
            continue;
        }
        const char *name = p + 1, *q = name;
        while (is_name_char(*q)) ++q;
        if (q == name) goto malformed;
        const size_t name_length = (size_t)(q - name);
        Attrs attrs = {0};
        int self_closing = 0;
        for (;;) {
            while (isspace((unsigned char)*q)) ++q;
            if (*q == '/' && q[1] == '>') { self_closing = 1, q += 2; break; }
            if (*q == '>') { ++q; break; }
            const char *attr_name = q;
            while (is_name_char(*q)) ++q;
            if (q == attr_name || attrs.count == (int)COUNT(attrs.names)) { attrs_free(&attrs); goto malformed; }
            const char *attr_end = q;
            while (isspace((unsigned char)*q)) ++q;
            if (*q++ != '=') { attrs_free(&attrs); goto malformed; }
            while (isspace((unsigned char)*q)) ++q;
            const char quote = *q++;
            if (quote != '"' && quote != '\'') { attrs_free(&attrs); goto malformed; }
            const char *value_end = strchr(q, quote);
            if (!value_end) { attrs_free(&attrs); goto malformed; }
            attrs.names[attrs.count] = strndup(attr_name, (size_t)(attr_end - attr_name));
            attrs.values[attrs.count] = decode(q, (size_t)(value_end - q), 1);
            ++attrs.count;
            if (!attrs.names[attrs.count - 1] || !attrs.values[attrs.count - 1]) { attrs_free(&attrs); goto malformed; }
            q = value_end + 1;
        }
        if (name_length == 8 && !memcmp(name, "Metadata", 8)) {
            Meta *grown = realloc(out->metas, (out->count + 1) * sizeof(*grown));
            if (!grown) { attrs_free(&attrs); goto malformed; }
            out->metas = grown;
            current = &out->metas[out->count++];
            *current = (Meta){attrs, NULL, 0};
            if (!self_closing) meta_depth = depth;
            else current = NULL;
        } else if (name_length == 4 && !memcmp(name, "Line", 4) && current) {
            Attrs *grown = realloc(current->lines, (current->line_count + 1) * sizeof(*grown));
            if (!grown) { attrs_free(&attrs); goto malformed; }
            current->lines = grown, current->lines[current->line_count++] = attrs;
        } else {
            if (name_length == 2 && !memcmp(name, "ID", 2) && !self_closing) id_text = q;
            attrs_free(&attrs);
        }
        if (!self_closing) ++depth;
        p = q;
    }
    if (depth != 0) goto malformed;
    return 0;
malformed:
    patch_file_free(out);
    return fail(error, error_size, "not well-formed (the patch file's XML)");
}

/* ---- a patch line's bytes (patches.py encode) ---- */
typedef struct { uint64_t offset; uint8_t *data; size_t size; } Write;
typedef struct { Write *items; size_t count; } Writes;
static int writes_add(Writes *w, uint64_t offset, uint8_t *data, size_t size) {
    Write *grown = realloc(w->items, (w->count + 1) * sizeof(*grown));
    if (!grown) return -1;
    w->items = grown, w->items[w->count++] = (Write){offset, data, size};
    return 0;
}
static void writes_free(Writes *w) {
    for (size_t i = 0; i < w->count; ++i) free(w->items[i].data);
    free(w->items);
    memset(w, 0, sizeof(*w));
}
/* UTF-8 to UTF-16LE (surrogate pairs above U+FFFF). */
static uint8_t *utf16le(const char *s, size_t *size) {
    const size_t length = strlen(s);
    uint8_t *out = malloc(length * 4 + 2), *o = out;
    if (!out) return NULL;
    for (size_t i = 0; i < length;) {
        unsigned long c = (unsigned char)s[i];
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3F >> extra;
        if (i + (size_t)extra >= length + (extra ? 0 : 1)) { free(out); return NULL; }
        for (int k = 1; k <= extra; ++k) c = c << 6 | ((unsigned char)s[i + (size_t)k] & 0x3F);
        i += (size_t)extra + 1;
        if (c >= 0x10000) {
            c -= 0x10000;
            const unsigned long hi = 0xD800 | c >> 10, lo = 0xDC00 | (c & 0x3FF);
            *o++ = (uint8_t)hi, *o++ = (uint8_t)(hi >> 8), *o++ = (uint8_t)lo, *o++ = (uint8_t)(lo >> 8);
        } else {
            *o++ = (uint8_t)c, *o++ = (uint8_t)(c >> 8);
        }
    }
    *o++ = 0, *o++ = 0;
    *size = (size_t)(o - out);
    return out;
}
static int encode(const Attrs *line, uint8_t **data, size_t *size, char *error, size_t error_size) {
    const char *kind = attr(line, "Type"), *value = attr(line, "Value");
    if (!kind) return fail(error, error_size, "unsupported patch type None");
    if (!value) return fail(error, error_size, "a %s line without a value", kind);
    if (!strcmp(kind, "bytes")) {
        uint8_t *out = malloc(strlen(value) / 2 + 1);
        size_t n = 0;
        if (!out) return fail(error, error_size, "out of memory");
        for (const char *p = value;;) {
            while (*p == ' ' || isspace((unsigned char)*p)) ++p;
            if (!*p) break;
            if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) {
                free(out);
                return fail(error, error_size, "non-hexadecimal number found in fromhex() arg");
            }
            char pair[3] = {p[0], p[1], 0};
            out[n++] = (uint8_t)strtoul(pair, NULL, 16);
            p += 2;
        }
        *data = out, *size = n;
        return 0;
    }
    if (!strcmp(kind, "bytes16") || !strcmp(kind, "bytes32") || !strcmp(kind, "bytes64")) {
        const size_t width = (size_t)atoi(kind + 5) / 8;
        int negative;
        uint64_t magnitude;
        if (parse_int0(value, &negative, &magnitude)) return fail(error, error_size, "invalid literal for int() with base 0: '%s'", value);
        if (negative || (width < 8 && magnitude >> (width * 8))) return fail(error, error_size, "int too big to convert");
        uint8_t *out = malloc(width);
        if (!out) return fail(error, error_size, "out of memory");
        for (size_t i = 0; i < width; ++i) out[i] = (uint8_t)(magnitude >> (8 * i));
        *data = out, *size = width;
        return 0;
    }
    if (!strcmp(kind, "float32") || !strcmp(kind, "float64")) {
        char *rest;
        const double d = strtod(value, &rest);
        while (isspace((unsigned char)*rest)) ++rest;
        if (rest == value || *rest) return fail(error, error_size, "could not convert string to float: '%s'", value);
        const size_t width = kind[5] == '3' ? 4 : 8;
        uint8_t *out = malloc(width);
        if (!out) return fail(error, error_size, "out of memory");
        if (width == 4) {
            const float f = (float)d;
            memcpy(out, &f, 4);
        } else {
            memcpy(out, &d, 8);
        }
        *data = out, *size = width;
        return 0;
    }
    if (!strcmp(kind, "utf8")) {
        const size_t n = strlen(value) + 1;
        uint8_t *out = malloc(n);
        if (!out) return fail(error, error_size, "out of memory");
        memcpy(out, value, n);
        *data = out, *size = n;
        return 0;
    }
    if (!strcmp(kind, "utf16")) {
        if (!(*data = utf16le(value, size))) return fail(error, error_size, "invalid UTF-8 in a utf16 line");
        return 0;
    }
    return fail(error, error_size, "unsupported patch type '%s'", kind);
}
static int in_segments(uint64_t offset, size_t size, uint64_t (*segments)[2], size_t segment_count) {
    for (size_t i = 0; i < segment_count; ++i)
        if (segments[i][0] <= offset && offset + size <= segments[i][1]) return 1;
    return 0;
}
/* The lines of META as writes (patches.py's loop body of compile_patches and compile_external). */
static int meta_writes(const Meta *meta, uint64_t (*segments)[2], size_t segment_count, Writes *out, const char *who,
                       char *error, size_t error_size) {
    for (size_t l = 0; l < meta->line_count; ++l) {
        const char *address = attr(&meta->lines[l], "Address");
        int negative;
        uint64_t magnitude;
        if (parse_int0(address, &negative, &magnitude) || negative)
            return fail(error, error_size, "invalid literal for int() with base 0: '%s'", address ? address : "");
        const uint64_t offset = magnitude - EBOOT_BASE;
        uint8_t *data;
        size_t size;
        if (encode(&meta->lines[l], &data, &size, error, error_size)) return -1;
        if (magnitude < EBOOT_BASE || !in_segments(offset, size, segments, segment_count)) {
            free(data);
            return who ? fail(error, error_size, "%s: address %s is outside the eboot", who, address)
                       : fail(error, error_size, "address %s is outside the eboot", address);
        }
        if (writes_add(out, offset, data, size)) { free(data); return fail(error, error_size, "out of memory"); }
    }
    return 0;
}
/* Python's repr of a list of names: ['a', 'b']. */
static void print_names(FILE *f, const Names *names) {
    fputc('[', f);
    for (size_t i = 0; i < names->count; ++i) fprintf(f, "%s'%s'", i ? ", " : "", names->items[i]);
    fputc(']', f);
}
static int compile_patches(const PatchFile *xml, const Names *names, const char *app_version,
                           uint64_t (*segments)[2], size_t segment_count, Writes *out, char *error, size_t error_size) {
    const Meta **found = calloc(names->count ? names->count : 1, sizeof(*found));
    if (!found) return fail(error, error_size, "out of memory");
    for (size_t m = 0; m < xml->count; ++m) {
        const Attrs *a = &xml->metas[m].attrs;
        const char *name = attr(a, "Name"), *version = attr(a, "AppVer"), *elf = attr(a, "AppElf");
        if (!name || !version || strcmp(version, app_version) || strcmp(elf ? elf : "eboot.bin", "eboot.bin")) continue;
        for (size_t n = 0; n < names->count; ++n)
            if (!strcmp(names->items[n], name)) found[n] = &xml->metas[m];
    }
    Names missing = {0};
    for (size_t n = 0; n < names->count; ++n)
        if (!found[n]) names_add(&missing, names->items[n], strlen(names->items[n]));
    if (missing.count) {
        char list[2048] = "";
        for (size_t i = 0; i < missing.count; ++i)
            snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s'%s'", i ? ", " : "", missing.items[i]);
        names_free(&missing);
        free(found);
        return fail(error, error_size, "patches not found for app version %s: [%s]", app_version, list);
    }
    for (size_t n = 0; n < names->count; ++n)
        if (meta_writes(found[n], segments, segment_count, out, names->items[n], error, error_size)) { free(found); return -1; }
    free(found);
    return 0;
}

/* ---- the resolution writes: the template's scene and UI sizes replaced ---- */
static int resolution_writes(const PatchFile *xml, int width, int height, const char *app_version,
                             uint64_t (*segments)[2], size_t segment_count, Writes *out, char *error, size_t error_size) {
    static const struct { uint64_t offset; uint8_t opcode; uint32_t old; int which; } SLOTS[] = {
        {SCENE_WIDTH, 0xB8, 0x500, 0}, {SCENE_HEIGHT, 0xB8, 0x2D0, 1}, {UI_WIDTH, 0xB8, 0x500, 2}, {UI_HEIGHT, 0xB9, 0x2D0, 3},
    };
    const int values[4] = {width, height, OUTPUT_W, OUTPUT_H};
    Names template = {0};
    if (names_add(&template, RESOLUTION_TEMPLATE, strlen(RESOLUTION_TEMPLATE))) return fail(error, error_size, "out of memory");
    const size_t first = out->count;
    const int rc = compile_patches(xml, &template, app_version, segments, segment_count, out, error, error_size);
    names_free(&template);
    if (rc) return -1;
    int seen[4] = {0};
    for (size_t w = first; w < out->count; ++w)
        for (size_t s = 0; s < COUNT(SLOTS); ++s) {
            if (out->items[w].offset != SLOTS[s].offset) continue;
            const uint8_t expected[4] = {SLOTS[s].opcode, (uint8_t)SLOTS[s].old, (uint8_t)(SLOTS[s].old >> 8), (uint8_t)(SLOTS[s].old >> 16)};
            if (out->items[w].size != 4 || memcmp(out->items[w].data, expected, 4) || seen[s])
                return fail(error, error_size, "unexpected resolution patch at %#llx", (unsigned long long)(SLOTS[s].offset + EBOOT_BASE));
            const uint32_t v = (uint32_t)values[SLOTS[s].which];
            out->items[w].data[1] = (uint8_t)v, out->items[w].data[2] = (uint8_t)(v >> 8), out->items[w].data[3] = (uint8_t)(v >> 16);
            seen[s] = 1;
        }
    for (size_t s = 0; s < COUNT(SLOTS); ++s)
        if (!seen[s]) return fail(error, error_size, "resolution patch is missing scene/UI viewport instructions");
    return 0;
}

/* ---- sizes ---- */
static int parse_size(const char *text, int *width, int *height) {
    if (!text) return -1;
    char lower[64];
    size_t n = 0;
    for (; text[n] && n + 1 < sizeof(lower); ++n) lower[n] = (char)tolower((unsigned char)text[n]);
    lower[n] = 0;
    char *x = strchr(lower, 'x');
    if (!x || strchr(x + 1, 'x')) return -1;
    *x = 0;
    long w, h;
    if (parse_decimal(lower, &w) || parse_decimal(x + 1, &h)) return -1;
    *width = (int)w, *height = (int)h;
    return 0;
}
static double preset_scale(const Settings *s) {
    long preset = 0;
    const char *value = setting(s, "preset");
    if (value && *value && parse_decimal(value, &preset)) preset = 0;
    if (preset < 0) preset = 0;
    if (preset > (long)COUNT(PRESET_SCALES) - 1) preset = (long)COUNT(PRESET_SCALES) - 1;
    return PRESET_SCALES[preset];
}
/* round(v / scale / 2) * 2, at least 2: Python's round halves to even, as nearbyint does. */
static int even_size(int v, double scale) {
    const int r = (int)nearbyint((double)v / scale / 2) * 2;
    return r > 2 ? r : 2;
}
int bb_patches_scaled_sizes(const char *settings_path, int render[2], int output[2]) {
    Settings s;
    settings_read(settings_path, &s);
    int out_w = OUTPUT_W, out_h = OUTPUT_H, w, h;
    if (!parse_size(setting(&s, "output_res"), &w, &h) && w > 0 && h > 0) out_w = w, out_h = h;
    const char *upscaler = setting(&s, "upscaler");
    int scaled = 0;
    if (!(upscaler && !strcmp(upscaler, "taa"))) {
        const double scale = upscaler && strcmp(upscaler, "off") ? preset_scale(&s) : 1.0;
        if (!(out_w == OUTPUT_W && out_h == OUTPUT_H && scale == 1.0)) {
            render[0] = even_size(out_w, scale), render[1] = even_size(out_h, scale);
            /* A scene of exactly 1920x1080 is the game's UI coordinate space to the port. */
            if (render[0] == OUTPUT_W && render[1] == OUTPUT_H) render[0] = 1916, render[1] = 1078;
            output[0] = out_w, output[1] = out_h;
            scaled = 1;
        }
    }
    settings_free(&s);
    return scaled;
}

/* ---- the debug menu's files (patches.py find_ignoring_case, debug_menu_problem) ---- */
static int find_ignoring_case(const char *root, const char *relative, char *found, size_t found_size) {
    char path[4096];
    snprintf(path, sizeof(path), "%s", root);
    char parts[1024];
    snprintf(parts, sizeof(parts), "%s", relative);
    for (char *save = NULL, *part = strtok_r(parts, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        char exact[4096];
        struct stat st;
        snprintf(exact, sizeof(exact), "%s/%s", path, part);
        if (!stat(exact, &st)) { snprintf(path, sizeof(path), "%s", exact); continue; }
        DIR *dir = opendir(path);
        if (!dir) return 0;
        struct dirent *e;
        int matched = 0;
        while ((e = readdir(dir)))
            if (!strcasecmp(e->d_name, part)) {
                char next[4096];
                snprintf(next, sizeof(next), "%s/%s", path, e->d_name);
                snprintf(path, sizeof(path), "%s", next);
                matched = 1;
                break;
            }
        closedir(dir);
        if (!matched) return 0;
    }
    snprintf(found, found_size, "%s", path);
    return 1;
}
static int debug_menu_problem(const char *game_dir, char *problem, size_t problem_size) {
    char dvdroot[4096], found[4096], missing[512] = "";
    snprintf(dvdroot, sizeof(dvdroot), "%s/dvdroot_ps4", game_dir);
    int all_in_font = 1, any = 0;
    for (size_t i = 0; i < COUNT(DEBUG_MENU_FONTS); ++i) {
        struct stat st;
        if (find_ignoring_case(dvdroot, DEBUG_MENU_FONTS[i], found, sizeof(found)) && !stat(found, &st) && S_ISREG(st.st_mode) && st.st_size)
            continue;
        snprintf(missing + strlen(missing), sizeof(missing) - strlen(missing), "%s%s", any ? ", " : "", DEBUG_MENU_FONTS[i]);
        any = 1;
        char in_font[256];
        snprintf(in_font, sizeof(in_font), "font/%s", strrchr(DEBUG_MENU_FONTS[i], '/') + 1);
        if (!find_ignoring_case(dvdroot, in_font, found, sizeof(found))) all_in_font = 0;
    }
    if (!any) return 0;
    snprintf(problem, problem_size,
             "the debug menu needs the font files from https://www.nexusmods.com/bloodborne/mods/253 in dvdroot_ps4 of "
             "the game folder or of a mod: missing %s%s",
             missing, all_in_font ? " (they are in dvdroot_ps4/font: the game reads them from dvdroot_ps4/adhoc/font)" : "");
    return 1;
}

/* ---- the player's patch files and patches.json ---- */
/* The strings of the JSON array under KEY in TEXT (patches.json's "enabled" and "disabled"). */
static void json_strings(const char *text, const char *key, Names *out) {
    char quoted[64];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    const char *p = text ? strstr(text, quoted) : NULL;
    if (!p) return;
    p = strchr(p + strlen(quoted), '[');
    if (!p) return;
    for (++p; *p && *p != ']';) {
        if (*p != '"') { ++p; continue; }
        char value[1024];
        size_t n = 0;
        for (++p; *p && *p != '"'; ++p) {
            char c = *p;
            if (c == '\\' && p[1]) {
                ++p;
                c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
                if (*p == 'u' && isxdigit((unsigned char)p[1])) {
                    char hex[5] = {0};
                    memcpy(hex, p + 1, 4);
                    char utf8[4], *o = utf8;
                    put_utf8(&o, strtoul(hex, NULL, 16));
                    for (char *c8 = utf8; c8 < o && n + 1 < sizeof(value); ++c8) value[n++] = *c8;
                    p += 4;
                    continue;
                }
            }
            if (n + 1 < sizeof(value)) value[n++] = c;
        }
        if (*p == '"') ++p;
        names_add(out, value, n);
    }
}
static int compare_strings(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int external_patches(const char *directory, const char *app_version, const char *builtin, const char *config,
                            uint64_t (*segments)[2], size_t segment_count, Writes *out) {
    DIR *dir = opendir(directory);
    if (!dir) return 0;
    Names files = {0};
    struct dirent *e;
    while ((e = readdir(dir))) {
        const size_t n = strlen(e->d_name);
        if (n > 4 && !strcmp(e->d_name + n - 4, ".xml")) names_add(&files, e->d_name, n);
    }
    closedir(dir);
    qsort(files.items, files.count, sizeof(*files.items), compare_strings);
    char *config_text = config ? read_text(config, NULL) : NULL;
    Names enabled = {0}, disabled = {0};
    json_strings(config_text, "enabled", &enabled);
    json_strings(config_text, "disabled", &disabled);
    free(config_text);
    char builtin_real[PATH_MAX] = "";
    if (builtin && !realpath(builtin, builtin_real)) builtin_real[0] = 0;
    for (size_t f = 0; f < files.count; ++f) {
        char path[4096], real[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", directory, files.items[f]);
        if (builtin_real[0] && realpath(path, real) && !strcmp(real, builtin_real)) continue;
        struct stat st;
        if (stat(path, &st) || !S_ISREG(st.st_mode)) continue;
        char *text = read_text(path, NULL), error[512];
        PatchFile file;
        if (!text || parse_patch_xml(text, &file, error, sizeof(error))) {
            fprintf(stderr, "Patches: %s: %s\n", files.items[f], text ? error : "cannot be read");
            free(text);
            continue;
        }
        free(text);
        int bloodborne = !file.id_count;
        for (size_t i = 0; i < file.id_count && !bloodborne; ++i)
            for (size_t k = 0; k < COUNT(BLOODBORNE_IDS); ++k)
                if (!strcmp(file.ids[i], BLOODBORNE_IDS[k])) bloodborne = 1;
        for (size_t m = 0; bloodborne && m < file.count; ++m) {
            const Attrs *a = &file.metas[m].attrs;
            const char *version = attr(a, "AppVer"), *elf = attr(a, "AppElf"), *name = attr(a, "Name");
            if (!version || strcmp(version, app_version) || strcmp(elf ? elf : "eboot.bin", "eboot.bin")) continue;
            char key[1024];
            snprintf(key, sizeof(key), "%s/%s", files.items[f], name ? name : "None");
            const char *flag = attr(a, "isEnabled");
            const int on = names_has(&enabled, key) || (!names_has(&disabled, key) && flag && !strcasecmp(flag, "true"));
            if (!on) continue;
            Writes ours = {0};
            if (meta_writes(&file.metas[m], segments, segment_count, &ours, NULL, error, sizeof(error))) {
                fprintf(stderr, "Patches: skipped %s: %s\n", key, error);
                writes_free(&ours);
                continue;
            }
            for (size_t w = 0; w < ours.count; ++w) writes_add(out, ours.items[w].offset, ours.items[w].data, ours.items[w].size);
            printf("Patches: external %s (%zu writes)\n", key, ours.count);
            free(ours.items);
        }
        patch_file_free(&file);
    }
    names_free(&files), names_free(&enabled), names_free(&disabled);
    return 0;
}

static int intel_cpu(void) {
#ifdef __APPLE__
    return 1; /* as patches.py: macOS reports the game's x86-64 process as GenuineIntel */
#else
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return 0;
    char line[512];
    int intel = 0;
    while (!intel && fgets(line, sizeof(line), f)) intel = !strncmp(line, "vendor_id", 9) && strstr(line, "GenuineIntel");
    fclose(f);
    return intel;
#endif
}

int bb_patches_build(const BbPatchOptions *o, uint8_t **blob, size_t *blob_size, char *error, size_t error_size) {
    const char *fps = o->fps ? o->fps : "uncap", *app_version = o->app_version ? o->app_version : "01.09";
    Names names = {0};
    Settings settings;
    settings_read(o->settings, &settings);
    PatchFile xml = {0};
    Writes writes = {0};
    char *xml_text = NULL;
    int rc = -1;
    /* The frame-rate preset's patches, only with BB_FRAME_HOOKS=0 above 30 FPS. */
    size_t preset = 0;
    while (preset < COUNT(FPS_PRESETS) && strcmp(FPS_PRESETS[preset].fps, fps)) ++preset;
    if (preset == COUNT(FPS_PRESETS)) { fail(error, error_size, "unknown FPS preset %s", fps); goto done; }
    if (!strcmp(fps, "30") || !strcmp(env_or("BB_FRAME_HOOKS", "1"), "0"))
        for (int i = 0; i < 2; ++i)
            if (FPS_PRESETS[preset].names[i]) names_add(&names, FPS_PRESETS[preset].names[i], strlen(FPS_PRESETS[preset].names[i]));
    for (const char *p = o->extra ? o->extra : ""; *p;) {
        const char *end = p + strcspn(p, ";");
        char *name = strip_copy(p, (size_t)(end - p));
        if (name && *name) names_add(&names, name, strlen(name));
        free(name);
        p = *end ? end + 1 : end;
    }
    /* Effect switches; the ones the runtime switches live are no start patches. */
    const int hot = strcmp(env_or("BB_HOT_EFFECTS", "1"), "0") && strcmp(env_or("BB_GAME_HOOKS", "1"), "0");
    for (size_t e = 0; e < COUNT(EFFECTS); ++e) {
        const char *value = setting(&settings, EFFECTS[e].key);
        if (!value || (hot && EFFECTS[e].hot)) continue;
        const char *name = !strcmp(value, "1") ? EFFECTS[e].on : EFFECTS[e].off;
        if (name && !names_has(&names, name)) names_add(&names, name, strlen(name));
    }
    const char *lod = setting(&settings, "model_lod");
    for (size_t l = 0; l < COUNT(MODEL_LOD); ++l)
        if (!strcmp(lod ? lod : "0", MODEL_LOD[l].value) && !names_has(&names, MODEL_LOD[l].name))
            names_add(&names, MODEL_LOD[l].name, strlen(MODEL_LOD[l].name));
    const char *forced = getenv("BB_INTEL_TONEMAP_FIX");
    const int tonemap = forced && (!strcmp(forced, "0") || !strcmp(forced, "1")) ? !strcmp(forced, "1") : intel_cpu();
    if (tonemap && !names_has(&names, INTEL_TONEMAP)) names_add(&names, INTEL_TONEMAP, strlen(INTEL_TONEMAP));
    if (strcmp(env_or("BB_SKIP_NETWORK_CHOICE", "1"), "0") && !names_has(&names, SKIP_NETWORK_CHOICE))
        names_add(&names, SKIP_NETWORK_CHOICE, strlen(SKIP_NETWORK_CHOICE));
    if (strcmp(env_or("BB_GX_HEAP", "1"), "0") && !names_has(&names, GX_HEAP)) names_add(&names, GX_HEAP, strlen(GX_HEAP));
    /* Conflicts, and the debug menu only with its font files (the game crashes without them). */
    if (names_has(&names, "Restore Debug Camera") && names_has(&names, "Enemy Control")) {
        fail(error, error_size, "Restore Debug Camera conflicts with Enemy Control; enable only one");
        goto done;
    }
    if (names_has(&names, DEBUG_MENU)) {
        char problem[1024];
        if (debug_menu_problem(o->game_dir, problem, sizeof(problem))) {
            fprintf(stderr, "Patches: debug menu off: %s\n", problem);
            Names kept = {0};
            for (size_t i = 0; i < names.count; ++i)
                if (strcmp(names.items[i], DEBUG_MENU)) names_add(&kept, names.items[i], strlen(names.items[i]));
            names_free(&names);
            names = kept;
        } else {
            char dvdroot[4096], found[4096], absent[512] = "";
            snprintf(dvdroot, sizeof(dvdroot), "%s/dvdroot_ps4", o->game_dir);
            for (size_t i = 0; i < COUNT(DEBUG_MENU_SHADERS); ++i)
                if (!find_ignoring_case(dvdroot, DEBUG_MENU_SHADERS[i], found, sizeof(found)))
                    snprintf(absent + strlen(absent), sizeof(absent) - strlen(absent), "%s%s", *absent ? ", " : "", DEBUG_MENU_SHADERS[i]);
            if (*absent)
                fprintf(stderr, "Patches: debug menu: %s not found (the mod's font shaders); the menu may not draw\n", absent);
        }
    }
    /* The executable's segments from the game folder (mods never replace eboot.bin). */
    uint64_t segments[64][2];
    size_t segment_count = 0;
    if (bb_eboot_segments(o->game_dir, segments, COUNT(segments), &segment_count, error, error_size)) goto done;
    if (!(xml_text = read_text(o->xml, NULL))) { fail(error, error_size, "cannot read %s", o->xml); goto done; }
    if (parse_patch_xml(xml_text, &xml, error, error_size)) goto done;
    if (compile_patches(&xml, &names, app_version, segments, segment_count, &writes, error, error_size)) goto done;
    int width = 0, height = 0;
    if (o->render_res && *o->render_res) {
        if (parse_size(o->render_res, &width, &height)) { fail(error, error_size, "invalid render size %s", o->render_res); goto done; }
        if (width == OUTPUT_W && height == OUTPUT_H) width = height = 0; /* the game's own size: no writes */
    }
    /* The UI keeps the game's 1920x1080 coordinates even for a larger output (UiComposition). */
    if (width) {
        if (resolution_writes(&xml, width, height, app_version, segments, segment_count, &writes, error, error_size)) goto done;
        if ((long)width * height > (long)OUTPUT_W * OUTPUT_H) {
            Names heap = {0};
            names_add(&heap, LARGER_HEAP, strlen(LARGER_HEAP));
            const int heap_rc = compile_patches(&xml, &heap, app_version, segments, segment_count, &writes, error, error_size);
            names_free(&heap);
            if (heap_rc) goto done;
            names_add(&names, LARGER_HEAP, strlen(LARGER_HEAP));
        }
        printf("Patches: scene %dx%d; UI %dx%d\n", width, height, OUTPUT_W, OUTPUT_H);
    }
    /* After the built-in ones: an external patch of the same bytes wins. */
    if (o->patches_dir && *o->patches_dir)
        external_patches(o->patches_dir, app_version, o->xml, o->patches_config, segments, segment_count, &writes);
    /* BBPATCH2: the patch base (the loader rebases pointers the patches write), the writes. */
    size_t size = 24;
    for (size_t w = 0; w < writes.count; ++w) size += 16 + writes.items[w].size;
    uint8_t *out = malloc(size), *p = out;
    if (!out) { fail(error, error_size, "out of memory"); goto done; }
    const uint64_t header[2] = {EBOOT_BASE, writes.count};
    memcpy(p, "BBPATCH2", 8), memcpy(p + 8, header, 16), p += 24;
    for (size_t w = 0; w < writes.count; ++w) {
        const uint64_t entry[2] = {writes.items[w].offset, writes.items[w].size};
        memcpy(p, entry, 16), memcpy(p + 16, writes.items[w].data, writes.items[w].size);
        p += 16 + writes.items[w].size;
    }
    *blob = out, *blob_size = size;
    printf("Patches: FPS preset %s; %zu writes from ", fps, writes.count);
    if (names.count) print_names(stdout, &names);
    else printf("none");
    printf("\n");
    rc = 0;
done:
    names_free(&names);
    settings_free(&settings);
    patch_file_free(&xml);
    writes_free(&writes);
    free(xml_text);
    return rc;
}
