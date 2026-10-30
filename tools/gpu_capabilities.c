/* Check whether native-size depth/stencil images can be blitted to reduced
 * renderer targets. The renderer needs both directions for live presets.
 * --gamepads: the connected gamepads, "GUID<tab>name" per line (the launcher's controller list,
 * BB_GAMEPAD). --displays: the monitors, "name<tab>WxH<tab>primary (1 or 0)" per line in SDL's
 * order (the launcher's monitor list, BB_DISPLAY; issue #69). --read-input: one key or button for the
 * launcher's controls (below). --device: "vendorID<tab>name" of the GPU the game takes (run.sh:
 * memory model and driver workarounds by vendor and chip). --fsr411 DIR, --fsr411-lossy DIR,
 * --fsr412 DIR: "1" when that FSR 4.1.x mode can run (an Apple Silicon GPU, the files the
 * runtime's build put in DIR, AMD's initializer with the right SHA-256 in BB_FSR411_DIR /
 * BB_DATA_DIR/fsr4_411 for 4.1 and 4.1 Lossy, or BB_FSR412_DIR / BB_DATA_DIR/fsr4_412 for
 * 4.1.2), else "0<tab>reason" (the launcher's upscaler list; the runtime checks the same in
 * vk_fsr4_metal.mm). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __APPLE__
#include <vulkan/vulkan.h>
#endif
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <sys/sysctl.h>
#endif

#ifndef __APPLE__
static VkDeviceSize largest_local_heap(VkPhysicalDevice device) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(device, &memory);
    VkDeviceSize largest = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            memory.memoryHeaps[i].size > largest) {
            largest = memory.memoryHeaps[i].size;
        }
    }
    return largest;
}

static int better_device(VkPhysicalDevice candidate, VkPhysicalDevice current) {
    VkPhysicalDeviceProperties next, old;
    vkGetPhysicalDeviceProperties(candidate, &next);
    vkGetPhysicalDeviceProperties(current, &old);
    const int next_api = next.apiVersion >= VK_API_VERSION_1_3;
    const int old_api = old.apiVersion >= VK_API_VERSION_1_3;
    if (next_api != old_api) return next_api;
    const int next_discrete = next.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    const int old_discrete = old.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    if (next_discrete != old_discrete) return next_discrete;
    const int next_cpu = next.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    const int old_cpu = old.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    if (next_cpu != old_cpu) return !next_cpu;
    return largest_local_heap(candidate) > largest_local_heap(current);
}

/* --live-resolution: prints 1 when live resolution changes suit the GPU (run.sh, setting
 * live_resolution=auto), else 0. Live scaling keeps the game's post-processing at 1080p and
 * copies scene targets every frame: fine on a strong discrete GPU, 7-8 FPS on the Steam Deck
 * and a GTX 1060. Rule: discrete, at least 8 GB of device memory, and not an NVIDIA GPU older
 * than Turing (no fragment shader barycentrics; its depth/stencil copies take nine draws). */
static int has_extension(VkPhysicalDevice device, const char *name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL) != VK_SUCCESS) return 0;
    VkExtensionProperties *list = calloc(count ? count : 1, sizeof(*list));
    int found = 0;
    if (list && vkEnumerateDeviceExtensionProperties(device, NULL, &count, list) == VK_SUCCESS)
        for (uint32_t i = 0; i < count && !found; ++i) found = !strcmp(list[i].extensionName, name);
    free(list);
    return found;
}

static int live_resolution_suits(VkPhysicalDevice device) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    const int discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    const VkDeviceSize memory = largest_local_heap(device);
    const int old_nvidia = props.vendorID == 0x10de &&
                           !has_extension(device, "VK_KHR_fragment_shader_barycentric");
    const int suits = discrete && memory >= (VkDeviceSize)7680 << 20 && !old_nvidia;
    fprintf(stderr, "GPU: %s, %s, %llu MiB: live resolution changes %s\n", props.deviceName,
            discrete ? "discrete" : "integrated or other", (unsigned long long)(memory >> 20),
            suits ? "on" : "off (startup resolution patch)");
    return suits;
}
#endif

static int list_gamepads(void) {
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "gamepads: %s\n", SDL_GetError());
        return 1;
    }
    // Some devices (HIDAPI) show up only after events are pumped.
    for (int i = 0; i < 5; ++i) {
        SDL_PumpEvents();
        SDL_Delay(40);
    }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        char guid[33];
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), guid, sizeof guid);
        const char *name = SDL_GetGamepadNameForID(ids[i]);
        printf("%s\t%s\n", guid, name ? name : "?");
    }
    SDL_free(ids);
    SDL_Quit();
    return 0;
}

