// SPDX-License-Identifier: GPL-2.0-or-later
// From Wooloomooloo2/bloodborne_mac_port (78b59f5, GPL-2.0-or-later), adapted to bloodborne-runtime.
// bbport: the Metal back end's entry into the Vulkan renderer (docs/DESIGN_METAL_BACKEND.md, M2).
// The renderer records its GPU work as vk::CommandBuffer calls, which go through vulkan.hpp's
// dynamic dispatcher. Install() replaces the command-buffer functions in that dispatcher with ours;
// each forwards to MoltenVK unless the Metal path owns the command buffer (later steps).
// Step A: forwarding plus counts (BB_METAL_TRACE=1 prints the command stream every 5 s).

#pragma once
namespace MetalFrame {
struct Owned;
}

namespace MetalLayer {

/// The native Metal startup path, selected by run.sh without opening the Vulkan loader.
bool NativeBoot();
void* NativeInstanceProcAddr();

/// `dispatcher`: VULKAN_HPP_DEFAULT_DISPATCHER, right after its init(device). Installs only when
/// BB_METAL_TRACE=1 or a Metal path is on (BB_METAL_PRESENT=1); otherwise it stays untouched.
void Install(void* dispatcher);

/// The device and queue (handles as void*: VkDevice, VkQueue) whose Metal objects the layer uses.
void SetDevice(void* device, void* queue);

/// id<MTLDevice> / id<MTLCommandQueue> of MoltenVK's queue (null while the Metal path is off).
void* MtlDevice();
void* MtlQueue();

/// Step C (BB_METAL_FRAME=1): every command buffer is encoded in Metal.
bool FrameEnabled();

/// The image of an image view the Metal frame knows (as u64; 0: not known).
unsigned long long ImageOfView(unsigned long long view);

/// The Metal encoding state of an owned command buffer (a VkCommandBuffer), or null: for the
/// render adapter's Metal recorder (mtl_recorder.mm), which encodes without Vulkan calls.
MetalFrame::Owned* OwnedFor(void* command_buffer);

/// With the Metal frame: a pipeline only Metal builds (MoltenVK never sees it). Returns a
/// placeholder VkPipeline the Metal frame finds the Metal pipeline by: built here from the
/// create info (a VkGraphicsPipelineCreateInfo or VkComputePipelineCreateInfo; one is null),
/// except for game pipelines (built by the pipeline cache with their keys, GameBuildScope) and
/// geometry-shader ones (likewise).
void* MetalOnlyPipeline(const void* graphics_info, const void* compute_info);

/// The id<MTLCommandBuffer> an owned command buffer is encoded into, with every encoder ended,
/// for work encoded outside the layer in order (MetalFX); null when it is not owned.
void* MetalCommandBuffer(void* command_buffer);
/// Borrow the Metal texture registered for a Metal-frame image.
void* MetalImageTexture(void* image);

/// Step B (BB_METAL_PRESENT=1): the presenter's command buffer is encoded in Metal.
bool PresentEnabled();

/// From now until it is submitted, `command_buffer` (a VkCommandBuffer, already begun) is encoded
/// by the Metal path; its commands do not reach MoltenVK.
void Own(void* command_buffer);

/// With the Metal frame: the render adapter's device made `buffer` (a VkBuffer). The Metal frame
/// keeps its MTLBuffer and offset, so binding it asks MoltenVK nothing (docs/RENDER_ADAPTER.md,
/// M4). BufferGone: right before the VkBuffer is destroyed.
void BufferMade(void* buffer);
void BufferGone(void* buffer);
/// With the Metal frame (M4): a buffer only Metal has. Returns a placeholder VkBuffer for
/// `buffer` (id<MTLBuffer>, at offset 0; the caller keeps its reference), registered with the
/// Metal frame. MetalOnlyBufferAddress: its device address (the MTLBuffer's gpuAddress), and the
/// frame keeps it resident for shaders that reach it by address.
void* MetalOnlyBuffer(void* buffer);
unsigned long long MetalOnlyBufferAddress(void* buffer);
/// With the Metal frame: a timeline semaphore's MTLSharedEvent (id<MTLSharedEvent>, owned by the
/// layer until vkDestroySemaphore), or null. Every signal of the semaphore reaches it, from Metal
/// submissions and from MoltenVK's. EventValue: its signaled value; WaitEvent: blocks until it
/// reaches `value` or `timeout_ns` passes (false then) (docs/RENDER_ADAPTER.md, M4).
void* SemaphoreEvent(void* semaphore);
unsigned long long EventValue(void* event);
bool WaitEvent(void* event, unsigned long long value, unsigned long long timeout_ns = ~0ull);
/// With the Metal frame: the GPU memory in use and the working-set budget of MoltenVK's Metal
/// device (what MoltenVK's VK_EXT_memory_budget reports); false when the frame is off.
bool MemoryUsage(unsigned long long* used, unsigned long long* budget);
/// With the Metal frame (M4): an image only Metal has. Returns a placeholder VkImage for `texture`
/// (id<MTLTexture>; the caller keeps its reference), registered with the Metal frame; null when
/// the frame is off. Its views are made in Metal (vkCreateImageView with it returns a placeholder
/// view). ImageGone before the texture is released.
void* MetalOnlyImage(void* texture);
bool IsMetalOnlyImage(void* image);
/// A MoltenVK image the device made (a VkImage): the frame keeps its texture, exported once.
void ImageMade(void* image);
void ImageGone(void* image);

} // namespace MetalLayer
