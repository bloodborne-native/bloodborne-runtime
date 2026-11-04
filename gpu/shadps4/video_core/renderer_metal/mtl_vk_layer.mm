// SPDX-License-Identifier: GPL-2.0-or-later
// From Wooloomooloo2/bloodborne_mac_port (78b59f5, GPL-2.0-or-later), adapted to bloodborne-runtime.

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include "video_core/renderer_metal/mtl_vk_layer.h"
#include "video_core/renderer_metal/mtl_vk_caps.h"
#include "video_core/renderer_vulkan/vk_common.h"
// The Metal export types (this directory builds without VK_USE_PLATFORM_METAL_EXT: under ARC its
// structs break vulkan.hpp; the C header alone is fine).
#include <vulkan/vulkan_metal.h>
#include "video_core/renderer_metal/mtl_frame.h"
#include "video_core/renderer_metal/mtl_shadow.h"
#include "video_core/renderer_vulkan/vk_metal_shadow.h"

namespace MetalLayer {
namespace {

using Dispatcher = std::remove_reference_t<decltype(VULKAN_HPP_DEFAULT_DISPATCHER)>;
using MetalFrame::Owned;

// The dispatcher members the renderer's command recording and submission use (core names and the
// extension aliases vulkan.hpp may have filled instead), and the device functions whose objects
// the Metal path has to know about.
#define BB_LAYER_FUNCTIONS(X)                                                                      \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkQueueSubmit2)               \
    X(vkQueueSubmit2KHR) X(vkCmdPipelineBarrier) X(vkCmdPipelineBarrier2)                          \
    X(vkCmdPipelineBarrier2KHR) X(vkCmdBindPipeline) X(vkCmdPushDescriptorSetKHR)                  \
    X(vkCmdPushConstants) X(vkCmdBindDescriptorSets) X(vkCmdDispatch) X(vkCmdDispatchIndirect)     \
    X(vkCmdBeginRendering) X(vkCmdBeginRenderingKHR) X(vkCmdEndRendering)                          \
    X(vkCmdEndRenderingKHR) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect)                  \
    X(vkCmdDrawIndexedIndirect) X(vkCmdDrawIndirectCount) X(vkCmdDrawIndexedIndirectCount)         \
    X(vkCmdBindVertexBuffers) X(vkCmdBindVertexBuffers2) X(vkCmdBindVertexBuffers2EXT)             \
    X(vkCmdBindIndexBuffer) X(vkCmdCopyImage) X(vkCmdCopyBuffer) X(vkCmdCopyImageToBuffer)         \
    X(vkCmdCopyBufferToImage) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearColorImage)       \
    X(vkCmdClearDepthStencilImage) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer)                         \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetViewportWithCount)                            \
    X(vkCmdSetViewportWithCountEXT) X(vkCmdSetScissorWithCount) X(vkCmdSetScissorWithCountEXT)     \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthTestEnableEXT) X(vkCmdSetDepthWriteEnable)           \
    X(vkCmdSetDepthWriteEnableEXT) X(vkCmdSetDepthCompareOp) X(vkCmdSetDepthCompareOpEXT)          \
    X(vkCmdSetDepthBoundsTestEnable) X(vkCmdSetDepthBounds) X(vkCmdSetDepthBiasEnable)             \
    X(vkCmdSetDepthBias) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilTestEnableEXT)              \
    X(vkCmdSetStencilOp) X(vkCmdSetStencilOpEXT) X(vkCmdSetStencilReference)                       \
    X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilCompareMask) X(vkCmdSetPrimitiveRestartEnable)    \
    X(vkCmdSetRasterizerDiscardEnable) X(vkCmdSetCullMode) X(vkCmdSetCullModeEXT)                  \
    X(vkCmdSetFrontFace) X(vkCmdSetFrontFaceEXT) X(vkCmdSetBlendConstants)                         \
    X(vkCmdSetColorWriteMaskEXT) X(vkCmdSetLineWidth) X(vkCmdSetVertexInputEXT)                    \
    X(vkCmdBeginDebugUtilsLabelEXT) X(vkCmdEndDebugUtilsLabelEXT) X(vkCmdInsertDebugUtilsLabelEXT) \
    X(vkCmdWriteTimestamp) X(vkCmdWriteTimestamp2) X(vkCmdWriteBufferMarkerAMD)                    \
    X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdResetQueryPool) X(vkCmdCopyQueryPoolResults)        \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool)                           \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkResetCommandBuffer)                    \
    X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout)                  \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkDestroyImage)               \
    X(vkCreateSampler) X(vkDestroySampler) X(vkDestroyImageView) X(vkDestroyPipeline)              \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkGetSemaphoreCounterValue)                      \
    X(vkWaitSemaphores) X(vkSignalSemaphore) X(vkCreateFence) X(vkDestroyFence)                   \
    X(vkWaitForFences) X(vkResetFences) X(vkGetFenceStatus)                                      \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool)                                           \
    X(vkGetBufferDeviceAddress) X(vkGetBufferDeviceAddressKHR) X(vkCreateShaderModule)            \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkCreateImageView)               \
    X(vkSetDebugUtilsObjectNameEXT) X(vkUpdateDescriptorSets) X(vkGetDeviceProcAddr)             \
    X(vkGetInstanceProcAddr)

enum Id : unsigned {
#define BB_ID(name) Id_##name,
    BB_LAYER_FUNCTIONS(BB_ID)
#undef BB_ID
        Count
};
constexpr const char* Names[] = {
#define BB_NAME(name) #name,
    BB_LAYER_FUNCTIONS(BB_NAME)
#undef BB_NAME
};

bool trace;
bool present_enabled; ///< BB_METAL_PRESENT=1: the present pass (step B)
bool frame_enabled;   ///< BB_METAL_FRAME=1: every command buffer (step C)
std::array<std::atomic<unsigned long long>, Count> calls{};
std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
std::atomic<unsigned long long> metal_submits{0}, vulkan_submits{0}; ///< SubmitInMetal
std::mutex report_mutex;
id<MTLCommandQueue> queue;

/// BB_METAL_INVENTORY=1 (docs/RENDER_ADAPTER.md, M4 step 2): every Vulkan function the dispatcher
/// loaded is wrapped, below the layer's own hooks, to count the calls that reach the driver
/// (MoltenVK). The list is tools/vk_inventory_list.py's, from vulkan.hpp.
bool inventory;
struct InventoryEntry {
    const char* name = nullptr;
    std::atomic<unsigned long long> calls{0};
    unsigned long long total = 0;
};
std::deque<InventoryEntry> inventory_entries; ///< filled once at Install: stable addresses
unsigned inventory_reports = 0;

template <int N, typename F>
struct Passthrough;
template <int N, typename R, typename... A>
struct Passthrough<N, R (*)(A...)> {
    static inline R (*original)(A...) = nullptr;
    static inline std::atomic<unsigned long long>* calls = nullptr;
    static R Call(A... args) {
        calls->fetch_add(1, std::memory_order_relaxed);
        return original(args...);
    }
    static void Install(R (*&slot)(A...), const char* name) {
        if (!slot) {
            return;
        }
        auto& entry = inventory_entries.emplace_back();
        entry.name = name;
        calls = &entry.calls;
        original = slot;
        slot = &Call;
    }
};

/// Per second since the last report; every 12th report also the totals since the start.
void ReportInventory(double seconds) {
    std::vector<std::pair<unsigned long long, const InventoryEntry*>> now, total;
    for (auto& e : inventory_entries) {
        const auto n = e.calls.exchange(0);
        e.total += n;
        if (n) {
            now.emplace_back(n, &e);
        }
        if (e.total) {
            total.emplace_back(e.total, &e);
        }
    }
    const auto print = [](const char* what, auto& list, double scale) {
        std::sort(list.rbegin(), list.rend());
        std::string line;
        for (const auto& [n, e] : list) {
            char item[128];
            std::snprintf(item, sizeof item, "%s%s %.0f", line.empty() ? "" : ", ", e->name + 2,
                          double(n) / scale);
            line += item;
        }
        std::printf("Metal inventory: %s (%zu functions): %s\n", what, list.size(), line.c_str());
    };
    print(NativeBoot() ? "adapter calls per second" : "calls reaching the driver per second", now, seconds);
    if (++inventory_reports % 12 == 1) {
        print(NativeBoot() ? "adapter calls since the start" : "calls reaching the driver since the start", total, 1.0);
    }
}

