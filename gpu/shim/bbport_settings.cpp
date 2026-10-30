// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <string_view>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <io.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace BbSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key == "menu_language") {
        if (value == "en") v.menu_language = MenuLanguage::English;
        else if (value == "ru") v.menu_language = MenuLanguage::Russian;
        else std::printf("Settings: unknown menu_language '%s' (expected en or ru)\n", value.c_str());
    } else if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "fps_position") {
        for (int p = 0; p < FpsPositionCount; ++p) {
            if (value == FpsPositionNames[p]) v.fps_position = p;
        }
    } else if (key == "fps_size") {
        for (int z = 0; z < FpsSizeCount; ++z) {
            if (value == FpsSizeNames[z]) v.fps_size = z;
        }
    } else if (key == "menu_pos") {
        float x = -1.0f, y = -1.0f;
        if (std::sscanf(value.c_str(), "%f,%f", &x, &y) == 2 && x >= 0.0f && x <= 1.0f && y >= 0.0f &&
            y <= 1.0f) {
            v.menu_x = x;
            v.menu_y = y;
        }
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "model_lod") {
        v.model_lod = std::clamp(i, -2, 2);
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        int w = 0, h = 0;
        if (std::sscanf(value.c_str(), "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200 && w <= 16384 &&
            h <= 16384) {
            v.output_res = PackSize({w, h});
        }
    } else if (key == "aspect_ratio") {
        v.aspect = Aspect16x9;
        for (int a = 0; a < AspectCount; ++a) {
            if (value == AspectKey(a)) v.aspect = a;
        }
    } else {
        for (int e = 0; e < EffectCount; ++e) {
            if (key == Effects[e].key) {
                v.effects[e] = i != 0;
            }
        }
    }
}

} // namespace

namespace {
/// The outputs of each aspect ratio (bbport 0.6). 2560x1080 and 5120x2160 are 64:27, 3440x1440
/// 43:18, 3840x1600 12:5: each output's own ratio is the camera's.
struct Group {
    const char* key;
    double ratio;
    std::array<OutputSize, 10> sizes;
    int count;
};
constexpr Group Groups[AspectCount] = {
    {"auto", 0.0, {}, 0},
    {"4:3", 4.0 / 3.0,
     {{{640, 480}, {800, 600}, {1024, 768}, {1280, 960}, {1440, 1080}, {1600, 1200}, {2048, 1536}, {2880, 2160}}}, 8},
    {"16:10", 16.0 / 10.0,
     {{{640, 400}, {960, 600}, {1280, 800}, {1440, 900}, {1680, 1050}, {1920, 1200}, {2560, 1600}, {3840, 2400}}}, 8},
    {"16:9", 16.0 / 9.0,
     {{{640, 360}, {854, 480}, {960, 540}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800},
       {3840, 2160}}}, 9},
    {"21:9", 64.0 / 27.0,
     {{{860, 360}, {1280, 540}, {1720, 720}, {2560, 1080}, {3440, 1440}, {3840, 1600}, {5120, 2160}}}, 7},
    {"32:9", 32.0 / 9.0, {{{1280, 360}, {1920, 540}, {2560, 720}, {3840, 1080}, {5120, 1440}, {7680, 2160}}}, 6},
};
std::atomic<std::uint32_t> monitor{0}; // PackSize, 0 until known
} // namespace

int OutputList::IndexOf(OutputSize size) const {
    for (int i = 0; i < count; ++i) {
        if (sizes[i] == size) return i;
    }
    return -1;
}

const char* AspectKey(int aspect) {
    return Groups[std::clamp(aspect, 0, AspectCount - 1)].key;
}

OutputSize Monitor() {
    return UnpackSize(monitor.load(std::memory_order_relaxed));
}

int ResolvedAspect(int aspect) {
    if (aspect > AspectAuto && aspect < AspectCount) return aspect;
    const OutputSize m = Monitor();
    if (m.width <= 0 || m.height <= 0) return Aspect16x9;
    const double ratio = double(m.width) / m.height;
    int best = Aspect16x9;
    for (int a = AspectAuto + 1; a < AspectCount; ++a) {
        if (std::abs(std::log(ratio / Groups[a].ratio)) < std::abs(std::log(ratio / Groups[best].ratio))) {
            best = a;
        }
    }
    return best;
}

