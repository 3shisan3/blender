/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "GPU_debug.hh"

#include "vk_backend.hh"
#include "vk_context.hh"
#include "vk_debug.hh"
#include "vk_framebuffer.hh"
#include "vk_immediate.hh"
#include "vk_shader.hh"
#include "vk_shader_interface.hh"
#include "vk_state_manager.hh"
#include "vk_texture.hh"

#include "GHOST_C-api.h"

namespace blender::gpu {

VKContext::VKContext(void *ghost_window, void *ghost_context)
{
  ghost_window_ = ghost_window;
  ghost_context_ = ghost_context;

  state_manager = new VKStateManager();

  back_left = new VKFrameBuffer("back_left");
  front_left = new VKFrameBuffer("front_left");
  active_fb = back_left;

  compiler = &VKBackend::get().shader_compiler;
}

VKContext::~VKContext()
{
  if (surface_texture_) {
    back_left->attachment_remove(GPU_FB_COLOR_ATTACHMENT0);
    front_left->attachment_remove(GPU_FB_COLOR_ATTACHMENT0);
    GPU_texture_free(surface_texture_);
    surface_texture_ = nullptr;
  }
  free_resources();
  VKBackend::get().device.context_unregister(*this);

  imm = nullptr;
  compiler = nullptr;
}

void VKContext::sync_backbuffer(bool cycle_resource_pool)
{
  if (ghost_window_) {
    GHOST_VulkanSwapChainData swap_chain_data = {};
    GHOST_GetVulkanSwapChainFormat((GHOST_WindowHandle)ghost_window_, &swap_chain_data);
    VKThreadData &thread_data = thread_data_.value().get();
    if (cycle_resource_pool) {
      thread_data.resource_pool_next();
      VKResourcePool &resource_pool = thread_data.resource_pool_get();
      imm = &resource_pool.immediate;
    }

    const bool reset_framebuffer = swap_chain_format_.format !=
                                       swap_chain_data.surface_format.format ||
                                   swap_chain_format_.colorSpace !=
                                       swap_chain_data.surface_format.colorSpace ||
                                   vk_extent_.width != swap_chain_data.extent.width ||
                                   vk_extent_.height != swap_chain_data.extent.height;
    if (reset_framebuffer) {
      if (has_active_framebuffer()) {
        deactivate_framebuffer();
      }
      if (surface_texture_) {
        GPU_texture_free(surface_texture_);
        surface_texture_ = nullptr;
      }
      surface_texture_ = GPU_texture_create_2d(
          "back-left",
          swap_chain_data.extent.width,
          swap_chain_data.extent.height,
          1,
          to_gpu_format(swap_chain_data.surface_format.format),
          GPU_TEXTURE_USAGE_ATTACHMENT,
          nullptr);

      back_left->attachment_set(GPU_FB_COLOR_ATTACHMENT0,
                                GPU_ATTACHMENT_TEXTURE(surface_texture_));
      front_left->attachment_set(GPU_FB_COLOR_ATTACHMENT0,
                                 GPU_ATTACHMENT_TEXTURE(surface_texture_));

      back_left->bind(false);

      swap_chain_format_ = swap_chain_data.surface_format;
      vk_extent_ = swap_chain_data.extent;
    }
  }
}

void VKContext::activate()
{
  /* Make sure no other context is already bound to this thread. */
  BLI_assert(is_active_ == false);

  VKDevice &device = VKBackend::get().device;
  VKThreadData &thread_data = device.current_thread_data();
  thread_data_ = std::reference_wrapper<VKThreadData>(thread_data);

  if (!render_graph_.has_value()) {
    render_graph_ = std::reference_wrapper<render_graph::VKRenderGraph>(
        *device.render_graph_new());
    for (const StringRef &group : debug_stack) {
      debug_group_begin(std::string(group).c_str(), 0);
    }
  }

  imm = &thread_data.resource_pool_get().immediate;

  is_active_ = true;

  sync_backbuffer(false);

  immActivate();
}

void VKContext::deactivate()
{
  flush_render_graph(RenderGraphFlushFlags(0));
  immDeactivate();
  imm = nullptr;
  thread_data_.reset();

  is_active_ = false;
}

void VKContext::begin_frame() {}

void VKContext::end_frame()
{
  VKDevice &device = VKBackend::get().device;
  device.orphaned_data.destroy_discarded_resources(device);
}

void VKContext::flush()
{
  flush_render_graph(RenderGraphFlushFlags::RENEW_RENDER_GRAPH);
}

TimelineValue VKContext::flush_render_graph(RenderGraphFlushFlags flags,
                                            VkPipelineStageFlags wait_dst_stage_mask,
                                            VkSemaphore wait_semaphore,
                                            VkSemaphore signal_semaphore,
                                            VkFence signal_fence)
{
  if (has_active_framebuffer()) {
    VKFrameBuffer &framebuffer = *active_framebuffer_get();
    if (framebuffer.is_rendering()) {
      framebuffer.rendering_end(*this);
    }
  }
  descriptor_set_get().upload_descriptor_sets();
  VKDevice &device = VKBackend::get().device;
  TimelineValue timeline = device.render_graph_submit(
      &render_graph_.value().get(),
      discard_pool,
      bool(flags & RenderGraphFlushFlags::SUBMIT),
      bool(flags & RenderGraphFlushFlags::WAIT_FOR_COMPLETION),
      wait_dst_stage_mask,
      wait_semaphore,
      signal_semaphore,
      signal_fence);
  render_graph_.reset();
  if (bool(flags & RenderGraphFlushFlags::RENEW_RENDER_GRAPH)) {
    render_graph_ = std::reference_wrapper<render_graph::VKRenderGraph>(
        *device.render_graph_new());
    for (const StringRef &group : debug_stack) {
      debug_group_begin(std::string(group).c_str(), 0);
    }
  }
  return timeline;
}

void VKContext::finish() {}

void VKContext::memory_statistics_get(int *r_total_mem_kb, int *r_free_mem_kb)
{
  const VKDevice &device = VKBackend::get().device;
  device.memory_statistics_get(r_total_mem_kb, r_free_mem_kb);
}

/* -------------------------------------------------------------------- */
/** \name State manager
 * \{ */

VKDescriptorPools &VKContext::descriptor_pools_get()
{
  return thread_data_.value().get().resource_pool_get().descriptor_pools;
}

VKDescriptorSetTracker &VKContext::descriptor_set_get()
{
  return thread_data_.value().get().resource_pool_get().descriptor_set;
}

VKStateManager &VKContext::state_manager_get() const
{
  return *static_cast<VKStateManager *>(state_manager);
}

void VKContext::debug_unbind_all_ubo()
{
  state_manager_get().uniform_buffer_unbind_all();
};

void VKContext::debug_unbind_all_ssbo()
{
  state_manager_get().storage_buffer_unbind_all();
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frame-buffer
 * \{ */

void VKContext::activate_framebuffer(VKFrameBuffer &framebuffer)
{
  if (has_active_framebuffer()) {
    deactivate_framebuffer();
  }

  BLI_assert(active_fb == nullptr);
  active_fb = &framebuffer;
  framebuffer.update_size();
  framebuffer.update_srgb();
  framebuffer.rendering_reset();
}

VKFrameBuffer *VKContext::active_framebuffer_get() const
{
  return unwrap(active_fb);
}

bool VKContext::has_active_framebuffer() const
{
  return active_framebuffer_get() != nullptr;
}

void VKContext::deactivate_framebuffer()
{
  VKFrameBuffer *framebuffer = active_framebuffer_get();
  BLI_assert(framebuffer != nullptr);
  if (framebuffer->is_rendering()) {
    framebuffer->rendering_end(*this);
  }
  active_fb = nullptr;
}

void VKContext::rendering_end()
{
  VKFrameBuffer *framebuffer = active_framebuffer_get();
  if (framebuffer) {
    framebuffer->rendering_end(*this);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pipeline
 * \{ */

void VKContext::update_pipeline_data(GPUPrimType primitive,
                                     VKVertexAttributeObject &vao,
                                     render_graph::VKPipelineData &r_pipeline_data)
{
  VKShader &vk_shader = unwrap(*shader);
  VKFrameBuffer &framebuffer = *active_framebuffer_get();
  update_pipeline_data(
      vk_shader,
      vk_shader.ensure_and_get_graphics_pipeline(primitive, vao, state_manager_get(), framebuffer),
      r_pipeline_data);
}

void VKContext::update_pipeline_data(render_graph::VKPipelineData &r_pipeline_data)
{
  VKShader &vk_shader = unwrap(*shader);
  update_pipeline_data(vk_shader, vk_shader.ensure_and_get_compute_pipeline(), r_pipeline_data);
}

void VKContext::update_pipeline_data(VKShader &vk_shader,
                                     VkPipeline vk_pipeline,
                                     render_graph::VKPipelineData &r_pipeline_data)
{
  r_pipeline_data.vk_pipeline_layout = vk_shader.vk_pipeline_layout;
  r_pipeline_data.vk_pipeline = vk_pipeline;

  /* Update push constants. */
  r_pipeline_data.push_constants_data = nullptr;
  r_pipeline_data.push_constants_size = 0;
  const VKPushConstants::Layout &push_constants_layout =
      vk_shader.interface_get().push_constants_layout_get();
  if (push_constants_layout.storage_type_get() == VKPushConstants::StorageType::PUSH_CONSTANTS) {
    r_pipeline_data.push_constants_size = push_constants_layout.size_in_bytes();
    r_pipeline_data.push_constants_data = vk_shader.push_constants.data();
  }

  /* Update descriptor set. */
  r_pipeline_data.vk_descriptor_set = VK_NULL_HANDLE;
  if (vk_shader.has_descriptor_set()) {
    VKDescriptorSetTracker &descriptor_set = descriptor_set_get();
    descriptor_set.update_descriptor_set(*this, access_info_);
    r_pipeline_data.vk_descriptor_set = descriptor_set.vk_descriptor_set;
  }
}

render_graph::VKResourceAccessInfo &VKContext::reset_and_get_access_info()
{
  access_info_.reset();
  return access_info_;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Graphics pipeline
 * \{ */

void VKContext::swap_buffers_pre_callback(const GHOST_VulkanSwapChainData *swap_chain_data)
{
  VKContext *context = VKContext::get();
  BLI_assert(context);
  context->swap_buffers_pre_handler(*swap_chain_data);
}

void VKContext::swap_buffers_post_callback()
{
  VKContext *context = VKContext::get();
  BLI_assert(context);
  context->swap_buffers_post_handler();
}

void VKContext::swap_buffers_pre_handler(const GHOST_VulkanSwapChainData &swap_chain_data)
{
  GPU_debug_group_begin("BackBuffer.Blit");

  VKFrameBuffer &framebuffer = *unwrap(active_fb);
  VKTexture *color_attachment = unwrap(unwrap(framebuffer.color_tex(0)));

  render_graph::VKBlitImageNode::CreateInfo blit_image = {};
  blit_image.src_image = color_attachment->vk_image_handle();
  blit_image.dst_image = swap_chain_data.image;
  blit_image.filter = VK_FILTER_NEAREST;

  VkImageBlit &region = blit_image.region;
  region.srcOffsets[0] = {0, color_attachment->height_get(), 0};
  region.srcOffsets[1] = {color_attachment->width_get(), 0, 1};
  region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.srcSubresource.mipLevel = 0;
  region.srcSubresource.baseArrayLayer = 0;
  region.srcSubresource.layerCount = 1;

  region.dstOffsets[0] = {0, 0, 0};
  region.dstOffsets[1] = {
      int32_t(swap_chain_data.extent.width), int32_t(swap_chain_data.extent.height), 1};
  region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.dstSubresource.mipLevel = 0;
  region.dstSubresource.baseArrayLayer = 0;
  region.dstSubresource.layerCount = 1;

  /* Swap chain commands are CPU synchronized at this moment, allowing to temporary add the swap
   * chain image as device resources. When we move towards GPU swap chain synchronization we need
   * to keep track of the swap chain image between frames. */
  VKDevice &device = VKBackend::get().device;
  device.resources.add_image(swap_chain_data.image, 1, "SwapchainImage");

  framebuffer.rendering_end(*this);
  flush_render_graph(RenderGraphFlushFlags::RENEW_RENDER_GRAPH);

  render_graph::VKRenderGraph &render_graph = this->render_graph();
  render_graph.add_node(blit_image);
  GPU_debug_group_end();
  descriptor_set_get().upload_descriptor_sets();
  render_graph::VKSynchronizationNode::CreateInfo synchronization = {};
  synchronization.vk_image = swap_chain_data.image;
  synchronization.vk_image_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  synchronization.vk_image_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  render_graph.add_node(synchronization);
  flush_render_graph(RenderGraphFlushFlags::SUBMIT | RenderGraphFlushFlags::RENEW_RENDER_GRAPH,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                     swap_chain_data.acquire_semaphore,
                     swap_chain_data.present_semaphore,
                     swap_chain_data.submission_fence);

  device.resources.remove_image(swap_chain_data.image);
#if 0
  device.debug_print();
#endif
}

void VKContext::swap_buffers_post_handler()
{
  sync_backbuffer(true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name OpenXR
 * \{ */

void VKContext::openxr_acquire_framebuffer_image_callback(GHOST_VulkanOpenXRData *openxr_data)
{
  VKContext *context = VKContext::get();
  BLI_assert(context);
  context->openxr_acquire_framebuffer_image_handler(*openxr_data);
}

void VKContext::openxr_release_framebuffer_image_callback(GHOST_VulkanOpenXRData *openxr_data)
{
  VKContext *context = VKContext::get();
  BLI_assert(context);
  context->openxr_release_framebuffer_image_handler(*openxr_data);
}

void VKContext::openxr_acquire_framebuffer_image_handler(GHOST_VulkanOpenXRData &openxr_data)
{
  VKFrameBuffer &framebuffer = *unwrap(active_fb);
  VKTexture *color_attachment = unwrap(unwrap(framebuffer.color_tex(0)));
  openxr_data.image_data = color_attachment->read(0, GPU_DATA_HALF_FLOAT);
  openxr_data.extent.width = color_attachment->width_get();
  openxr_data.extent.height = color_attachment->height_get();
}

void VKContext::openxr_release_framebuffer_image_handler(GHOST_VulkanOpenXRData &openxr_data)
{
  MEM_freeN(openxr_data.image_data);
  openxr_data.image_data = nullptr;
}

/** \} */

}  // namespace blender::gpu