void Report() {
    const auto now = std::chrono::steady_clock::now();
    if (now - since < std::chrono::seconds(5)) {
        return;
    }
    std::unique_lock lock{report_mutex, std::try_to_lock};
    if (!lock || now - since < std::chrono::seconds(5)) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - since).count();
    since = now;
    if (frame_enabled) {
        auto& s = MetalFrame::GetStats();
        std::printf("Metal frame: per second: %.0f draws, %.0f dispatches; skipped: %.0f without "
                    "a Metal pipeline, %.0f tessellated, %.0f emulated topology, %.0f discarded; "
                    "%.0f unsupported commands; %zu device-address buffers declared per encoder; tessellated "
                    "draws %.0f, of them right after another %.0f\n",
                    s.draws.exchange(0) / seconds, s.dispatches.exchange(0) / seconds,
                    s.skipped_no_pipeline.exchange(0) / seconds, s.skipped_tess.exchange(0) / seconds,
                    s.skipped_emulated.exchange(0) / seconds, s.skipped_discard.exchange(0) / seconds,
                    s.unsupported.exchange(0) / seconds, MetalFrame::AddressBufferCount(),
                    s.tess_draws.exchange(0) / seconds, s.tess_after_tess.exchange(0) / seconds);
        std::printf("Metal layer: per second: %.0f submits committed in Metal, %.0f through "
                    "MoltenVK\n",
                    metal_submits.exchange(0) / seconds, vulkan_submits.exchange(0) / seconds);
    }
    if (inventory) {
        ReportInventory(seconds);
    }
    if (!trace) {
        return;
    }
    std::vector<std::pair<unsigned long long, unsigned>> top;
    for (unsigned i = 0; i < Count; ++i) {
        if (const auto n = calls[i].exchange(0)) {
            top.emplace_back(n, i);
        }
    }
    std::sort(top.rbegin(), top.rend());
    std::string line;
    for (const auto& [n, i] : top) {
        char item[96];
        std::snprintf(item, sizeof item, "%s%s %.0f", line.empty() ? "" : ", ", Names[i] + 2,
                      double(n) / seconds);
        line += item;
    }
    std::printf("Metal layer: Vulkan calls per second: %s\n", line.c_str());
}

/// Our entry points by Id (installed ones), for vkGet*ProcAddr: libraries that load their own
/// function pointers (FSR, the ImGui overlay) then record through the layer too.
std::array<PFN_vkVoidFunction, Count> hooked{};
PFN_vkVoidFunction HookedByName(const char* name) {
    if (!name || !(present_enabled || frame_enabled)) {
        return nullptr;
    }
    for (unsigned i = 0; i < Count; ++i) {
        if (hooked[i] && i != Id_vkGetDeviceProcAddr && i != Id_vkGetInstanceProcAddr &&
            std::strcmp(Names[i], name) == 0) {
            return hooked[i];
        }
    }
    return nullptr;
}

std::mutex owned_mutex;
std::unordered_map<VkCommandBuffer, Owned*> owned;
std::atomic<unsigned> owned_count{0}; ///< owned.size(): the lock is skipped while it is zero

/// Bumped whenever an owned command buffer appears or goes: the per-thread lookup cache below is
/// valid while it is unchanged (a command buffer is recorded by one thread at a time).
std::atomic<u64> owned_generation{0};

Owned* FindOwned(VkCommandBuffer cb) {
    if (owned_count.load(std::memory_order_acquire) == 0) {
        return nullptr;
    }
    thread_local VkCommandBuffer cached_cb = VK_NULL_HANDLE;
    thread_local Owned* cached = nullptr;
    thread_local u64 cached_generation = ~0ull;
    const u64 generation = owned_generation.load(std::memory_order_acquire);
    if (cb == cached_cb && generation == cached_generation) {
        return cached;
    }
    std::scoped_lock lock{owned_mutex};
    auto it = owned.find(cb);
    cached_cb = cb;
    cached = it == owned.end() ? nullptr : it->second;
    cached_generation = owned_generation.load(std::memory_order_acquire);
    return cached;
}

void OwnBuffer(VkCommandBuffer cb) {
    auto* o = new Owned;
    @autoreleasepool {
        o->cmd = [queue commandBufferWithUnretainedReferences];
    }
    o->cmd.label = @"bbport Metal path";
    std::scoped_lock lock{owned_mutex};
    if (auto it = owned.find(cb); it != owned.end()) {
        delete it->second; // begun again without a submit (reset)
        it->second = o;
    } else {
        owned[cb] = o;
    }
    owned_count.store(unsigned(owned.size()), std::memory_order_release);
    ++owned_generation;
}

/// With the Metal frame, command pools and buffers are the layer's own handles (M4): MoltenVK
/// never makes or sees one. A pool lists its buffers; a buffer's Metal state is its Owned entry.
constexpr uint64_t PoolTag = 0xBC10000000000000ull, BufferTag = 0xBCB0000000000000ull;
std::mutex pools_mutex;
std::unordered_map<VkCommandPool, std::vector<VkCommandBuffer>> pools;
std::atomic<uint64_t> next_pool{1}, next_command_buffer{1};
/// Likewise the objects only Metal uses with the frame: samplers (the frame makes the MTLSampler
/// from the create info), shader modules (their SPIR-V goes to the Metal builder), and the
/// layouts (no pipeline reaches MoltenVK; push descriptors and constants are encoded in Metal).
constexpr uint64_t SamplerTag = 0xBC50000000000000ull, ModuleTag = 0xBC60000000000000ull,
                   PipelineLayoutTag = 0xBC70000000000000ull, SetLayoutTag = 0xBC80000000000000ull,
                   ImageTag = 0xBC90000000000000ull, ViewTag = 0xBCA0000000000000ull,
                   MetalBufferTag = 0xBCC0000000000000ull,
                   SemaphoreTag = 0xBCD0000000000000ull,
                   FenceTag = 0xBCE0000000000000ull,
                   DescriptorPoolTag = 0xBCF0000000000000ull;
std::atomic<uint64_t> next_object{1};
template <typename H>
H OurHandle(uint64_t tag) {
    return reinterpret_cast<H>(tag | next_object++);
}
template <typename H>
bool IsOurs(H handle, uint64_t tag) {
    return (reinterpret_cast<uint64_t>(handle) & 0xFFF0000000000000ull) == tag;
}
bool IsOurCommandBuffer(VkCommandBuffer cb) {
    return (reinterpret_cast<uint64_t>(cb) & 0xFFF0000000000000ull) == BufferTag;
}
bool IsOurPool(VkCommandPool pool) {
    return (reinterpret_cast<uint64_t>(pool) & 0xFFF0000000000000ull) == PoolTag;
}
/// Drops a command buffer's encoding state: reset, freed, or its pool reset or destroyed. What it
/// encoded and never submitted is dropped with it.
void DropOwned(VkCommandBuffer cb) {
    std::scoped_lock lock{owned_mutex};
    if (auto it = owned.find(cb); it != owned.end()) {
        delete it->second;
        owned.erase(it);
        owned_count.store(unsigned(owned.size()), std::memory_order_release);
        ++owned_generation;
    }
}

/// Placeholder handles for the pipelines only the Metal path has (geometry shaders).
std::mutex placeholder_mutex;
std::vector<VkPipeline> placeholders;
std::atomic<uint64_t> next_placeholder{1};
VkPipeline PlaceholderPipeline() {
    const VkPipeline p = reinterpret_cast<VkPipeline>(0xBB00000000000000ull | next_placeholder++);
    std::scoped_lock lock{placeholder_mutex};
    placeholders.push_back(p);
    return p;
}
/// `forget`: the handle goes (vkDestroyPipeline).
bool IsPlaceholderPipeline(VkPipeline p, bool forget = true) {
    if ((reinterpret_cast<uint64_t>(p) >> 56) != 0xBB) {
        return false;
    }
    std::scoped_lock lock{placeholder_mutex};
    auto it = std::find(placeholders.begin(), placeholders.end(), p);
    if (it == placeholders.end()) {
        return false;
    }
    if (forget) {
        placeholders.erase(it);
    }
    return true;
}

void Unsupported(unsigned id) {
    ++MetalFrame::GetStats().unsupported;
    static std::array<std::atomic<bool>, Count> logged{};
    if (!logged[id].exchange(true)) {
        std::printf("Metal layer: %s is not implemented in the Metal path yet (skipped)\n",
                    Names[id]);
    }
}

/// A timeline value to signal on a semaphore's MTLSharedEvent (Metal submissions).
struct EventSignal {
    id<MTLSharedEvent> event;
    uint64_t value;
};

struct FenceState {
    std::atomic<bool> signaled{false};
};
std::mutex fences_mutex;
std::condition_variable fences_cv;
std::unordered_map<VkFence, std::shared_ptr<FenceState>> fences;

std::shared_ptr<FenceState> FenceOf(VkFence fence) {
    std::scoped_lock lock{fences_mutex};
    const auto it = fences.find(fence);
    return it == fences.end() ? nullptr : it->second;
}

void CompleteFence(id<MTLCommandBuffer> cmd, const std::shared_ptr<FenceState>& fence) {
    if (!fence) return;
    [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
      fence->signaled.store(true, std::memory_order_release);
      fences_cv.notify_all();
    }];
}

