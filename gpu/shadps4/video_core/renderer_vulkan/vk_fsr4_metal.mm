// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: FSR 4.1.x on Metal (macOS), from fsr4-native-macos v2.2 (gpu/fsr4_metal/VENDOR.txt).
// On the native Metal renderer (MetalLayer::FrameEnabled) it encodes on the frame's own
// MTLCommandBuffer, with the layer's direct MTLTexture accessors. On MoltenVK, the scene's color,
// depth and motion images are MoltenVK's own MTLTextures (VK_EXT_metal_objects), read and written
// in place on its MTLDevice, in a command queue of this upscaler's own.
//
// Ordering (MoltenVK, Events by default): the frame so far is submitted and FSR waits for it on
// the GPU through the scheduler's timeline exported as an MTLSharedEvent; FSR then signals a
// timeline of its own, and the next submission waits for it before a full barrier, which orders
// everything after it in submission order. No CPU waits. Without shared events (or
// BB_FSR4_METAL_SYNC=1) the GPU thread finishes the frame so far and waits for FSR. Callers fetch
// CommandBuffer() again. The native Metal path needs none of this: it records in frame order.
//
// The kernels and the models' layouts are built with the runtime (gpu/fsr4_metal/build_assets.py:
// metallibs, bindings, native manifests); AMD's initializers stay with the player
// (BB_FSR411_DIR / BB_FSR412_DIR, the player's data folder), as for the Vulkan FSR 4.1.1 path.
#include "video_core/renderer_vulkan/vk_fsr4_metal.h"

#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vulkan/vulkan_metal.h>

#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "fsr4_metal/fsr411_runtime.h"
#include "video_core/renderer_metal/mtl_vk_layer.h"

namespace Vulkan {

namespace {

bool EnvOn(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] == '1';
}

bool Exists(const std::string& path) {
    return [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:path.c_str()]];
}

id<MTLTexture> ExportTexture(VkDevice device, PFN_vkExportMetalObjectsEXT export_fn, VkImage image) {
    if (!device || !export_fn || !image) return nil;
    VkExportMetalTextureInfoEXT tex_info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
        .image = image,
        .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
    };
    VkExportMetalObjectsInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
        .pNext = &tex_info,
    };
    export_fn(device, &info);
    return tex_info.mtlTexture;
}

/// The built kernels and model layouts: BB_FSR4_METAL_DIR, the app's game/fsr4_metal, or the
/// runtime's out/fsr4_metal (run.sh from a source tree).
std::string AssetDir() {
    if (const char* env = std::getenv("BB_FSR4_METAL_DIR"); env && env[0] && Exists(env)) {
        return env;
    }
    for (NSString* path in @[[[NSBundle mainBundle] pathForResource:@"fsr4_metal" ofType:nil
                                                         inDirectory:@"game"] ?: @"",
                              @"fsr4_metal", @"out/fsr4_metal"]) {
        if (path.length && [[NSFileManager defaultManager] fileExistsAtPath:path]) {
            return path.UTF8String;
        }
    }
    return "fsr4_metal";
}

/// AMD's 4.1.1 initializers (initializers.bin, initializers-ultraperformance.bin): the player's,
/// never shipped (tools/fsr4_metal_initializers.sh takes them from their own AMD DLL). The
/// folders run.sh uses for the Vulkan FSR 4.1.1 path: BB_FSR411_DIR, fsr4_411 next to run.sh,
/// BB_DATA_DIR/fsr4_411.
std::vector<std::string> InitializerDirs() {
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("BB_FSR411_DIR"); env && env[0]) dirs.emplace_back(env);
    dirs.emplace_back("fsr4_411");
    if (const char* env = std::getenv("BB_DATA_DIR"); env && env[0]) {
        dirs.push_back(std::string(env) + "/fsr4_411");
    }
    return dirs;
}