OutputList Outputs(int aspect) {
    const Group& g = Groups[ResolvedAspect(aspect)];
    OutputList list;
    for (int i = 0; i < g.count; ++i) list.sizes[list.count++] = g.sizes[i];
    // Auto: the monitor's own size too (another ratio than its group's sizes, 1366x768, ...).
    const OutputSize m = Monitor();
    const auto insert = [&list](OutputSize size) {
        if (size.width < 640 || size.height < 360 || size.width > 7680 || size.height > 4320 ||
            list.IndexOf(size) >= 0 || list.count >= int(list.sizes.size())) {
            return;
        }
        int at = 0;
        while (at < list.count && (list.sizes[at].height < size.height ||
                                   (list.sizes[at].height == size.height && list.sizes[at].width < size.width))) {
            ++at;
        }
        for (int i = list.count; i > at; --i) list.sizes[i] = list.sizes[i - 1];
        list.sizes[at] = size;
        ++list.count;
    };
    if (aspect == AspectAuto && m.width > 0 && m.height > 0) {
        insert(m);
        // A screen of a ratio no group has (a phone: 2868x1320 is 2.17:1): its ratio at the usual
        // heights up to its own, so a smaller output fills it too (even widths: the game has
        // half-resolution buffers).
        const double ratio = double(m.width) / m.height;
        if (std::abs(std::log(ratio / g.ratio)) > 0.01) {
            for (const int height : {540, 720, 1080, 1440, 2160}) {
                if (height < m.height) insert({int(std::lround(height * ratio / 2.0)) * 2, height});
            }
        }
    }
    return list;
}

OutputSize NearestOutput(int aspect, OutputSize size) {
    const OutputList list = Outputs(aspect);
    if (list.IndexOf(size) >= 0) return size;
    OutputSize best = list.sizes[0];
    for (const OutputSize& s : list) {
        if (std::abs(s.height - size.height) < std::abs(best.height - size.height)) best = s;
    }
    return best;
}

OutputSize CurrentOutput() {
    const auto& v = Get();
    return NearestOutput(v.aspect.load(std::memory_order_relaxed),
                         UnpackSize(v.output_res.load(std::memory_order_relaxed)));
}

std::string SizeText(OutputSize size) {
    return std::to_string(size.width) + "x" + std::to_string(size.height);
}

void SetMonitor(int width, int height) {
    // The game is landscape: a screen reported upright (a phone's) is its landscape size.
    if (height > width) std::swap(width, height);
    const OutputSize m{width, height};
    if (width <= 0 || height <= 0 || m == Monitor()) return;
    monitor.store(PackSize(m), std::memory_order_relaxed);
    auto& v = Get();
    // Auto follows the monitor: its output is kept (and saved: run.sh's startup patch reads it).
    const OutputSize fitted = CurrentOutput();
    if (v.aspect == AspectAuto && PackSize(fitted) != v.output_res.load()) {
        std::printf("Output: %s for the monitor (%dx%d, aspect ratio auto)\n", SizeText(fitted).c_str(),
                    width, height);
        v.output_res = PackSize(fitted);
        Save();
    }
}

Values& Get() {
    static Values values;
    return values;
}

const char* MenuText(const char* english, const char* russian) {
    return Get().menu_language == MenuLanguage::Russian ? russian : english;
}