/// Commits the Metal command buffers of the owned command buffers in `buffers`, in order, and
/// releases them; MoltenVK then gets the submit unchanged (its copies of them are empty). With
/// MoltenVK's single-queue semaphores ("Metal implicit guarantees within a single queue") order
/// on the queue is what synchronizes, and ours comes first. `signals`, when the submit does not
/// go to MoltenVK (SubmitInMetal): encoded on the last one (or a command buffer of their own).
template <typename Get>
void CommitOwned(uint32_t count, Get&& get, const std::vector<EventSignal>* signals = nullptr,
                 const std::shared_ptr<FenceState>& fence = {}) {
    const bool signal = signals && !signals->empty();
    if (owned_count.load(std::memory_order_acquire) == 0 && !signal && !fence) {
        return;
    }
    // The last owned command buffer of the submit carries the signals.
    uint32_t last = count;
    if (signal || fence) {
        for (uint32_t i = count; i-- > 0;) {
            if (FindOwned(get(i))) {
                last = i;
                break;
            }
        }
        if (last == count) {
            @autoreleasepool {
                id<MTLCommandBuffer> cmd = [queue commandBuffer];
                cmd.label = @"bbport Metal submit signal";
                if (signals) {
                    for (const auto& s : *signals) {
                        [cmd encodeSignalEvent:s.event value:s.value];
                    }
                }
                CompleteFence(cmd, fence);
                [cmd commit];
            }
            return;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        const VkCommandBuffer cb = get(i);
        Owned* o = nullptr;
        {
            std::scoped_lock lock{owned_mutex};
            auto it = owned.find(cb);
            if (it == owned.end()) {
                continue;
            }
            o = it->second;
            owned.erase(it);
            owned_count.store(unsigned(owned.size()), std::memory_order_release);
            ++owned_generation;
        }
        @autoreleasepool {
        MetalFrame::EndEncoders(*o);
        if (i == last) {
            if (signals) for (const auto& s : *signals) {
                [o->cmd encodeSignalEvent:s.event value:s.value];
            }
            CompleteFence(o->cmd, fence);
        }
        MetalFrame::RetainFencesUntilDone(*o);
        MetalFrame::ReturnScratchWhenDone(*o);
        if (!o->keep.empty()) {
            // Our temporary buffers live until the GPU is done with them.
            __block std::vector<id<MTLBuffer>> keep = std::move(o->keep);
            [o->cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
              keep.clear();
            }];
        }
        [o->cmd commit];
        }
        static std::atomic<unsigned long long> committed{0};
        const auto n = ++committed;
        if (n == 1 || n % 1000 == 0) {
            std::printf("Metal layer: %llu command buffers encoded in Metal (this one: %u commands)\n",
                        n, o->commands);
        }
        delete o;
    }
}

// ---- Metal submissions (the skeleton, docs/RENDER_ADAPTER.md M2 step 4) ----
// A submit whose command buffers the Metal frame owns is committed in Metal alone: the timeline
// semaphores it signals advance on their MTLSharedEvents (MoltenVK's own, exported), so Vulkan's
// reads and waits of them (the scheduler's ticks, MoltenVK's waits) see the values; it waits on
// nothing but queue order (everything is on one Metal queue). Not taken (MoltenVK submits it as
// before): a fence, a binary semaphore, a semaphore without a shared event, or a wait for a
// value not committed yet. BB_METAL_SUBMIT=0: every submit goes to MoltenVK.
PFN_vkExportMetalObjectsEXT export_fn;
VkDevice layer_device;
std::mutex event_mutex;
std::unordered_map<VkSemaphore, id<MTLSharedEvent>> events; ///< nil: not exportable
std::unordered_map<VkSemaphore, uint64_t> committed_values; ///< highest value committed in Metal
std::unordered_map<VkSemaphore, bool> timeline_semaphores;

id<MTLSharedEvent> EventOfLocked(VkSemaphore semaphore) {
    if (auto it = events.find(semaphore); it != events.end()) {
        return it->second;
    }
    if (!export_fn) return nil;
    VkExportMetalSharedEventInfoEXT event{VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT};
    event.semaphore = semaphore;
    VkExportMetalObjectsInfoEXT info{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &event};
    export_fn(layer_device, &info);
    id<MTLSharedEvent> shared = event.mtlSharedEvent;
    events[semaphore] = shared;
    return shared;
}

void ForgetSemaphore(VkSemaphore semaphore) {
    std::scoped_lock lock{event_mutex};
    events.erase(semaphore);
    committed_values.erase(semaphore);
    timeline_semaphores.erase(semaphore);
}

const VkTimelineSemaphoreSubmitInfo* TimelineInfo(const VkSubmitInfo& submit) {
    for (auto* next = static_cast<const VkBaseInStructure*>(submit.pNext); next;
         next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
            return reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(next);
        }
    }
    return nullptr;
}

bool SubmitInMetal(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_METAL_SUBMIT");
        const bool on = !(env && env[0] == '0');
        std::printf("Metal layer: submissions %s\n",
                    on ? "committed in Metal (no MoltenVK submit for the frame's work)"
                       : "through MoltenVK (BB_METAL_SUBMIT=0)");
        return on;
    }();
    if (!enabled || (fence && !FenceOf(fence))) {
        return false;
    }
    // One at a time: a submit's values are committed before another can count on them.
    static std::mutex submit_mutex;
    std::scoped_lock submit_lock{submit_mutex};
    std::vector<std::vector<EventSignal>> signals(count);
    {
        std::scoped_lock lock{event_mutex};
        for (uint32_t s = 0; s < count; ++s) {
            const VkSubmitInfo& submit = submits[s];
            for (uint32_t i = 0; i < submit.commandBufferCount; ++i) {
                if (!FindOwned(submit.pCommandBuffers[i])) {
                    return false;
                }
            }
            const auto* timeline = TimelineInfo(submit);
            // The native Metal swapchain does not use binary semaphores. Refuse an untracked
            // binary wait until its acquisition semantics are represented by this queue.
            if ((submit.waitSemaphoreCount || submit.signalSemaphoreCount) && !timeline)
                return false;
            for (uint32_t i = 0; i < submit.waitSemaphoreCount; ++i) {
                const VkSemaphore semaphore = submit.pWaitSemaphores[i];
                id<MTLSharedEvent> event = EventOfLocked(semaphore);
                if (!event || i >= timeline->waitSemaphoreValueCount) {
                    return false;
                }
                const uint64_t value = timeline->pWaitSemaphoreValues[i];
                if (event.signaledValue < value && committed_values[semaphore] < value) {
                    return false; // its signal is not on the queue yet
                }
            }
            for (uint32_t i = 0; i < submit.signalSemaphoreCount; ++i) {
                id<MTLSharedEvent> event = EventOfLocked(submit.pSignalSemaphores[i]);
                if (!event || i >= timeline->signalSemaphoreValueCount) {
                    return false;
                }
                signals[s].push_back({event, timeline->pSignalSemaphoreValues[i]});
            }
        }
        for (uint32_t s = 0; s < count; ++s) {
            for (uint32_t i = 0; i < submits[s].signalSemaphoreCount; ++i) {
                auto& value = committed_values[submits[s].pSignalSemaphores[i]];
                value = std::max(value, TimelineInfo(submits[s])->pSignalSemaphoreValues[i]);
            }
        }
    }
    for (uint32_t s = 0; s < count; ++s) {
        CommitOwned(submits[s].commandBufferCount,
                    [&](uint32_t i) { return submits[s].pCommandBuffers[i]; }, &signals[s],
                    s + 1 == count ? FenceOf(fence) : nullptr);
    }
    ++metal_submits;
    return true;
}

/// One forwarding function per dispatcher member: counts, encodes Metal for an owned command
/// buffer, else calls MoltenVK.
template <unsigned I, typename F>
struct Hook;
template <unsigned I, typename R, typename... A>
struct Hook<I, R (*)(A...)> {
    static inline R (*original)(A...) = nullptr;

