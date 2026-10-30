// SPDX-License-Identifier: GPL-2.0-or-later
// Saved FSR 4 choices must be safe before the first rendered frame on older GPUs.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <unistd.h>
#include "bbport_settings.h"

int main() {
    using namespace BbSettings;
    char path[] = "/tmp/bbport-upscaler-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    setenv("BB_CONFIG", path, 1);
    unsetenv("BB_UPSCALER");
    unsetenv("BB_UPSCALE_PRESET");
    unsetenv("BB_RENDER_RES");
    auto& s = Get();
    s.upscaler = UpscalerTaa;
    s.preset = Performance;
    assert(RenderPreset() == NativeAA && s.preset == Performance);
    Save();
    s.upscaler = UpscalerOff;
    Load();
    assert(s.upscaler == UpscalerTaa && s.preset == Performance);
    assert(RenderPreset() == NativeAA && !ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr3;
    assert(RenderPreset() == Performance);
    for (bool fsr4 : {false, true}) {
        for (bool fsr411 : {false, true}) {
            for (bool fsr411_lossy : {false, true}) {
                for (bool fsr412 : {false, true}) {
                    for (int requested = 0; requested < UpscalerCount; ++requested) {
                        FILE* config = std::fopen(path, "w");
                        assert(config);
                        std::fprintf(config, "upscaler=%s\npreset=3\noutput_res=2560x1440\n",
                                     UpscalerName(requested));
                        std::fclose(config);
                        s.fsr4_problem = nullptr;
                        Load();
                        assert(s.upscaler == requested);
                        ConfigureUpscalerSupport(fsr4, fsr411, fsr411_lossy, fsr412);
                        const bool unsupported =
                            (requested == UpscalerFsr4 && !fsr4) ||
#ifdef __APPLE__
                            (requested == UpscalerFsr411 && !fsr411) ||
                            (requested == UpscalerFsr411Lossy && !fsr411_lossy) ||
                            (requested == UpscalerFsr412 && !fsr412);
#else
                            (requested == UpscalerFsr411 && !(fsr4 && fsr411)) ||
                            requested == UpscalerFsr411Lossy || requested == UpscalerFsr412;
#endif
                        const int selected = requested == UpscalerMetalFx ? UpscalerOff :
                                             unsupported ? UpscalerFsr3 : requested;
                        assert(s.upscaler == selected);
                        assert(s.fsr4_supported == fsr4);
#ifdef __APPLE__
                        assert(s.fsr411_supported == fsr411);
                        assert(s.fsr411_lossy_supported == fsr411_lossy);
                        assert(s.fsr412_supported == fsr412);
#else
                        assert(s.fsr411_supported == (fsr4 && fsr411));
                        assert(!s.fsr411_lossy_supported && !s.fsr412_supported);
#endif
                        assert(bool(s.fsr4_problem.load()) == unsupported);
                        assert(s.preset == Performance && s.output_res == PackSize({2560, 1440}));
                        // Startup patch settings still describe the already applied guest patches.
                        assert(s.startup_upscaler == requested);
                        ConfigureUpscalerSupport(fsr4, fsr411, fsr411_lossy, fsr412);
                        assert(s.upscaler == selected);
                    }
                }
            }
        }
    }
    s.startup_preset = Quality;
    s.startup_upscaler = UpscalerFsr3;
    s.upscaler = UpscalerFsr3;
    s.preset = Performance;
    assert(RenderPreset() == Performance && !ResolutionNeedsRestart());
    setenv("BB_RENDER_RES", "1706x960", 1);
    assert(FixedRenderSession());
    assert(RenderPreset() == Quality && ResolutionNeedsRestart());
    s.preset = Quality;
    assert(!ResolutionNeedsRestart());
    s.upscaler = UpscalerOff;
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    assert(!ResolutionNeedsRestart()); // provider change at the same patched size is live
    s.upscaler = UpscalerTaa;          // TAA needs native guest targets
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    const auto output = s.startup_output_res;
    s.output_res = PackSize({2560, 1600}); // another output (here another aspect ratio too)
    assert(ResolutionNeedsRestart());
    s.output_res = output;
    setenv("BB_RENDER_RES", "", 1);
    assert(!FixedRenderSession() && !ResolutionNeedsRestart());
    setenv("BB_RENDER_RES", "1920x1080", 1);
    assert(!FixedRenderSession() && !ResolutionNeedsRestart());
    setenv("BB_RENDER_RES", "1920x1080junk", 1);
    assert(!FixedRenderSession());
    unsetenv("BB_RENDER_RES");
    std::remove(path);
    std::puts("Upscaler support: saved choices and FSR 3.1 fallback PASS");
}