void Load() {
    auto& v = Get();
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
    // bbport: the menu's language until one is chosen in it (menu_language in bbport.ini): the
    // launcher's (BB_MENU_LANGUAGE), else the system's, as the launcher picks its own.
    {
        const char* lang = std::getenv("BB_MENU_LANGUAGE");
        if (!lang || !*lang) {
            for (const char* key : {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"}) {
                if (const char* value = std::getenv(key); value && *value) {
                    lang = value;
                    break;
                }
            }
        }
        v.menu_language = lang && (lang[0] == 'r' || lang[0] == 'R') && (lang[1] == 'u' || lang[1] == 'U')
                              ? MenuLanguage::Russian
                              : MenuLanguage::English;
    }
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411, bool metalfx, bool metalfx_only) {
    auto& v = Get();
    v.fsr4_supported = fsr4 && !metalfx_only;
#ifdef __APPLE__
    v.fsr411_supported = fsr411 && !metalfx_only;
#else
    v.fsr411_supported = fsr4 && fsr411;
#endif
    v.fsr3_supported = !metalfx_only;
    v.metalfx_supported = metalfx;
    const int requested = v.upscaler;
    if (metalfx_only && (requested == UpscalerFsr3 || requested == UpscalerFsr4 ||
                         requested == UpscalerFsr411 || requested == UpscalerDlss)) {
        v.upscaler = metalfx ? UpscalerMetalFx : UpscalerOff;
        std::printf("Upscaler: %s is not offered on the Metal renderer; %s before the first frame\n",
                    UpscalerName(requested), metalfx ? "MetalFX" : "upscaler off");
        return;
    }
    if (requested == UpscalerMetalFx && !metalfx) {
        // Off, not FSR 3.1: only macOS has MetalFX, where FSR 3.1 is not proven either.
        std::printf("Upscaler: MetalFX unavailable here; upscaler off before the first frame\n");
        v.upscaler = UpscalerOff;
        return;
    }
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        const int fallback = v.fsr3_supported ? UpscalerFsr3
                             : v.metalfx_supported ? UpscalerMetalFx : UpscalerOff;
        std::printf("Upscaler: DLSS unavailable (%s); falling back to %s\n",
                    kept.empty() ? "the DLSS bridge or NVIDIA's DLSS library is missing" : kept.c_str(),
                    UpscalerName(fallback));
        v.upscaler = fallback;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("BB_RENDER_RES");
    unsigned width = 0, height = 0;
    char trailing = 0;
    return size && std::sscanf(size, "%ux%u%c", &width, &height, &trailing) == 2 &&
           width && height && (width != OutputDefault.width || height != OutputDefault.height);
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

namespace {

/// The keys the menu writes, in this order, with their current values.
std::vector<std::pair<std::string, std::string>> Keys() {
    const auto& v = Get();
    std::vector<std::pair<std::string, std::string>> keys;
    const auto put = [&](const char* key, std::string value) { keys.emplace_back(key, std::move(value)); };
    const auto fixed = [](float value, int digits) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.*f", digits, value);
        return std::string(text);
    };
    const auto flag = [](bool value) { return std::string(value ? "1" : "0"); };
    put("menu_language", v.menu_language == MenuLanguage::Russian ? "ru" : "en");
    put("upscaler", UpscalerName(v.upscaler));
    put("preset", std::to_string(v.preset.load()));
    put("sharpen", flag(v.sharpen));
    put("sharpness", fixed(v.sharpness, 2));
    put("jitter", flag(v.jitter));
    put("reactive", flag(v.reactive));
    put("object_motion", flag(v.object_motion));
    put("reactive_scale", fixed(v.reactive_scale, 2));
    put("reactive_threshold", fixed(v.reactive_threshold, 2));
    put("reactive_max", fixed(v.reactive_max, 2));
    put("debug_view", std::to_string(v.debug_view.load()));
    put("show_fps", flag(v.show_fps));
    put("fps_position", FpsPositionNames[v.fps_position]);
    put("fps_size", FpsSizeNames[v.fps_size]);
    put("fsr4_auto_exposure", flag(v.fsr4_auto_exposure));
    put("fsr4_invert_jitter", flag(v.fsr4_invert_jitter));
    // Read by patches.py at start.
    for (int e = 0; e < EffectCount; ++e) {
        put(Effects[e].key, flag(v.effects[e]));
    }
    put("model_lod", std::to_string(v.model_lod.load()));
    put("output_res", SizeText(UnpackSize(v.output_res)));
    put("aspect_ratio", AspectKey(v.aspect));
    // Read by run.sh at start.
    put("live_resolution", v.live_resolution < 0 ? "auto" : flag(v.live_resolution != 0));
    if (v.menu_x >= 0.0f && v.menu_y >= 0.0f) {
        put("menu_pos", fixed(v.menu_x, 4) + "," + fixed(v.menu_y, 4));
    }
    return keys;
}

// The launcher's settings channel (BB_SETTINGS_CHANNEL=1, set by the launcher, which owns the
// settings in its database): the menu's changes go out as "BBSET key=value" lines on stdout, the
// launcher's come in the same way on stdin. `known` holds the values the launcher has, so neither
// side echoes the other's change back.
bool Channel() {
    static const bool on = [] {
        const char* env = std::getenv("BB_SETTINGS_CHANNEL");
        return env && env[0] == '1';
    }();
    return on;
}
std::mutex channel_mutex;
std::map<std::string, std::string> known;

// The launcher's options (LauncherKey): launcher.json's key, the variable the launcher starts the
// game with, its default, and how a value becomes that variable (empty: the variable is unset).
struct LauncherOption {
    const char* key;
    const char* env;
    const char* fallback;
};
constexpr LauncherOption LauncherOptions[LauncherKeyCount] = {
    {"fullscreen", "BB_FULLSCREEN", "false"}, {"display", "BB_DISPLAY", ""},
    {"present_mode", "BB_PRESENT_MODE", "Fifo"}, {"fps_mode", "BB_FPS", "uncap"},
    {"fps_limit", "BB_FPS_LIMIT", "0"},       {"metal_hud", "MTL_HUD_ENABLED", "false"},
    {"draw_pipe", "BB_DRAW_PIPE", ""},        {"readbacks", "BB_READBACKS", ""},
    {"preupload", "BB_PREUPLOAD", ""},        {"renderer", "BB_METAL_FRAME", "metal"},
};
std::string launcher_values[LauncherKeyCount], launcher_startup[LauncherKeyCount];
std::atomic<int> fullscreen_request{-1};

bool IsBoolOption(int key) {
    return key == LauncherFullscreen || key == LauncherMetalHud;
}

/// The launcher's value text from the variable it set (the inverse of EnvText).
std::string FromEnvironment(int key) {
    const char* env = std::getenv(LauncherOptions[key].env);
    std::string value = env ? env : "";
    if (IsBoolOption(key)) {
        return value == "1" ? "true" : "false";
    }
    // The renderer: BB_METAL_FRAME=1 is the Metal backend's frame path (run.sh's default on
    // Apple Silicon), 0 MoltenVK.
    if (key == LauncherRenderer) {
        return value == "1" ? "metal" : "moltenvk";
    }
    // run.sh exports BB_PREUPLOAD=1 (normal) when the launcher left it unset.
    if (key == LauncherPreupload && value == "1") {
        return "";
    }
    return value.empty() ? LauncherOptions[key].fallback : value;
}

std::string EnvText(int key, const std::string& value) {
    if (IsBoolOption(key)) {
        return value == "true" ? "1" : key == LauncherFullscreen ? "0" : "";
    }
    if (key == LauncherRenderer) {
        return value == "metal" ? "1" : "0";
    }
    if (key == LauncherFpsLimit && std::atoi(value.c_str()) <= 0) {
        return "";
    }
    return value;
}

/// Set by the frame rate hooks (bbport 0.6) when they run the game's pacer: the limit is live.
std::atomic<void (*)(int)> live_fps_limit{nullptr};

/// Launcher options applied while the game runs: fullscreen, and the frame rate limit with the
/// frame rate hooks.
bool IsLive(int key) {
    return key == LauncherFullscreen || (key == LauncherFpsLimit && live_fps_limit.load());
}

void ApplyLive(int key, const std::string& value) {
    if (key == LauncherFpsLimit) {
        if (const auto apply = live_fps_limit.load()) apply(std::max(0, std::atoi(value.c_str())));
    }
}

int LauncherIndex(const std::string& key) {
    for (int k = 0; k < LauncherKeyCount; ++k) {
        if (key == LauncherOptions[k].key) return k;
    }
    return -1;
}

/// The next line of the launcher's input. read(2) on fd 0, not stdio: a thread blocked in fgets
/// holds stdin's lock, and fflush(NULL) (an in-game restart) waited for it forever.
bool NextLine(std::string& line) {
    static std::string pending;
    for (;;) {
        if (const auto end = pending.find('\n'); end != std::string::npos) {
            line = pending.substr(0, end);
            pending.erase(0, end + 1);
            return true;
        }
        char buffer[512];
#ifdef _WIN32
        const int got = _read(0, buffer, sizeof(buffer));
#else
        const ssize_t got = read(0, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) {
            continue;
        }
#endif
        if (got <= 0) {
            return false;
        }
        pending.append(buffer, size_t(got));
    }
}

void ReadLauncher() {
    std::string text;
    while (NextLine(text)) {
        text.erase(text.find_last_not_of(" \t\r\n") + 1);
        if (text.rfind("BBSET ", 0) != 0) {
            continue;
        }
        const auto eq = text.find('=');
        if (eq == std::string::npos || eq <= 6) {
            continue;
        }
        const std::string key = text.substr(6, eq - 6), value = text.substr(eq + 1);
        if (key.rfind("launcher:", 0) == 0) {
            const int k = LauncherIndex(key.substr(9));
            if (k < 0) {
                continue;
            }
            std::scoped_lock lock{channel_mutex};
            if (launcher_values[k] != value && k == LauncherFullscreen) {
                fullscreen_request = value == "true" ? 1 : 0;
            }
            ApplyLive(k, value);
            launcher_values[k] = value;
            known[key] = value;
            std::printf("Settings: %s=%s from the launcher\n", key.c_str(), value.c_str());
            continue;
        }
        Set(Get(), key, value);
        std::scoped_lock lock{channel_mutex};
        // As the menu would write it (normalized), so the next Save does not send it back.
        for (const auto& [k, v] : Keys()) {
            if (k == key) known[k] = v;
        }
        std::printf("Settings: %s=%s from the launcher\n", key.c_str(), value.c_str());
    }
}

} // namespace

void StartLauncherChannel() {
    {
        std::scoped_lock lock{channel_mutex};
        for (int k = 0; k < LauncherKeyCount; ++k) {
            launcher_values[k] = launcher_startup[k] = FromEnvironment(k);
            known[std::string("launcher:") + LauncherOptions[k].key] = launcher_values[k];
        }
    }
    if (!Channel()) {
        return;
    }
    {
        std::scoped_lock lock{channel_mutex};
        for (const auto& [key, value] : Keys()) {
            known[key] = value;
        }
    }
    std::thread(ReadLauncher).detach();
    std::printf("Settings: kept in sync with the launcher\n");
}

bool LauncherChannel() {
    return Channel();
}

std::string Launcher(LauncherKey key) {
    std::scoped_lock lock{channel_mutex};
    return launcher_values[key];
}

void SetLauncher(LauncherKey key, const std::string& value) {
    std::scoped_lock lock{channel_mutex};
    if (key == LauncherFullscreen && launcher_values[key] != value) {
        fullscreen_request = value == "true" ? 1 : 0;
    }
    ApplyLive(key, value);
    launcher_values[key] = value;
}

void SetLiveFpsLimit(void (*apply)(int fps)) {
    live_fps_limit.store(apply);
}

bool LauncherNeedsRestart() {
    std::scoped_lock lock{channel_mutex};
    for (int k = 0; k < LauncherKeyCount; ++k) {
        if (!IsLive(k) && launcher_values[k] != launcher_startup[k]) return true;
    }
    return false;
}

void ExportLauncherEnvironment() {
    std::scoped_lock lock{channel_mutex};
    for (int k = 0; k < LauncherKeyCount; ++k) {
        const std::string value = EnvText(k, launcher_values[k]);
#ifdef _WIN32
        _putenv_s(LauncherOptions[k].env, value.c_str()); // empty removes it
#else
        if (value.empty()) {
            unsetenv(LauncherOptions[k].env);
        } else {
            setenv(LauncherOptions[k].env, value.c_str(), 1);
        }
#endif
    }
}

int TakeFullscreenRequest() {
    return fullscreen_request.exchange(-1);
}

void Save() {
    const auto keys = Keys();
    if (Channel()) {
        std::scoped_lock lock{channel_mutex};
        for (const auto& [key, value] : keys) {
            if (key == "menu_pos") continue;
            if (auto it = known.find(key); it == known.end() || it->second != value) {
                known[key] = value;
                std::printf("BBSET %s=%s\n", key.c_str(), value.c_str());
            }
        }
        for (int k = 0; k < LauncherKeyCount; ++k) {
            const std::string key = std::string("launcher:") + LauncherOptions[k].key;
            if (known[key] != launcher_values[k]) {
                known[key] = launcher_values[k];
                std::printf("BBSET %s=%s\n", key.c_str(), launcher_values[k].c_str());
            }
        }
        std::fflush(stdout);
    }

    std::string out;
    bool had_lines = false;
    std::vector<bool> written(keys.size());
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), file)) {
            had_lines = true;
            std::string text{line};
            text.erase(text.find_last_not_of("\r\n") + 1);
            const auto eq = text.find('=');
            if (!text.empty() && text[0] != '#' && eq != std::string::npos) {
                std::string key = text.substr(0, eq);
                key.erase(key.find_last_not_of(" \t") + 1);
                key.erase(0, key.find_first_not_of(" \t"));
                const auto it = std::find_if(keys.begin(), keys.end(),
                                             [&](const auto& k) { return k.first == key; });
                if (it != keys.end()) {
                    out += it->first + "=" + it->second + "\n";
                    written[size_t(it - keys.begin())] = true;
                    continue;
                }
            }
            out += text + "\n";
        }
        std::fclose(file);
    }
    if (!had_lines) {
        out = "# bbport settings (in-game menu: Insert / L3+R3)\n";
    }
    for (size_t k = 0; k < keys.size(); ++k) {
        if (!written[k]) {
            out += keys[k].first + "=" + keys[k].second + "\n";
        }
    }
    // Replaced whole: a crash while writing leaves the old file.
    const std::string temporary = std::string(Path()) + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "w");
    bool ok = file && std::fwrite(out.data(), 1, out.size(), file) == out.size();
    ok = file && std::fclose(file) == 0 && ok;
    if (!ok || std::rename(temporary.c_str(), Path()) != 0) {
        std::printf("Settings: cannot write %s\n", Path());
    }
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa",
                                                         "dlss", "metalfx"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace BbSettings