    static R Call(A... args) {
        if (trace) {
            calls[I].fetch_add(1, std::memory_order_relaxed);
        }
        if constexpr (I == Id_vkQueueSubmit) {
            Report();
            const auto [vkqueue, count, submits, fence] = std::tuple{args...};
            if (frame_enabled && SubmitInMetal(count, submits, fence)) {
                return VK_SUCCESS;
            }
            ++vulkan_submits;
            // MoltenVK gets the submit's waits, signals and fence; the layer's command buffers are
            // committed in Metal first (same queue, so in order) and left out.
            std::vector<VkSubmitInfo> stripped(submits, submits + count);
            std::vector<std::vector<VkCommandBuffer>> kept(count);
            for (uint32_t s = 0; s < count; ++s) {
                for (uint32_t i = 0; i < submits[s].commandBufferCount; ++i) {
                    if (!IsOurCommandBuffer(submits[s].pCommandBuffers[i])) {
                        kept[s].push_back(submits[s].pCommandBuffers[i]);
                    }
                }
                stripped[s].commandBufferCount = uint32_t(kept[s].size());
                stripped[s].pCommandBuffers = kept[s].data();
            }
            for (uint32_t s = 0; s < count; ++s) {
                CommitOwned(submits[s].commandBufferCount,
                            [&](uint32_t i) { return submits[s].pCommandBuffers[i]; });
            }
            return original(vkqueue, count, stripped.data(), fence);
        } else if constexpr (I == Id_vkQueueSubmit2 || I == Id_vkQueueSubmit2KHR) {
            Report();
            const auto [vkqueue, count, submits, fence] = std::tuple{args...};
            for (uint32_t s = 0; s < count; ++s) {
                CommitOwned(submits[s].commandBufferInfoCount, [&](uint32_t i) {
                    return submits[s].pCommandBufferInfos[i].commandBuffer;
                });
            }
            return original(args...);
        } else if constexpr (I == Id_vkCreateCommandPool) {
            if (!frame_enabled) {
                return original(args...);
            }
            const auto [dev, info, alloc, out] = std::tuple{args...};
            *out = reinterpret_cast<VkCommandPool>(PoolTag | next_pool++);
            std::scoped_lock lock{pools_mutex};
            pools[*out];
            return VK_SUCCESS;
        } else if constexpr (I == Id_vkDestroyCommandPool || I == Id_vkResetCommandPool) {
            const VkCommandPool pool = std::get<1>(std::tuple{args...});
            if (!IsOurPool(pool)) {
                return original(args...);
            }
            std::vector<VkCommandBuffer> buffers;
            {
                std::scoped_lock lock{pools_mutex};
                if (auto it = pools.find(pool); it != pools.end()) {
                    buffers = it->second;
                    if constexpr (I == Id_vkDestroyCommandPool) {
                        pools.erase(it);
                    }
                }
            }
            for (VkCommandBuffer cb : buffers) {
                DropOwned(cb);
            }
            if constexpr (!std::is_void_v<R>) {
                return VK_SUCCESS;
            }
        } else if constexpr (I == Id_vkAllocateCommandBuffers) {
            const auto [dev, info, out] = std::tuple{args...};
            if (!IsOurPool(info->commandPool)) {
                return original(args...);
            }
            std::scoped_lock lock{pools_mutex};
            auto& list = pools[info->commandPool];
            for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
                out[i] = reinterpret_cast<VkCommandBuffer>(BufferTag | next_command_buffer++);
                list.push_back(out[i]);
            }
            return VK_SUCCESS;
        } else if constexpr (I == Id_vkFreeCommandBuffers) {
            const auto [dev, pool, count, buffers] = std::tuple{args...};
            if (!IsOurPool(pool)) {
                return original(args...);
            }
            for (uint32_t i = 0; i < count; ++i) {
                DropOwned(buffers[i]);
            }
            std::scoped_lock lock{pools_mutex};
            if (auto it = pools.find(pool); it != pools.end()) {
                std::erase_if(it->second, [&](VkCommandBuffer cb) {
                    return std::find(buffers, buffers + count, cb) != buffers + count;
                });
            }
        } else if constexpr (I == Id_vkResetCommandBuffer) {
            const VkCommandBuffer cb = std::get<0>(std::tuple{args...});
            if (!IsOurCommandBuffer(cb)) {
                return original(args...);
            }
            DropOwned(cb);
            return VK_SUCCESS;
        } else if constexpr (I == Id_vkBeginCommandBuffer) {
            const VkCommandBuffer cb = std::get<0>(std::tuple{args...});
            if (IsOurCommandBuffer(cb)) {
                OwnBuffer(cb); // MoltenVK has no command buffer behind it
                return VK_SUCCESS;
            }
            const R result = original(args...);
            if (frame_enabled) {
                OwnBuffer(cb);
            }
            return result;
        } else if constexpr (I == Id_vkEndCommandBuffer) {
            const VkCommandBuffer cb = std::get<0>(std::tuple{args...});
            if (Owned* o = FindOwned(cb)) {
                @autoreleasepool {
                    MetalFrame::EndEncoders(*o);
                }
            }
            if (IsOurCommandBuffer(cb)) {
                return VK_SUCCESS;
            }
            return original(args...);
        } else if constexpr (I == Id_vkCreateSampler) {
            const auto [dev, info, alloc, out] = std::tuple{args...};
            if (frame_enabled) {
                *out = OurHandle<VkSampler>(SamplerTag);
                @autoreleasepool {
                    MetalFrame::SamplerCreated(*out, info);
                }
                return VK_SUCCESS;
            }
            const R result = original(args...);
            if (result == VK_SUCCESS && (present_enabled || frame_enabled)) {
                @autoreleasepool {
                    MetalFrame::SamplerCreated(*out, info);
                }
            }
            return result;
        } else if constexpr (I == Id_vkDestroyImage) {
            if (IsOurs(std::get<1>(std::tuple{args...}), ImageTag)) {
                return; // the device gives up the texture (UniqueImage::Destroy)
            }
            return original(args...);
        } else if constexpr (I == Id_vkCreateShaderModule) {
            const auto [dev, info, alloc, out] = std::tuple{args...};
            if (frame_enabled) {
                *out = OurHandle<VkShaderModule>(ModuleTag);
                MetalShadow::ModuleCreated(*out, info);
                return VK_SUCCESS;
            }
            const R result = original(args...);
            if (result == VK_SUCCESS && frame_enabled) {
                MetalShadow::ModuleCreated(*out, info);
            }
            return result;
        } else if constexpr (I == Id_vkCreateGraphicsPipelines) {
            // Pipelines with a geometry shader exist only on the Metal path (MoltenVK has no
            // geometry stage): they get a placeholder handle MoltenVK never sees.
            const auto [dev, cache, count, infos, alloc, out] = std::tuple{args...};
            R result = VK_SUCCESS;
            for (uint32_t i = 0; i < count; ++i) {
                bool geometry = false;
                for (uint32_t st = 0; st < infos[i].stageCount; ++st) {
                    geometry |= infos[i].pStages[st].stage == VK_SHADER_STAGE_GEOMETRY_BIT;
                }
                if (geometry && frame_enabled) {
                    out[i] = PlaceholderPipeline();
                } else {
                    const R one = original(dev, cache, 1, &infos[i], alloc, &out[i]);
                    if (one != VK_SUCCESS) {
                        result = one;
                        continue;
                    }
                }
                if (frame_enabled && !geometry) {
                    MetalShadow::GraphicsCreated(&infos[i], out[i]);
                }
            }
            return result;
        } else if constexpr (I == Id_vkCreateComputePipelines) {
            const auto [dev, cache, count, infos, alloc, out] = std::tuple{args...};
            const R result = original(args...);
            if (frame_enabled) {
                for (uint32_t i = 0; i < count; ++i) {
                    @autoreleasepool {
                        MetalShadow::ComputeCreated(&infos[i], out[i]);
                    }
                }
            }
            return result;
        } else if constexpr (I == Id_vkCreateImageView) {
            const auto [dev, info, alloc, out] = std::tuple{args...};
            if (frame_enabled && IsOurs(info->image, ImageTag)) {
                // A view of an image only Metal has: made in Metal too.
                const VkImageView view = OurHandle<VkImageView>(ViewTag);
                bool made = false;
                @autoreleasepool {
                    made = MetalFrame::MakeView(view, info);
                }
                if (!made) {
                    std::printf("Metal layer: no Metal view of format %d (type %d) for an image "
                                "only Metal has\n",
                                int(info->format), int(info->viewType));
                    return VK_ERROR_FORMAT_NOT_SUPPORTED;
                }
                *out = view;
                return VK_SUCCESS;
            }
            const R result = original(args...);
            if (result == VK_SUCCESS && (present_enabled || frame_enabled)) {
                MetalFrame::ImageViewCreated(*out, info);
            }
            return result;
        } else if constexpr (I == Id_vkSetDebugUtilsObjectNameEXT) {
            const auto info = std::get<1>(std::tuple{args...});
            if (frame_enabled) {
                return VK_SUCCESS; // names only label MoltenVK's objects, which the frame does not use
            }
            if (info && info->objectType == VK_OBJECT_TYPE_PIPELINE &&
                IsPlaceholderPipeline(reinterpret_cast<VkPipeline>(info->objectHandle), false)) {
                return VK_SUCCESS; // MoltenVK does not know the placeholder
            }
            if (info && (info->objectType == VK_OBJECT_TYPE_COMMAND_POOL ||
                         info->objectType == VK_OBJECT_TYPE_COMMAND_BUFFER) &&
                (IsOurPool(reinterpret_cast<VkCommandPool>(info->objectHandle)) ||
                 IsOurCommandBuffer(reinterpret_cast<VkCommandBuffer>(info->objectHandle)))) {
                return VK_SUCCESS; // likewise the layer's command pools and buffers
            }
            return original(args...);
        } else if constexpr (I == Id_vkGetDeviceProcAddr || I == Id_vkGetInstanceProcAddr) {
            const char* name = std::get<1>(std::tuple{args...});
            if (PFN_vkVoidFunction ours = HookedByName(name)) {
                return ours;
            }
            if (frame_enabled && name && std::strncmp(name, "vkCmd", 5) == 0) {
                // A command the layer does not cover: it would reach MoltenVK out of order.
                static std::mutex logged_mutex;
                static std::vector<std::string> logged;
                std::scoped_lock lock{logged_mutex};
                if (std::find(logged.begin(), logged.end(), name) == logged.end()) {
                    logged.emplace_back(name);
                    std::printf("Metal layer: %s requested but not covered by the Metal path\n", name);
                }
            }
            return original(args...);
        } else if constexpr (I == Id_vkUpdateDescriptorSets) {
            original(args...);
            if (frame_enabled) {
                MetalFrame::DescriptorSetsUpdated(std::get<1>(std::tuple{args...}),
                                                  std::get<2>(std::tuple{args...}));
            }
            return;
        } else if constexpr (I == Id_vkDestroySampler) {
            const VkSampler sampler = std::get<1>(std::tuple{args...});
            if (present_enabled || frame_enabled) {
                MetalFrame::SamplerDestroyed(sampler);
            }
            if (IsOurs(sampler, SamplerTag)) {
                return;
            }
            return original(args...);
        } else if constexpr (I == Id_vkDestroyShaderModule) {
            const VkShaderModule module = std::get<1>(std::tuple{args...});
            if (IsOurs(module, ModuleTag)) {
                MetalShadow::ForgetModule(vk::ShaderModule{module});
                return;
            }
            return original(args...);
        } else if constexpr (I == Id_vkCreatePipelineLayout || I == Id_vkCreateDescriptorSetLayout) {
            if (!frame_enabled) {
                return original(args...);
            }
            auto* out = std::get<3>(std::tuple{args...});
            using Handle = std::remove_pointer_t<decltype(out)>;
            *out = OurHandle<Handle>(I == Id_vkCreatePipelineLayout ? PipelineLayoutTag : SetLayoutTag);
            return VK_SUCCESS;
        } else if constexpr (I == Id_vkDestroyPipelineLayout || I == Id_vkDestroyDescriptorSetLayout) {
            const auto handle = std::get<1>(std::tuple{args...});
            if (IsOurs(handle, I == Id_vkDestroyPipelineLayout ? PipelineLayoutTag : SetLayoutTag)) {
                return;
            }
            return original(args...);
        } else if constexpr (I == Id_vkCreateSemaphore) {
            if (!frame_enabled) return original(args...);
            const auto [dev, info, alloc, out] = std::tuple{args...};
            VkSemaphoreType type = VK_SEMAPHORE_TYPE_BINARY;
            uint64_t initial = 0;
            for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next;
                 next = next->pNext) {
                if (next->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) {
                    const auto* ti = reinterpret_cast<const VkSemaphoreTypeCreateInfo*>(next);
                    type = ti->semaphoreType;
                    initial = ti->initialValue;
                }
            }
            id<MTLSharedEvent> event = [queue.device newSharedEvent];
            if (!event) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            event.signaledValue = initial;
            *out = OurHandle<VkSemaphore>(SemaphoreTag);
            {
                std::scoped_lock lock{event_mutex};
                events[*out] = event;
                committed_values[*out] = initial;
                timeline_semaphores[*out] = type == VK_SEMAPHORE_TYPE_TIMELINE;
            }
            return VK_SUCCESS;
        } else if constexpr (I == Id_vkDestroySemaphore) {
            const VkSemaphore sem = std::get<1>(std::tuple{args...});
            if (IsOurs(sem, SemaphoreTag)) {
                ForgetSemaphore(sem);
                return;
            }
            if (frame_enabled) ForgetSemaphore(sem);
            return original(args...);
        } else if constexpr (I == Id_vkGetSemaphoreCounterValue) {
            const auto [dev, sem, out] = std::tuple{args...};
            if (!IsOurs(sem, SemaphoreTag)) return original(args...);
            std::scoped_lock lock{event_mutex};
            auto it = events.find(sem);
            if (it == events.end()) return VK_ERROR_DEVICE_LOST;
            *out = it->second.signaledValue;
            return VK_SUCCESS;
        } else if constexpr (I == Id_vkWaitSemaphores) {
            const auto [dev, info, timeout] = std::tuple{args...};
            if (!frame_enabled) return original(args...);
            const auto start = std::chrono::steady_clock::now();
            for (uint32_t i = 0; i < info->semaphoreCount; ++i) {
                void* event = SemaphoreEvent(info->pSemaphores[i]);
                if (!event) return VK_ERROR_DEVICE_LOST;
                uint64_t remaining = timeout;
                if (timeout != ~0ull) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start).count();
                    remaining = elapsed >= 0 && uint64_t(elapsed) < timeout
                                    ? timeout - uint64_t(elapsed) : 0;
                }
                if (!WaitEvent(event, info->pValues[i], remaining)) return VK_TIMEOUT;
            }
            return VK_SUCCESS;
        } else if constexpr (I == Id_vkSignalSemaphore) {
            const auto [dev, info] = std::tuple{args...};
            if (!IsOurs(info->semaphore, SemaphoreTag)) return original(args...);
            std::scoped_lock lock{event_mutex};
            auto it = events.find(info->semaphore);
            if (it == events.end()) return VK_ERROR_DEVICE_LOST;
            it->second.signaledValue = info->value;
            committed_values[info->semaphore] = std::max(committed_values[info->semaphore], info->value);
            return VK_SUCCESS;
        } else if constexpr (I == Id_vkCreateFence) {
            if (!frame_enabled) return original(args...);
            const auto [dev, info, alloc, out] = std::tuple{args...};
            *out = OurHandle<VkFence>(FenceTag);
            auto state = std::make_shared<FenceState>();
            state->signaled.store(bool(info->flags & VK_FENCE_CREATE_SIGNALED_BIT));
            std::scoped_lock lock{fences_mutex};
            fences[*out] = std::move(state);
            return VK_SUCCESS;
        } else if constexpr (I == Id_vkDestroyFence) {
            const VkFence fence = std::get<1>(std::tuple{args...});
            if (!IsOurs(fence, FenceTag)) return original(args...);
            std::scoped_lock lock{fences_mutex};
            fences.erase(fence);
            return;
        } else if constexpr (I == Id_vkGetFenceStatus) {
            const VkFence fence = std::get<1>(std::tuple{args...});
            if (!IsOurs(fence, FenceTag)) return original(args...);
            auto state = FenceOf(fence);
            return state && state->signaled.load(std::memory_order_acquire) ? VK_SUCCESS
                                                                             : VK_NOT_READY;
        } else if constexpr (I == Id_vkResetFences) {
            const auto [dev, count, handles] = std::tuple{args...};
            if (!frame_enabled) return original(args...);
            for (uint32_t i = 0; i < count; ++i) {
                auto state = FenceOf(handles[i]);
                if (!state) return VK_ERROR_DEVICE_LOST;
                state->signaled.store(false, std::memory_order_release);
            }
            return VK_SUCCESS;
        } else if constexpr (I == Id_vkWaitForFences) {
            const auto [dev, count, handles, all, timeout] = std::tuple{args...};
            if (!frame_enabled) return original(args...);
            std::vector<std::shared_ptr<FenceState>> states;
            states.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                auto state = FenceOf(handles[i]);
                if (!state) return VK_ERROR_DEVICE_LOST;
                states.push_back(std::move(state));
            }
            const auto ready = [&] {
                uint32_t signaled = 0;
                for (const auto& state : states) {
                    signaled += state->signaled.load(std::memory_order_acquire);
                }
                return all ? signaled == count : signaled != 0;
            };
            if (ready()) return VK_SUCCESS;
            if (timeout == 0) return VK_TIMEOUT;
            std::unique_lock lock{fences_mutex};
            if (timeout == ~0ull) {
                fences_cv.wait(lock, ready);
                return VK_SUCCESS;
            }
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::nanoseconds(std::min<uint64_t>(timeout, INT64_MAX));
            return fences_cv.wait_until(lock, deadline, ready) ? VK_SUCCESS : VK_TIMEOUT;
        } else if constexpr (I == Id_vkCreateDescriptorPool) {
            if (!frame_enabled) return original(args...);
            *std::get<3>(std::tuple{args...}) = OurHandle<VkDescriptorPool>(DescriptorPoolTag);
            return VK_SUCCESS;
        } else if constexpr (I == Id_vkDestroyDescriptorPool) {
            const VkDescriptorPool pool = std::get<1>(std::tuple{args...});
            if (IsOurs(pool, DescriptorPoolTag)) return;
            return original(args...);
        } else if constexpr (I == Id_vkDestroyImageView) {
            const VkImageView view = std::get<1>(std::tuple{args...});
            if (present_enabled || frame_enabled) {
                MetalFrame::ImageViewDestroyed(view);
            }
            if (IsOurs(view, ViewTag)) {
                return;
            }
            return original(args...);
        } else if constexpr (I == Id_vkDestroyPipeline) {
            const VkPipeline pipeline = std::get<1>(std::tuple{args...});
            MetalShadow::Forget(pipeline);
            if (IsPlaceholderPipeline(pipeline)) {
                return;
            }
            return original(args...);
        } else if constexpr ((I == Id_vkGetBufferDeviceAddress || I == Id_vkGetBufferDeviceAddressKHR)) {
            if (IsOurs(std::get<1>(std::tuple{args...})->buffer, MetalBufferTag)) {
                return R(MetalOnlyBufferAddress(std::get<1>(std::tuple{args...})->buffer));
            }
            const R address = original(args...);
            if (frame_enabled) {
                MetalFrame::DeviceAddressTaken(std::get<1>(std::tuple{args...})->buffer);
            }
            return address;
        } else {
            // vkCmd*: the first argument is the command buffer.
            if (Owned* o = FindOwned(std::get<0>(std::tuple{args...}))) {
                ++o->commands;
                // The recorder threads never drain an autorelease pool of their own: Metal's
                // autoreleased objects (pass descriptors, encoders) leaked ~17 KB per frame.
                @autoreleasepool {
                    Encode(*o, args...);
                }
                if constexpr (!std::is_void_v<R>) {
                    return R{};
                } else {
                    return;
                }
            }
            return original(args...);
        }
    }

    /// The Metal encoding of each command (mtl_frame.mm).
    static void Encode(Owned& o, A... args) {
        namespace F = MetalFrame;
        const auto t = std::tuple{args...};
        if constexpr (I == Id_vkCmdPipelineBarrier || I == Id_vkCmdPipelineBarrier2 ||
                      I == Id_vkCmdPipelineBarrier2KHR) {
            F::Barrier(o);
        } else if constexpr (I == Id_vkCmdBeginDebugUtilsLabelEXT ||
                             I == Id_vkCmdEndDebugUtilsLabelEXT ||
                             I == Id_vkCmdInsertDebugUtilsLabelEXT || I == Id_vkCmdSetLineWidth ||
                             I == Id_vkCmdSetPrimitiveRestartEnable ||
                             I == Id_vkCmdSetDepthBoundsTestEnable || I == Id_vkCmdSetDepthBounds ||
                             I == Id_vkCmdWriteTimestamp || I == Id_vkCmdWriteTimestamp2 ||
                             I == Id_vkCmdBeginQuery || I == Id_vkCmdEndQuery ||
                             I == Id_vkCmdResetQueryPool || I == Id_vkCmdCopyQueryPoolResults) {
            // Nothing in Metal (primitive restart is always on for strips; no depth bounds test;
            // no occlusion queries or timestamps: their results stay as they were).
        } else if constexpr (I == Id_vkCmdBeginRendering || I == Id_vkCmdBeginRenderingKHR) {
            F::BeginRendering(o, std::get<1>(t));
        } else if constexpr (I == Id_vkCmdEndRendering || I == Id_vkCmdEndRenderingKHR) {
            F::EndRendering(o);
        } else if constexpr (I == Id_vkCmdBindPipeline) {
            F::BindPipeline(o, std::get<1>(t), std::get<2>(t));
        } else if constexpr (I == Id_vkCmdPushDescriptorSetKHR) {
            F::PushDescriptorSet(o, std::get<1>(t), std::get<3>(t), std::get<4>(t), std::get<5>(t));
        } else if constexpr (I == Id_vkCmdBindDescriptorSets) {
            F::BindDescriptorSets(o, std::get<1>(t), std::get<3>(t), std::get<4>(t), std::get<5>(t));
        } else if constexpr (I == Id_vkCmdPushConstants) {
            F::PushConstants(o, std::get<3>(t), std::get<4>(t), std::get<5>(t));
        } else if constexpr (I == Id_vkCmdBindVertexBuffers) {
            F::BindVertexBuffers(o, std::get<1>(t), std::get<2>(t), std::get<3>(t),
                                 std::get<4>(t), nullptr);
        } else if constexpr (I == Id_vkCmdBindVertexBuffers2 || I == Id_vkCmdBindVertexBuffers2EXT) {
            F::BindVertexBuffers(o, std::get<1>(t), std::get<2>(t), std::get<3>(t),
                                 std::get<4>(t), std::get<6>(t));
        } else if constexpr (I == Id_vkCmdBindIndexBuffer) {
            F::BindIndexBuffer(o, std::get<1>(t), std::get<2>(t), std::get<3>(t));
        } else if constexpr (I == Id_vkCmdSetViewport) {
            F::SetViewports(o, std::get<2>(t), std::get<3>(t));
        } else if constexpr (I == Id_vkCmdSetViewportWithCount || I == Id_vkCmdSetViewportWithCountEXT) {
            F::SetViewports(o, std::get<1>(t), std::get<2>(t));
        } else if constexpr (I == Id_vkCmdSetScissor) {
            F::SetScissors(o, std::get<2>(t), std::get<3>(t));
        } else if constexpr (I == Id_vkCmdSetScissorWithCount || I == Id_vkCmdSetScissorWithCountEXT) {
            F::SetScissors(o, std::get<1>(t), std::get<2>(t));
        } else if constexpr (I == Id_vkCmdSetCullMode || I == Id_vkCmdSetCullModeEXT) {
            o.cull_mode = std::get<1>(t);
        } else if constexpr (I == Id_vkCmdSetFrontFace || I == Id_vkCmdSetFrontFaceEXT) {
            o.front_face = std::get<1>(t);
        } else if constexpr (I == Id_vkCmdSetDepthTestEnable || I == Id_vkCmdSetDepthTestEnableEXT) {
            o.depth_test = std::get<1>(t);
        } else if constexpr (I == Id_vkCmdSetDepthWriteEnable || I == Id_vkCmdSetDepthWriteEnableEXT) {
            o.depth_write = std::get<1>(t);
        } else if constexpr (I == Id_vkCmdSetDepthCompareOp || I == Id_vkCmdSetDepthCompareOpEXT) {
            o.depth_compare = std::get<1>(t);
        } else if constexpr (I == Id_vkCmdSetDepthBiasEnable) {
            o.depth_bias_enable = std::get<1>(t);
        } else if constexpr (I == Id_vkCmdSetDepthBias) {
            o.depth_bias[0] = std::get<1>(t);
            o.depth_bias[1] = std::get<2>(t);
            o.depth_bias[2] = std::get<3>(t);
        } else if constexpr (I == Id_vkCmdSetStencilTestEnable || I == Id_vkCmdSetStencilTestEnableEXT) {
            o.stencil_test = std::get<1>(t);
        } else if constexpr (I == Id_vkCmdSetStencilOp || I == Id_vkCmdSetStencilOpEXT) {
            const VkStencilFaceFlags faces = std::get<1>(t);
            for (auto* f : {&o.front, &o.back}) {
                if (faces & (f == &o.front ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT)) {
                    f->fail = std::get<2>(t);
                    f->pass = std::get<3>(t);
                    f->depth_fail = std::get<4>(t);
                    f->compare = std::get<5>(t);
                }
            }
        } else if constexpr (I == Id_vkCmdSetStencilReference || I == Id_vkCmdSetStencilWriteMask ||
                             I == Id_vkCmdSetStencilCompareMask) {
            const VkStencilFaceFlags faces = std::get<1>(t);
            const uint32_t value = std::get<2>(t);
            for (auto* f : {&o.front, &o.back}) {
                if (faces & (f == &o.front ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT)) {
                    if constexpr (I == Id_vkCmdSetStencilReference) {
                        f->reference = value;
                    } else if constexpr (I == Id_vkCmdSetStencilWriteMask) {
                        f->write_mask = value;
                    } else {
                        f->compare_mask = value;
                    }
                }
            }
        } else if constexpr (I == Id_vkCmdSetBlendConstants) {
            std::copy_n(std::get<1>(t), 4, o.blend_constants);
        } else if constexpr (I == Id_vkCmdSetRasterizerDiscardEnable) {
            o.rasterizer_discard = std::get<1>(t);
        } else if constexpr (I == Id_vkCmdDraw) {
            F::Draw(o, std::get<1>(t), std::get<2>(t), std::get<3>(t), std::get<4>(t));
        } else if constexpr (I == Id_vkCmdDrawIndexed) {
            F::DrawIndexed(o, std::get<1>(t), std::get<2>(t), std::get<3>(t), std::get<4>(t),
                           std::get<5>(t));
        } else if constexpr (I == Id_vkCmdDrawIndirect) {
            F::DrawIndirect(o, std::get<1>(t), std::get<2>(t), std::get<3>(t), std::get<4>(t), false);
        } else if constexpr (I == Id_vkCmdDrawIndexedIndirect) {
            F::DrawIndirect(o, std::get<1>(t), std::get<2>(t), std::get<3>(t), std::get<4>(t), true);
        } else if constexpr (I == Id_vkCmdDispatch) {
            F::Dispatch(o, std::get<1>(t), std::get<2>(t), std::get<3>(t));
        } else if constexpr (I == Id_vkCmdDispatchIndirect) {
            F::DispatchIndirect(o, std::get<1>(t), std::get<2>(t));
        } else if constexpr (I == Id_vkCmdCopyBuffer) {
            F::CopyBuffer(o, std::get<1>(t), std::get<2>(t), std::get<3>(t), std::get<4>(t));
        } else if constexpr (I == Id_vkCmdFillBuffer) {
            F::FillBuffer(o, std::get<1>(t), std::get<2>(t), std::get<3>(t), std::get<4>(t));
        } else if constexpr (I == Id_vkCmdUpdateBuffer) {
            F::UpdateBuffer(o, std::get<1>(t), std::get<2>(t), std::get<3>(t), std::get<4>(t));
        } else if constexpr (I == Id_vkCmdCopyImage) {
            F::CopyImage(o, std::get<1>(t), std::get<3>(t), std::get<5>(t), std::get<6>(t));
        } else if constexpr (I == Id_vkCmdCopyBufferToImage) {
            F::CopyBufferToImage(o, std::get<1>(t), std::get<2>(t), std::get<4>(t), std::get<5>(t));
        } else if constexpr (I == Id_vkCmdCopyImageToBuffer) {
            F::CopyImageToBuffer(o, std::get<1>(t), std::get<3>(t), std::get<4>(t), std::get<5>(t));
        } else if constexpr (I == Id_vkCmdClearColorImage) {
            F::ClearColorImage(o, std::get<1>(t), std::get<3>(t), std::get<4>(t), std::get<5>(t));
        } else if constexpr (I == Id_vkCmdClearDepthStencilImage) {
            F::ClearDepthStencilImage(o, std::get<1>(t), std::get<3>(t), std::get<4>(t),
                                      std::get<5>(t));
        } else if constexpr (I == Id_vkCmdBlitImage) {
            F::BlitImage(o, std::get<1>(t), std::get<3>(t), std::get<5>(t), std::get<6>(t),
                         std::get<7>(t));
        } else {
            Unsupported(I);
        }
    }

    static void Install(R (*&slot)(A...)) {
        if (slot) {
            original = slot;
            slot = &Call;
        }
    }
};

} // namespace