/// AMD's 4.1.2 Lite initializers (initializers-lite.bin, initializers-lite-ultraperformance.bin):
/// the player's, never shipped (tools/fsr4_metal_initializers.sh takes them from their own AMD
/// DLL). BB_FSR412_DIR, fsr4_412 next to run.sh, BB_DATA_DIR/fsr4_412.
std::vector<std::string> InitializerDirs412() {
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("BB_FSR412_DIR"); env && env[0]) dirs.emplace_back(env);
    dirs.emplace_back("fsr4_412");
    if (const char* env = std::getenv("BB_DATA_DIR"); env && env[0]) {
        dirs.push_back(std::string(env) + "/fsr4_412");
    }
    return dirs;
}

const char* ModeName(Fsr4MetalMode mode) {
    return mode == Fsr4MetalMode::Fsr412 ? "FSR 4.1.2"
           : mode == Fsr4MetalMode::Fsr411Lossy ? "FSR 4.1 Lossy"
                                                : "FSR 4.1";
}

class Fsr4MetalBackendImpl final : public Fsr4MetalBackend {
public:
    Fsr4MetalBackendImpl(const Instance& instance, Scheduler& scheduler, Fsr4MetalMode mode)
        : instance_{instance}, scheduler_{scheduler}, mode_{mode},
          timing_{std::make_shared<Timing>(ModeName(mode))} {
        InitMetal();
    }

    ~Fsr4MetalBackendImpl() override {
        if (last_ && !last_completed()) {
            [last_ waitUntilCompleted];
        }
        if (done_semaphore_) {
            // Submissions that wait for it may still be in flight.
            scheduler_.Finish();
            instance_.GetDevice().destroySemaphore(done_semaphore_);
        }
    }

    bool Record(const Fsr4Upscaler::Frame& frame) override {
        if (!valid_) {
            return false;
        }
        if (last_ && last_.status == MTLCommandBufferStatusError) {
            problem_ = std::string("Metal ") + ModeName(mode_) + " failed on the GPU: " +
                       std::string(last_.error.localizedDescription.UTF8String ?: "unknown error");
            return false;
        }
        id<MTLTexture> color = nil, depth = nil, motion = nil, output = nil;
        if (native_) {
            const auto texture = [](vk::Image image) {
                return (__bridge id<MTLTexture>)MetalLayer::MetalImageTexture(
                    reinterpret_cast<void*>(static_cast<VkImage>(image)));
            };
            color = texture(frame.color.image);
            depth = texture(frame.depth.image);
            motion = texture(frame.motion.image);
            output = texture(frame.output.image);
        } else {
            color = ExportTexture(vk_device_, export_objects_, frame.color.image);
            depth = ExportTexture(vk_device_, export_objects_, frame.depth.image);
            motion = ExportTexture(vk_device_, export_objects_, frame.motion.image);
            output = ExportTexture(vk_device_, export_objects_, frame.output.image);
        }
        if (!color || !depth || !motion || !output) {
            problem_ = native_ ? "the Metal frame did not expose the images' MTLTextures"
                               : "MoltenVK did not export the images' MTLTextures (VK_EXT_metal_objects)";
            return false;
        }

        fsr411::Frame mtl{};
        mtl.color = color;
        mtl.depth = depth;
        mtl.motion = motion;
        mtl.output = output;
        // The conventions of the Vulkan FSR 4.1.1 path (vk_fsr4.cpp Record411) and of FSR 3.1 and
        // DLSS here: motion vectors in render pixels, previous minus current position, not
        // jittered, at the render size (motion scale 1, Y flipped by toggle Fsr4MotionYFlip);
        // jitter in render pixels as given; colour not pre-exposed; depth 0 at the near plane
        // (vk_camera_motion.cpp: depth = zs + zo / z), so not reversed.
        Fsr4InputDesc& in = mtl.input;
        in.rw = frame.render_width;
        in.rh = frame.render_height;
        in.uw = frame.output.width;
        in.uh = frame.output.height;
        in.jitterX = frame.jitter[0];
        in.jitterY = frame.jitter[1];
        in.mvScaleX = 1.0f;
        in.mvScaleY = BbToggle::Disabled(BbToggle::Fsr4MotionYFlip) ? -1.0f : 1.0f;
        in.preExposure = 1.0f;
        in.reset = frame.reset ? 1 : 0;
        in.depthReversed = 0;
        in.jitteredMV = 0;
        in.outputResMV = 0;
        in.autoExposure = frame.auto_exposure ? 1 : 0;
        in.inputContentProperties = -1;
        in.geometry.color = {uint32_t(color.width), uint32_t(color.height), 0, 0};
        in.geometry.depth = {uint32_t(depth.width), uint32_t(depth.height), 0, 0};
        in.geometry.motion = {uint32_t(motion.width), uint32_t(motion.height), 0, 0};
        in.geometry.output = {uint32_t(output.width), uint32_t(output.height), 0, 0};

        fsr411::Options options{};
        options.backend = fsr411::Backend::Auto;
        switch (mode_) {
        case Fsr4MetalMode::Fsr411: options.profile = fsr411::Profile::ExactFast; break;
        case Fsr4MetalMode::Fsr411Lossy: options.profile = fsr411::Profile::Lossy2; break;
        case Fsr4MetalMode::Fsr412: options.profile = fsr411::Profile::Int8Lite; options.modelLite = true; break;
        }
        options.ultraPerformance = frame.preset >= 4;
        options.rcas = frame.sharpen && frame.sharpness > 0.0f;
        options.sharpness = options.rcas ? frame.sharpness : 0.0f;

        if (native_) {
            id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)MetalLayer::MetalCommandBuffer(
                static_cast<void*>(static_cast<VkCommandBuffer>(frame.cmdbuf)));
            if (!cmd) {
                problem_ = "the Metal frame has no command buffer";
                return false;
            }
            std::string error;
            if (!runtime_->encode(cmd, *view_, mtl, options, error)) {
                problem_ = std::string("Metal ") + ModeName(mode_) + ": " + error;
                return false;
            }
            if (const std::string label = view_->backendLabel; label != described_) {
                described_ = label;
                std::printf("Upscaler: %s on Metal, %s, %ux%u -> %ux%u (frame command buffer)\n",
                            ModeName(mode_), label.c_str(), in.rw, in.rh, in.uw, in.uh);
            }
            problem_.clear();
            return true;
        }