static int list_displays(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "displays: %s\n", SDL_GetError());
        return 1;
    }
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    int count = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&count);
    for (int i = 0; ids && i < count; ++i) {
        const char *name = SDL_GetDisplayName(ids[i]);
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(ids[i]);
        printf("%s\t%dx%d\t%d\n", name && *name ? name : "?", mode ? mode->w : 0,
               mode ? mode->h : 0, ids[i] == primary);
    }
    SDL_free(ids);
    SDL_Quit();
    return 0;
}

/* --read-input key|pad: a small window; prints names accepted by bbport.ini's key.* and pad.*
 * lines. A held Shift/Ctrl/Alt plus a key or mouse button is one chord. Escape, closing the
 * window or waiting 15 s cancels. */
static int modifier_key(SDL_Scancode code) {
    return code == SDL_SCANCODE_LSHIFT || code == SDL_SCANCODE_RSHIFT ||
           code == SDL_SCANCODE_LCTRL || code == SDL_SCANCODE_RCTRL ||
           code == SDL_SCANCODE_LALT || code == SDL_SCANCODE_RALT;
}

static const char *held_modifier(SDL_Scancode key) {
    const bool *down = SDL_GetKeyboardState(NULL);
    const SDL_Scancode mods[] = {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT,
                                 SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL,
                                 SDL_SCANCODE_LALT, SDL_SCANCODE_RALT};
    for (size_t i = 0; i < sizeof(mods) / sizeof(*mods); ++i) {
        if (mods[i] != key && down[mods[i]]) return SDL_GetScancodeName(mods[i]);
    }
    return NULL;
}

static void print_key_binding(const char *name, SDL_Scancode key) {
    const char *mod = held_modifier(key);
    printf("key %s%s%s\n", mod ? mod : "", mod ? "+" : "", name);
}