bool PresentEnabled() {
    return present_enabled && !frame_enabled;
}

bool FrameEnabled() {
    return frame_enabled;
}

unsigned long long ImageOfView(unsigned long long view) {
    return (unsigned long long)MetalFrame::ImageOfView(VkImageView(view));
}

void* MetalOnlyPipeline(const void* graphics_info, const void* compute_info) {
    const VkPipeline pipeline = PlaceholderPipeline();
    @autoreleasepool {
        if (const auto* info = static_cast<const VkGraphicsPipelineCreateInfo*>(graphics_info)) {
            bool geometry = false;
            for (uint32_t st = 0; st < info->stageCount; ++st) {
                geometry |= info->pStages[st].stage == VK_SHADER_STAGE_GEOMETRY_BIT;
            }
            if (!geometry) {
                MetalShadow::GraphicsCreated(info, pipeline);
            }
        } else if (const auto* info =
                       static_cast<const VkComputePipelineCreateInfo*>(compute_info)) {
            MetalShadow::ComputeCreated(info, pipeline);
        }
    }
    return pipeline;
}

MetalFrame::Owned* OwnedFor(void* command_buffer) {
    return FindOwned(static_cast<VkCommandBuffer>(command_buffer));
}

void* MetalCommandBuffer(void* command_buffer) {
    Owned* o = FindOwned(static_cast<VkCommandBuffer>(command_buffer));
    if (!o) {
        return nullptr;
    }
    MetalFrame::EndEncoders(*o);
    return (__bridge void*)o->cmd;
}