        // The writes of the passes before FSR, and the output's earlier readers, before the
        // submission that FSR waits for.
        constexpr auto rw = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        const vk::MemoryBarrier2 full{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = rw,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = rw,
        };
        scheduler_.CommandBuffer().pipelineBarrier2(
            {.memoryBarrierCount = 1, .pMemoryBarriers = &full});

        const u64 inputs_tick = scheduler_.CurrentTick();
        if (events_) {
            scheduler_.Flush();
        } else {
            scheduler_.Finish();
        }
        id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
        if (!cmd) {
            problem_ = "cannot create a Metal command buffer";
            return false;
        }
        if (events_) {
            [cmd encodeWaitForEvent:work_event_ value:inputs_tick];
        }
        std::string error;
        if (!runtime_->encode(cmd, *view_, mtl, options, error)) {
            // Nothing was committed: the next submission does not wait.
            problem_ = std::string("Metal ") + ModeName(mode_) + ": " + error;
            return false;
        }
        if (events_) {
            [cmd encodeSignalEvent:done_event_ value:++done_value_];
        }
        // GPU time of each FSR command buffer, averaged in the log every 600 frames.
        std::shared_ptr<Timing> timing = timing_; // outlives this backend if it must
        [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
            if (done.status == MTLCommandBufferStatusCompleted && done.GPUEndTime > done.GPUStartTime) {
                timing->Add((done.GPUEndTime - done.GPUStartTime) * 1000.0);
            }
        }];
        [cmd commit];
        last_ = cmd;
        if (events_) {
            // FSR's output and everything after it in submission order wait for FSR.
            SubmitInfo info{};
            info.AddWait(done_semaphore_, done_value_);
            scheduler_.CommandBuffer().pipelineBarrier2(
                {.memoryBarrierCount = 1, .pMemoryBarriers = &full});
            scheduler_.Flush(info);
        } else {
            [cmd waitUntilCompleted];
        }
        if (const std::string label = view_->backendLabel; label != described_) {
            described_ = label;
            std::printf("Upscaler: %s on Metal, %s, %ux%u -> %ux%u%s\n", ModeName(mode_), label.c_str(),
                        in.rw, in.rh, in.uw, in.uh, events_ ? "" : " (synchronous)");
        }
        problem_.clear();
        return true;
    }

    const char* Problem() const noexcept override {
        return problem_.empty() ? nullptr : problem_.c_str();
    }