static int read_input(const char *kind) {
    const int want_key = strcmp(kind, "pad") != 0, want_pad = strcmp(kind, "key") != 0;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "read-input: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    const char *prompt = want_key && want_pad ? "Press a key, a mouse button or a gamepad button"
                         : want_key           ? "Press a key or a mouse button"
                                              : "Press a gamepad button";
    if (!SDL_CreateWindowAndRenderer("bbport", 520, 90, 0, &window, &renderer)) {
        fprintf(stderr, "read-input: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
    const Uint64 end = SDL_GetTicks() + 15000;
    int done = 0;
    SDL_Scancode pending_modifier = SDL_SCANCODE_UNKNOWN;
    while (!done && SDL_GetTicks() < end) {
        SDL_SetRenderDrawColor(renderer, 24, 24, 28, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
        SDL_RenderDebugText(renderer, 16, 30, prompt);
        SDL_RenderDebugText(renderer, 16, 50, "Escape: cancel");
        SDL_RenderPresent(renderer);
        SDL_Event e;
        while (!done && SDL_WaitEventTimeout(&e, 50)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                done = 1;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                SDL_OpenGamepad(e.gdevice.which);
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                    done = 1;
                } else if (want_key) {
                    if (modifier_key(e.key.scancode)) {
                        pending_modifier = e.key.scancode;
                    } else {
                        print_key_binding(SDL_GetScancodeName(e.key.scancode), e.key.scancode);
                        done = 1;
                    }
                }
                break;
            case SDL_EVENT_KEY_UP:
                if (want_key && e.key.scancode == pending_modifier) {
                    printf("key %s\n", SDL_GetScancodeName(pending_modifier));
                    done = 1;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: /* the names runtime_pad.c's key.* lines take */
                if (want_key && e.button.button >= SDL_BUTTON_LEFT && e.button.button <= SDL_BUTTON_X2) {
                    static const char *const names[] = {"", "Mouse Left", "Mouse Middle", "Mouse Right",
                                                        "Mouse X1", "Mouse X2"};
                    print_key_binding(names[e.button.button], SDL_SCANCODE_UNKNOWN);
                    done = 1;
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (want_key && e.wheel.y != 0) {
                    const float y = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
                    print_key_binding(y > 0 ? "Wheel Up" : "Wheel Down", SDL_SCANCODE_UNKNOWN);
                    done = 1;
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (want_pad) {
                    printf("pad %s\n", SDL_GetGamepadStringForButton((SDL_GamepadButton)e.gbutton.button));
                    done = 1;
                }
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (want_pad && e.gaxis.value > 16000 &&
                    (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
                    printf("pad %s\n", e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "lefttrigger" : "righttrigger");
                    done = 1;
                }
                break;
            default:
                break;
            }
        }
    }
    fflush(stdout);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

#ifdef __APPLE__
static int read_file(const char *path, unsigned char *buffer, size_t capacity, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    *length = fread(buffer, 1, capacity, file);
    const int whole = fgetc(file) == EOF;
    fclose(file);
    return whole;
}

/* The initializer is accepted with the SHA-256 the native manifest names, as by the runtime. */
static int initializer_ok(const char *dir, const char *expected, int lite) {
    const char *names[3];
    size_t count = 0;
    names[count++] = lite ? "/initializers-lite.bin" : "/initializers.bin";
    if (!lite) {
        names[count++] = "/t1080_m0/initializer.bin";
        names[count++] = "/t2160_m0/initializer.bin";
    }
    const size_t wanted = lite ? 27392 : 131072;
    static unsigned char data[131073];
    for (size_t i = 0; i < count; ++i) {
        char path[4096];
        size_t length = 0;
        snprintf(path, sizeof path, "%s%s", dir, names[i]);
        if (!read_file(path, data, sizeof data, &length) || length != wanted) continue;
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
        CC_SHA256(data, (CC_LONG)length, digest);
        for (int j = 0; j < CC_SHA256_DIGEST_LENGTH; ++j) snprintf(hex + 2 * j, 3, "%02x", digest[j]);
        if (!strcmp(hex, expected)) return 1;
    }
    return 0;
}

enum FsrMode { Fsr41 = 0, Fsr41Lossy = 1, Fsr412 = 2 };

static int fsr_support(const char *assets, int mode) {
    int arm64 = 0;
    size_t size = sizeof arm64;
    if (sysctlbyname("hw.optional.arm64", &arm64, &size, NULL, 0) || !arm64) {
        printf("0\tneeds an Apple M1 or newer GPU\n");
        return 0;
    }
    const char *name = mode == Fsr412 ? "4.1.2" : mode == Fsr41Lossy ? "4.1 Lossy" : "4.1";
    const char *variant = mode == Fsr412
                              ? "/fsr412_1080_di0_lm1_jm0_cs0_ae1_o1_up0_pc1_v2/bindings.json"
                          : mode == Fsr41Lossy
                              ? "/fsr411_1080_di0_lm1_jm0_cs0_ae1_o1_lossy2_pc1_ca1/bindings.json"
                              : "/fsr411_1080_di0_lm1_jm0_cs0_ae1_o1_fast_pc1_ca1/bindings.json";
    const char *native = mode == Fsr412 ? "fsr412_native_1080"
                         : mode == Fsr41Lossy ? "fsr411_native_1080_lossy2"
                                              : "fsr411_native_1080";
    const char *recipe = mode == Fsr412 ? "/fsr412_native_weights_recipe.bin" : "/fsr411_native_recipe.bin";
    const char *files[6];
    int count = 0;
    files[count++] = "/fsr411_helpers.metallib";
    files[count++] = "/fsr411_simd.metallib";
    files[count++] = variant;
    char native_manifest[256], native_recipe[256], native_params[256];
    snprintf(native_manifest, sizeof native_manifest, "/%s/fsr411_native_manifest.json", native);
    snprintf(native_recipe, sizeof native_recipe, "/%s%s", native, recipe);
    snprintf(native_params, sizeof native_params, "/%s/fsr411_native_params.bin", native);
    files[count++] = native_manifest;
    files[count++] = native_recipe;
    files[count++] = native_params;
    char path[4096];
    for (int i = 0; i < count; ++i) {
        snprintf(path, sizeof path, "%s%s", assets, files[i]);
        FILE *file = fopen(path, "rb");
        if (!file) {
            printf("0\tthis build has no FSR %s Metal files\n", name);
            return 0;
        }
        fclose(file);
    }
    static unsigned char manifest[1 << 20];
    size_t length = 0;
    snprintf(path, sizeof path, "%s%s", assets, native_manifest);
    read_file(path, manifest, sizeof manifest - 1, &length);
    manifest[length] = 0;
    const char *key = strstr((const char *)manifest, "\"initializer_sha256\": \"");
    char expected[65] = {0};
    if (!key || sscanf(key + 23, "%64[0-9a-f]", expected) != 1) {
        printf("0\tthis build has no FSR %s Metal files\n", name);
        return 0;
    }
    const int lite = mode == Fsr412;
    const char *dir = getenv(lite ? "BB_FSR412_DIR" : "BB_FSR411_DIR");
    if (dir && dir[0] && initializer_ok(dir, expected, lite)) {
        printf("1\n");
        return 0;
    }
    const char *user = getenv("BB_DATA_DIR");
    if (user && user[0]) {
        snprintf(path, sizeof path, lite ? "%s/fsr4_412" : "%s/fsr4_411", user);
        if (initializer_ok(path, expected, lite)) {
            printf("1\n");
            return 0;
        }
    }
    printf("0\tAMD's FSR %s initializer%s is missing\n", name, lite ? " initializers-lite.bin" : "s.bin");
    return 0;
}
#endif

int main(int argc, char **argv) {
#ifdef __APPLE__
    if (argc > 2) {
        if (!strcmp(argv[1], "--fsr411")) return fsr_support(argv[2], Fsr41);
        if (!strcmp(argv[1], "--fsr411-lossy")) return fsr_support(argv[2], Fsr41Lossy);
        if (!strcmp(argv[1], "--fsr412")) return fsr_support(argv[2], Fsr412);
    }
#endif
    if (argc > 1 && !strcmp(argv[1], "--read-input")) {
        return read_input(argc > 2 ? argv[2] : "any");
    }
    if (argc > 1 && !strcmp(argv[1], "--gamepads")) {
        return list_gamepads();
    }
    if (argc > 1 && !strcmp(argv[1], "--displays")) {
        return list_displays();
    }
#ifdef __APPLE__
    if (argc > 1 && !strcmp(argv[1], "--device")) {
        puts("0x106b\tApple GPU");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--live-resolution")) {
        // The automatic live path is expensive on an integrated GPU. Presets use the
        // startup-size patch; the in-game setting can still explicitly select live mode.
        puts("0");
        return 0;
    }
    return 0;
#else
    const int live_mode = argc > 1 && !strcmp(argv[1], "--live-resolution");
    const int device_mode = argc > 1 && !strcmp(argv[1], "--device");
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bbport scene scaling probe",
        .apiVersion = VK_API_VERSION_1_3,
    };
#ifdef __APPLE__
    /* MoltenVK is a portability driver: listed only when the instance asks for it. */
    static const char *const portability[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
#endif
    const VkInstanceCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
#ifdef __APPLE__
        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = portability,
#endif
        .pApplicationInfo = &app,
    };
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&create, NULL, &instance) != VK_SUCCESS) {
        fputs("GPU scene scaling: cannot create Vulkan instance\n", stderr);
        return 1;
    }
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS || !count) {
        fputs("GPU scene scaling: no Vulkan device\n", stderr);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices || vkEnumeratePhysicalDevices(instance, &count, devices) != VK_SUCCESS) {
        fputs("GPU scene scaling: cannot enumerate Vulkan devices\n", stderr);
        free(devices);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    /* Match vk_instance.cpp's default ranking or its explicit BB_GPU_ID index. */
    VkPhysicalDevice selected = devices[0];
    const char *gpu_id = getenv("BB_GPU_ID");
    if (gpu_id && atoi(gpu_id) >= 0) {
        const unsigned long index = strtoul(gpu_id, NULL, 10);
        if (index >= count) {
            fputs("GPU scene scaling: BB_GPU_ID is outside the device list\n", stderr);
            free(devices);
            vkDestroyInstance(instance, NULL);
            return 1;
        }
        selected = devices[index];
    } else {
        for (uint32_t i = 1; i < count; ++i)
            if (better_device(devices[i], selected)) selected = devices[i];
    }
    if (device_mode) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(selected, &props);
        printf("%#06x\t%s\n", props.vendorID, props.deviceName);
        free(devices);
        vkDestroyInstance(instance, NULL);
        return 0;
    }
    if (live_mode) {
        printf("%d\n", live_resolution_suits(selected));
        free(devices);
        vkDestroyInstance(instance, NULL);
        return 0;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(selected, &props);
    const struct { VkFormat format; const char *name; } formats[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, "RGBA8"},
        {VK_FORMAT_R8G8B8A8_SRGB, "RGBA8 sRGB"},
        {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "B10G11R11"},
        {VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F"},
        {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32S8"},
    };
    int supported = 1;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        VkFormatProperties features;
        vkGetPhysicalDeviceFormatProperties(selected, formats[i].format, &features);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                              VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if ((features.optimalTilingFeatures & required) != required) {
            fprintf(stderr, "GPU scene scaling: %s lacks blit support for %s\n",
                    props.deviceName, formats[i].name);
            supported = 0;
        }
    }
    if (supported) fprintf(stderr, "GPU scene scaling: %s supports live presets\n", props.deviceName);
    free(devices);
    vkDestroyInstance(instance, NULL);
    return supported ? 0 : 1;
#endif
}