void* MetalImageTexture(void* image) {
    return (__bridge void*)MetalFrame::ImageTexture(reinterpret_cast<VkImage>(image));
}

void* MtlDevice() {
    return queue ? (__bridge void*)queue.device : nullptr;
}

void* MtlQueue() {
    return (__bridge void*)queue;
}

void SetDevice(void* device, void* vkqueue) {
    if (!present_enabled && !frame_enabled) {
        return;
    }
    const VkDevice vk_device = static_cast<VkDevice>(device);
    if (NativeBoot()) {
        id<MTLDevice> metal = MTLCreateSystemDefaultDevice();
        queue = [metal newCommandQueue];
        if (!queue) {
            std::printf("Metal backend: failed to make a Metal command queue\n");
            frame_enabled = false;
            return;
        }
        queue.label = @"bloodborne Metal frame";
        export_fn = nullptr;
        layer_device = vk_device;
        MetalFrame::Init(vk_device, queue, nullptr, nullptr);
        std::printf("Metal backend: native device and queue on %s\n", metal.name.UTF8String);
        return;
    }
    auto export_objects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(vk_device, "vkExportMetalObjectsEXT"));
    if (!export_objects) {
        std::printf("Metal layer: no VK_EXT_metal_objects; the Metal path stays off\n");
        present_enabled = frame_enabled = false;
        return;
    }
    VkExportMetalCommandQueueInfoEXT q{VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT};
    q.queue = static_cast<VkQueue>(vkqueue);
    VkExportMetalObjectsInfoEXT info{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &q};
    export_objects(vk_device, &info);
    queue = q.mtlCommandQueue;
    if (!queue) {
        std::printf("Metal layer: MoltenVK's queue could not be exported; the Metal path stays off\n");
        present_enabled = frame_enabled = false;
        return;
    }
    // The original (MoltenVK) entry: through the hooks the lookup would come back to the layer
    // (vkGetDeviceProcAddr answers with them) and recurse.
    using AddressHook = Hook<Id_vkGetBufferDeviceAddress,
                             decltype(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferDeviceAddress)>;
    auto get_address = AddressHook::original
                           ? AddressHook::original
                           : reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
                                 VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferDeviceAddress);
    MetalFrame::Init(vk_device, queue, export_objects, get_address);
    export_fn = export_objects;
    layer_device = vk_device;
    std::printf("Metal layer: %s encoded in Metal on %s\n",
                frame_enabled ? "every command buffer is (BB_METAL_FRAME=1)"
                              : "the present pass is (BB_METAL_PRESENT=1)",
                queue.device.name.UTF8String);
}