private:
    /// Upscaler GPU time (Metal's GPUStartTime/GPUEndTime), from Metal's completion thread.
    struct Timing {
        explicit Timing(const char* mode_name) : mode_name(mode_name) {}
        const char* mode_name;
        std::mutex mutex;
        double sum = 0, worst = 0;
        u32 frames = 0;
        void Add(double ms) {
            std::scoped_lock lock{mutex};
            sum += ms;
            worst = std::max(worst, ms);
            if (++frames == 600) {
                std::printf("Upscaler: %s on Metal GPU time %.2f ms a frame (worst %.2f ms, "
                            "600 frames)\n", mode_name, sum / frames, worst);
                sum = worst = 0;
                frames = 0;
            }
        }
    };

    bool last_completed() const {
        return last_.status == MTLCommandBufferStatusCompleted ||
               last_.status == MTLCommandBufferStatusError;
    }

    void InitMetal() {
        // On the native Metal renderer the layer exposes the frame's MTLTextures and the frame's
        // command buffer directly; encode FSR there (MetalFX uses the same accessors).
        if (MetalLayer::FrameEnabled()) {
            native_ = true;
            device_ = (__bridge id<MTLDevice>)MetalLayer::MtlDevice();
            if (!device_) {
                problem_ = "the Metal renderer has no MTLDevice";
                return;
            }
            std::vector<std::string> initializers = InitializerDirs();
            for (const auto& dir : InitializerDirs412()) initializers.push_back(dir);
            runtime_ = std::make_unique<fsr411::Runtime>(device_, AssetDir(), std::move(initializers));
            view_ = std::make_unique<fsr411::View>();
            valid_ = true;
            return;
        }
        if (!instance_.IsMetalObjectsSupported()) {
            problem_ = "the Vulkan driver lacks VK_EXT_metal_objects (MoltenVK has it)";
            return;
        }
        vk_device_ = instance_.GetDevice();
        export_objects_ = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
            instance_.GetDevice().getProcAddr("vkExportMetalObjectsEXT"));
        if (!export_objects_) {
            problem_ = "vkExportMetalObjectsEXT is missing";
            return;
        }
        VkExportMetalDeviceInfoEXT device_info{
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT,
        };
        VkExportMetalObjectsInfoEXT objects{
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
            .pNext = &device_info,
        };
        export_objects_(vk_device_, &objects);
        device_ = device_info.mtlDevice;
        if (!device_) {
            problem_ = "MoltenVK did not export its MTLDevice";
            return;
        }
        // FSR's own queue: its wait for the Vulkan work never blocks MoltenVK's queue.
        queue_ = [device_ newCommandQueue];
        if (!queue_) {
            problem_ = "cannot create a Metal command queue";
            return;
        }
        SetUpEvents();
        std::vector<std::string> initializers = InitializerDirs();
        for (const auto& dir : InitializerDirs412()) initializers.push_back(dir);
        runtime_ = std::make_unique<fsr411::Runtime>(device_, AssetDir(), std::move(initializers));
        view_ = std::make_unique<fsr411::View>();
        valid_ = true;
    }

    void SetUpEvents() {
        if (EnvOn("BB_FSR4_METAL_SYNC")) {
            std::printf("Upscaler: %s on Metal synchronous (BB_FSR4_METAL_SYNC=1)\n", ModeName(mode_));
            return;
        }
        const auto device = instance_.GetDevice();
        const vk::SemaphoreTypeCreateInfo timeline{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        };
        const auto created = device.createSemaphore({.pNext = &timeline});
        if (created.result != vk::Result::eSuccess) {
            return;
        }
        done_semaphore_ = created.value;
        const auto event_of = [&](vk::Semaphore semaphore) -> id<MTLSharedEvent> {
            VkExportMetalSharedEventInfoEXT event_info{
                .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT,
                .semaphore = static_cast<VkSemaphore>(semaphore),
            };
            VkExportMetalObjectsInfoEXT info{
                .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
                .pNext = &event_info,
            };
            export_objects_(vk_device_, &info);
            return event_info.mtlSharedEvent;
        };
        done_event_ = event_of(done_semaphore_);
        work_event_ = event_of(scheduler_.GetWorkSemaphore()->Handle());
        if (!done_event_ || !work_event_) {
            std::printf("Upscaler: %s on Metal synchronous (no shared events)\n", ModeName(mode_));
            device.destroySemaphore(done_semaphore_);
            done_semaphore_ = vk::Semaphore{};
            done_event_ = work_event_ = nil;
            return;
        }
        events_ = true;
    }

    const Instance& instance_;
    Scheduler& scheduler_;
    VkDevice vk_device_{VK_NULL_HANDLE};
    PFN_vkExportMetalObjectsEXT export_objects_{nullptr};
    id<MTLDevice> device_{nil};
    id<MTLCommandQueue> queue_{nil};
    id<MTLCommandBuffer> last_{nil};
    bool events_{false};
    vk::Semaphore done_semaphore_{}; ///< timeline signalled by Metal (done_event_)
    id<MTLSharedEvent> done_event_{nil};
    id<MTLSharedEvent> work_event_{nil}; ///< the scheduler's timeline
    u64 done_value_{0};
    Fsr4MetalMode mode_;
    bool native_{false};
    std::unique_ptr<fsr411::Runtime> runtime_;
    std::unique_ptr<fsr411::View> view_;
    bool valid_{false};
    std::string problem_;
    std::string described_;
    std::shared_ptr<Timing> timing_;
};

} // namespace