void Own(void* command_buffer) {
    if (!PresentEnabled()) {
        return; // frame mode owns every command buffer at vkBeginCommandBuffer
    }
    OwnBuffer(static_cast<VkCommandBuffer>(command_buffer));
}

void* MetalOnlyBuffer(void* buffer) {
    if (!frame_enabled || !buffer) {
        return nullptr;
    }
    const VkBuffer handle = OurHandle<VkBuffer>(MetalBufferTag);
    MetalFrame::MetalBufferMade(handle, (__bridge id<MTLBuffer>)buffer);
    return handle;
}

unsigned long long MetalOnlyBufferAddress(void* buffer) {
    const VkBuffer handle = static_cast<VkBuffer>(buffer);
    @autoreleasepool {
        MetalFrame::DeviceAddressTaken(handle);
        return MetalFrame::BufferAddress(handle);
    }
}

void BufferMade(void* buffer) {
    if (frame_enabled && buffer && !IsOurs(static_cast<VkBuffer>(buffer), MetalBufferTag)) {
        @autoreleasepool {
            MetalFrame::BufferMade(static_cast<VkBuffer>(buffer));
        }
    }
}

void BufferGone(void* buffer) {
    if (frame_enabled && buffer) {
        MetalFrame::BufferGone(static_cast<VkBuffer>(buffer));
    }
}

void* MetalOnlyImage(void* texture) {
    if (!frame_enabled || !texture) {
        return nullptr;
    }
    const VkImage image = OurHandle<VkImage>(ImageTag);
    MetalFrame::ImageMade(image, (__bridge id<MTLTexture>)texture);
    return image;
}

bool IsMetalOnlyImage(void* image) {
    return IsOurs(static_cast<VkImage>(image), ImageTag);
}

void ImageMade(void* image) {
    if (frame_enabled && image && !IsMetalOnlyImage(image)) {
        @autoreleasepool {
            MetalFrame::ImageMade(static_cast<VkImage>(image), nil);
        }
    }
}

void ImageGone(void* image) {
    if (frame_enabled && image) {
        MetalFrame::ImageGone(static_cast<VkImage>(image));
    }
}

void* SemaphoreEvent(void* semaphore) {
    if (!frame_enabled || !semaphore) {
        return nullptr;
    }
    std::scoped_lock lock{event_mutex};
    @autoreleasepool {
        return (__bridge void*)EventOfLocked(static_cast<VkSemaphore>(semaphore));
    }
}

unsigned long long EventValue(void* event) {
    return [(__bridge id<MTLSharedEvent>)event signaledValue];
}

bool WaitEvent(void* event, unsigned long long value, unsigned long long timeout_ns) {
    id<MTLSharedEvent> shared = (__bridge id<MTLSharedEvent>)event;
    if (shared.signaledValue >= value) {
        return true;
    }
    if (timeout_ns == 0) {
        return false;
    }
    static MTLSharedEventListener* listener = [[MTLSharedEventListener alloc]
        initWithDispatchQueue:dispatch_queue_create("bbport.ticks", DISPATCH_QUEUE_CONCURRENT)];
    dispatch_semaphore_t reached = dispatch_semaphore_create(0);
    @autoreleasepool {
        [shared notifyListener:listener
                       atValue:value
                         block:^(id<MTLSharedEvent>, uint64_t) {
                           dispatch_semaphore_signal(reached);
                         }];
    }
    if (timeout_ns != ~0ull) {
        // A late signal still runs the block; it signals a semaphore nobody waits on.
        return dispatch_semaphore_wait(reached, dispatch_time(DISPATCH_TIME_NOW,
                                                              int64_t(std::min<unsigned long long>(
                                                                  timeout_ns, INT64_MAX)))) == 0;
    }
    bool reported = false;
    while (dispatch_semaphore_wait(reached, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC))) {
        if (!reported) {
            std::printf("Metal layer: still waiting for tick %llu (at %llu) after 10 s\n", value,
                        shared.signaledValue);
            reported = true;
        }
    }
    return true;
}

bool MemoryUsage(unsigned long long* used, unsigned long long* budget) {
    if (!frame_enabled) {
        return false;
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)MtlDevice();
    if (!device) {
        return false;
    }
    *used = device.currentAllocatedSize;
    *budget = device.recommendedMaxWorkingSetSize;
    return true;
}

void Install(void* dispatcher_ptr) {
    const char* env = std::getenv("BB_METAL_TRACE");
    trace = env && env[0] == '1';
    const char* present = std::getenv("BB_METAL_PRESENT");
    present_enabled = present && present[0] == '1';
    const char* frame = std::getenv("BB_METAL_FRAME");
    frame_enabled = frame && frame[0] == '1';
    const char* inventory_env = std::getenv("BB_METAL_INVENTORY");
    inventory = inventory_env && inventory_env[0] == '1';
    if (!trace && !present_enabled && !frame_enabled && !inventory) {
        return;
    }
    auto& d = *static_cast<Dispatcher*>(dispatcher_ptr);
    if (inventory) {
        // Below the hooks: their `original` is then the counting wrapper.
#define BB_INVENTORY(name) Passthrough<__COUNTER__, decltype(d.name)>::Install(d.name, #name);
#include "video_core/renderer_metal/mtl_vk_inventory.inc"
#undef BB_INVENTORY
        std::printf("Metal inventory: counting the calls of %zu Vulkan functions that reach the driver\n",
                    inventory_entries.size());
    }
#define BB_INSTALL(name)                                                                           \
    Hook<Id_##name, decltype(d.name)>::Install(d.name);                                            \
    hooked[Id_##name] = Hook<Id_##name, decltype(d.name)>::original                                \
                            ? reinterpret_cast<PFN_vkVoidFunction>(d.name)                         \
                            : nullptr;
    BB_LAYER_FUNCTIONS(BB_INSTALL)
#undef BB_INSTALL
    std::printf("Metal layer: installed over %u Vulkan functions\n", unsigned(Count));
}

namespace {
constexpr uint64_t NativeInstanceTag = 0xBC01000000000001ull;
constexpr uint64_t NativePhysicalTag = 0xBC02000000000001ull;
constexpr uint64_t NativeDeviceTag = 0xBC03000000000001ull;
constexpr uint64_t NativeQueueTag = 0xBC04000000000001ull;

VKAPI_ATTR VkResult VKAPI_CALL NativeEnumerateVersion(uint32_t* version) {
    *version = VK_API_VERSION_1_3;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL NativeEnumerateInstanceExtensions(
    const char*, uint32_t* count, VkExtensionProperties* out) {
    static constexpr const char* names[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME,
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
        VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME};
    constexpr uint32_t total = std::size(names);
    if (!out) { *count = total; return VK_SUCCESS; }
    const uint32_t n = std::min(*count, total);
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = {};
        std::strncpy(out[i].extensionName, names[i], VK_MAX_EXTENSION_NAME_SIZE - 1);
        out[i].specVersion = 1;
    }
    *count = n;
    return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL NativeEnumerateLayers(uint32_t* count,
                                                      VkLayerProperties*) {
    *count = 0;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL NativeCreateInstance(const VkInstanceCreateInfo*,
                                                     const VkAllocationCallbacks*, VkInstance* out) {
    *out = reinterpret_cast<VkInstance>(NativeInstanceTag);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL NativeDestroyInstance(VkInstance, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL NativeEnumeratePhysicalDevices(VkInstance, uint32_t* count,
                                                               VkPhysicalDevice* out) {
    if (!out) { *count = 1; return VK_SUCCESS; }
    if (*count == 0) return VK_INCOMPLETE;
    out[0] = reinterpret_cast<VkPhysicalDevice>(NativePhysicalTag);
    *count = 1;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL NativeCreateDevice(VkPhysicalDevice,
                                                   const VkDeviceCreateInfo*,
                                                   const VkAllocationCallbacks*, VkDevice* out) {
    *out = reinterpret_cast<VkDevice>(NativeDeviceTag);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL NativeDestroyDevice(VkDevice, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL NativeGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue* out) {
    *out = reinterpret_cast<VkQueue>(NativeQueueTag);
}
VKAPI_ATTR VkResult VKAPI_CALL NativeWaitIdle(VkDevice) {
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (!cmd) return VK_ERROR_DEVICE_LOST;
    [cmd commit];
    [cmd waitUntilCompleted];
    return cmd.status == MTLCommandBufferStatusCompleted ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}
VKAPI_ATTR VkResult VKAPI_CALL NativeQueueWaitIdle(VkQueue) {
    return NativeWaitIdle(nullptr);
}

template <unsigned I, typename F> struct NativeFallback;
template <unsigned I, typename R, typename... A>
struct NativeFallback<I, R (*)(A...)> {
    static R Call(A...) {
        static std::atomic<bool> once{false};
        if (!once.exchange(true))
            std::fprintf(stderr, "Metal backend: unimplemented entry point %s\n", Names[I]);
        if constexpr (std::is_same_v<R, VkResult>) return VK_ERROR_FEATURE_NOT_PRESENT;
        else if constexpr (!std::is_void_v<R>) return R{};
    }
};

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL NativeGetProc(VkInstance, const char*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL NativeGetDeviceProc(VkDevice, const char* name) {
    return NativeGetProc(nullptr, name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL NativeGetProc(VkInstance, const char* name) {
    if (!name) return nullptr;
#define BB_NATIVE_PROC(symbol, fn) \
    if (std::strcmp(name, "vk" #symbol) == 0) \
        return reinterpret_cast<PFN_vkVoidFunction>(fn);
    BB_NATIVE_PROC(GetInstanceProcAddr, NativeGetProc)
    BB_NATIVE_PROC(GetDeviceProcAddr, NativeGetDeviceProc)
    BB_NATIVE_PROC(EnumerateInstanceVersion, NativeEnumerateVersion)
    BB_NATIVE_PROC(EnumerateInstanceExtensionProperties, NativeEnumerateInstanceExtensions)
    BB_NATIVE_PROC(EnumerateInstanceLayerProperties, NativeEnumerateLayers)
    BB_NATIVE_PROC(CreateInstance, NativeCreateInstance)
    BB_NATIVE_PROC(DestroyInstance, NativeDestroyInstance)
    BB_NATIVE_PROC(EnumeratePhysicalDevices, NativeEnumeratePhysicalDevices)
    BB_NATIVE_PROC(CreateDevice, NativeCreateDevice)
    BB_NATIVE_PROC(DestroyDevice, NativeDestroyDevice)
    BB_NATIVE_PROC(GetDeviceQueue, NativeGetDeviceQueue)
    BB_NATIVE_PROC(DeviceWaitIdle, NativeWaitIdle)
    BB_NATIVE_PROC(QueueWaitIdle, NativeQueueWaitIdle)
#undef BB_NATIVE_PROC
    if (auto proc = MetalCaps::GetProc(name))
        return reinterpret_cast<PFN_vkVoidFunction>(proc);
#define BB_NATIVE_FALLBACK(symbol) \
    if (std::strcmp(name, #symbol) == 0) \
        return reinterpret_cast<PFN_vkVoidFunction>( \
            &NativeFallback<Id_##symbol, decltype(std::declval<Dispatcher>().symbol)>::Call);
    BB_LAYER_FUNCTIONS(BB_NATIVE_FALLBACK)
#undef BB_NATIVE_FALLBACK
    return nullptr;
}
} // namespace

bool NativeBoot() {
    const char* frame = std::getenv("BB_METAL_FRAME");
    const char* native = std::getenv("BB_METAL_NATIVE_BOOT");
    return frame && frame[0] == '1' && native && native[0] == '1';
}

void* NativeInstanceProcAddr() {
    return reinterpret_cast<void*>(NativeGetProc);
}

} // namespace MetalLayer