namespace {
std::string SupportProblem(const Instance& instance, Fsr4MetalMode mode) {
    std::string reason;
    if (!instance.IsMetalObjectsSupported() && !MetalLayer::FrameEnabled()) {
        reason = "the Vulkan driver lacks VK_EXT_metal_objects (MoltenVK has it)";
    } else {
        id<MTLDevice> device = MetalLayer::FrameEnabled()
                                    ? (__bridge id<MTLDevice>)MetalLayer::MtlDevice()
                                    : MTLCreateSystemDefaultDevice();
        if (!device || ![device supportsFamily:MTLGPUFamilyApple7]) {
            reason = "needs an Apple M1 or newer GPU";
        } else {
            const fsr411::Profile profile =
                mode == Fsr4MetalMode::Fsr412        ? fsr411::Profile::Int8Lite
                : mode == Fsr4MetalMode::Fsr411Lossy ? fsr411::Profile::Lossy2
                                                     : fsr411::Profile::ExactFast;
            if (!fsr411::Runtime::assets_ready(AssetDir(), InitializerDirs(), InitializerDirs412(),
                                               profile, reason)) {
                // reason already names the mode's missing files
            }
        }
    }
    if (reason.empty()) {
        std::printf("Upscaler: %s on Metal available\n", ModeName(mode));
    } else {
        std::printf("Upscaler: %s on Metal unavailable: %s\n", ModeName(mode), reason.c_str());
    }
    return reason;
}
} // namespace

bool Fsr4MetalBackend::IsSupported(const Instance& instance, Fsr4MetalMode mode) {
    // Once per run: the files do not change while the game runs, and this is asked every frame.
    static const std::string reason411 = SupportProblem(instance, Fsr4MetalMode::Fsr411);
    static const std::string reasonLossy = SupportProblem(instance, Fsr4MetalMode::Fsr411Lossy);
    static const std::string reason412 = SupportProblem(instance, Fsr4MetalMode::Fsr412);
    const std::string& reason = mode == Fsr4MetalMode::Fsr412        ? reason412
                                : mode == Fsr4MetalMode::Fsr411Lossy ? reasonLossy
                                                                     : reason411;
    return reason.empty();
}

std::unique_ptr<Fsr4MetalBackend> Fsr4MetalBackend::Create(const Instance& instance,
                                                           Scheduler& scheduler,
                                                           Fsr4MetalMode mode) {
    return std::make_unique<Fsr4MetalBackendImpl>(instance, scheduler, mode);
}

} // namespace Vulkan
#endif
