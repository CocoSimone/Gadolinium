#include <File.hpp>
#include <ParallelRDPWrapper.hpp>
#include <memory>
#include <rdp_device.hpp>
#include <core/mmio/VI.hpp>

using namespace Vulkan;
using namespace RDP;

bool ParallelRDP::IsFramerateUnlocked() const { return wsi->get_present_mode() != PresentMode::SyncToVBlank; }

void ParallelRDP::SetFramerateUnlocked(bool unlocked) const {
  if (unlocked) {
    wsi->set_present_mode(PresentMode::UnlockedForceTearing);
  } else {
    wsi->set_present_mode(PresentMode::SyncToVBlank);
  }
}

Program *fullscreen_quad_program;

// Copied and modified from WSI::init_context_from_platform
Util::IntrusivePtr<Context> InitVulkanContext(WSIPlatform *platform, unsigned num_thread_indices,
                                              const Context::SystemHandles &system_handles,
                                              InstanceFactory *instance_factory) {
  VK_ASSERT(platform);
  auto instance_ext = platform->get_instance_extensions();
  auto device_ext = platform->get_device_extensions();
  auto new_context = Util::make_handle<Context>();

  new_context->set_application_info(platform->get_application_info());
  new_context->set_num_thread_indices(num_thread_indices);
  new_context->set_system_handles(system_handles);
  if (instance_factory) {
    new_context->set_instance_factory(instance_factory);
  }

  if (!new_context->init_instance(instance_ext.data(), instance_ext.size(), CONTEXT_CREATION_ENABLE_ADVANCED_WSI_BIT)) {
    Util::panic("Failed to create Vulkan instance.\n");
  }

  VkSurfaceKHR tmp_surface = platform->create_surface(new_context->get_instance(), VK_NULL_HANDLE);

  bool ret = new_context->init_device(VK_NULL_HANDLE, tmp_surface, device_ext.data(), device_ext.size(),
                                      CONTEXT_CREATION_ENABLE_ADVANCED_WSI_BIT);

  if (tmp_surface) {
    platform->destroy_surface(new_context->get_instance(), tmp_surface);
  }

  if (!ret) {
    Util::panic("Failed to create Vulkan device.\n");
  }

  return new_context;
}

void ParallelRDP::LoadWSIPlatform(const std::shared_ptr<InstanceFactory> &instanceFactory,
                                  const std::shared_ptr<WSIPlatform> &wsi_platform,
                                  const std::shared_ptr<WindowInfo> &newWindowInfo) {
  wsi = std::make_shared<WSI>();
  wsi->set_backbuffer_srgb(false);
  wsi->set_platform(wsi_platform.get());
  wsi->set_present_mode(PresentMode::SyncToVBlank);
  Context::SystemHandles handles;

  if (!wsi->init_from_existing_context(InitVulkanContext(wsi_platform.get(), 1, handles, instanceFactory.get()))) {
    Util::panic("Failed to initialize WSI: init_from_existing_context() failed");
  }

  if (!wsi->init_device()) {
    Util::panic("Failed to initialize WSI: init_device() failed");
  }

  if (!wsi->init_surface_swapchain()) {
    Util::panic("Failed to initialize WSI: init_surface_swapchain() failed");
  }

  windowInfo = newWindowInfo;
}

void ParallelRDP::Init(const std::shared_ptr<InstanceFactory> &factory, const std::shared_ptr<WSIPlatform> &wsiPlatform,
                       const std::shared_ptr<WindowInfo> &newWindowInfo, const u8 *rdram) {
  LoadWSIPlatform(factory, wsiPlatform, newWindowInfo);

  ResourceLayout vertLayout;
  ResourceLayout fragLayout;

  vertLayout.input_mask = 1;
  vertLayout.output_mask = 1;

  fragLayout.input_mask = 1;
  fragLayout.output_mask = 1;
  fragLayout.spec_constant_mask = 1;
  fragLayout.push_constant_size = 4 * sizeof(float);

  fragLayout.sets[0].sampled_image_mask = 1;
  fragLayout.sets[0].fp_mask = 1;
  fragLayout.sets[0].array_size[0] = 1;

  auto fullscreenQuadVert = Util::ReadFileBinary("resources/vert.spv");
  auto fullscreenQuadFrag = Util::ReadFileBinary("resources/frag.spv");
  auto sizeVert = fullscreenQuadVert.size();
  auto sizeFrag = fullscreenQuadFrag.size();

  fullscreen_quad_program = wsi->get_device().request_program(
    reinterpret_cast<u32 *>(fullscreenQuadVert.data()), sizeVert, reinterpret_cast<u32 *>(fullscreenQuadFrag.data()),
    sizeFrag, &vertLayout, &fragLayout);

  auto aligned_rdram = reinterpret_cast<uintptr_t>(rdram);
  uintptr_t offset = 0;

  if (wsi->get_device().get_device_features().supports_external_memory_host) {
    size_t align = wsi->get_device().get_device_features().host_memory_properties.minImportedHostPointerAlignment;
    offset = aligned_rdram & (align - 1);
    aligned_rdram -= offset;
  }

  CommandProcessorFlags flags = 0;

  command_processor = std::make_shared<CommandProcessor>(wsi->get_device(), reinterpret_cast<void *>(aligned_rdram),
                                                         offset, 8 * 1024 * 1024, 4 * 1024 * 1024, flags);

  if (!command_processor->device_is_supported()) {
    Util::panic("This device probably does not support 8/16-bit storage. Make sure you're using up-to-date drivers!");
  }
}

void ParallelRDP::DrawFullscreenTexturedQuad(Util::IntrusivePtr<Image> image,
                                             Util::IntrusivePtr<CommandBuffer> cmd) const {
  cmd->set_texture(0, 0, image->get_view(), StockSampler::LinearClamp);
  cmd->set_program(fullscreen_quad_program);
  cmd->set_quad_state();
  auto data = static_cast<float *>(cmd->allocate_vertex_data(0, 6 * sizeof(float), 2 * sizeof(float)));
  data[0] = -1.0f;
  data[1] = -3.0f;
  data[2] = -1.0f;
  data[3] = +1.0f;
  data[4] = +3.0f;
  data[5] = +1.0f;

  auto windowSize = windowInfo->get_window_size();

  float zoom = std::min(windowSize.x / wsi->get_platform().get_surface_width(),
                        windowSize.y / wsi->get_platform().get_surface_height());

  float width = (wsi->get_platform().get_surface_width() / windowSize.x) * zoom;
  float height = (wsi->get_platform().get_surface_height() / windowSize.y) * zoom;

  float uniform_data[] = {// Size
                          width, height,
                          // Offset
                          (1.0f - width) * 0.5f, (1.0f - height) * 0.5f};

  cmd->push_constants(uniform_data, 0, sizeof(uniform_data));

  cmd->set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, 0);
  cmd->set_depth_test(false, false);
  cmd->set_depth_compare(VK_COMPARE_OP_ALWAYS);
  cmd->set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
  cmd->draw(3, 1);
}

void ParallelRDP::UpdateScreen(Util::IntrusivePtr<Image> image) const {
  wsi->begin_frame();

  if (!image) {
    auto info = ImageCreateInfo::immutable_2d_image(800, 600, VK_FORMAT_R8G8B8A8_UNORM);
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.misc = IMAGE_MISC_MUTABLE_SRGB_BIT;
    info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    image = wsi->get_device().create_image(info);

    auto cmd = wsi->get_device().request_command_buffer();

    cmd->image_barrier(*image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT);
    cmd->clear_image(*image, {});
    cmd->image_barrier(*image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    wsi->get_device().submit(cmd);
  }

  Util::IntrusivePtr<CommandBuffer> cmd = wsi->get_device().request_command_buffer();

  cmd->begin_render_pass(wsi->get_device().get_swapchain_render_pass(SwapchainRenderPass::ColorOnly));
  DrawFullscreenTexturedQuad(image, cmd);

  cmd->end_render_pass();
  wsi->get_device().submit(cmd);
  wsi->end_frame();
}

void ParallelRDP::UpdateScreen(const n64::VI &vi) const {
  command_processor->set_vi_register(VIRegister::Control, vi.status.raw);
  command_processor->set_vi_register(VIRegister::Origin, vi.origin);
  command_processor->set_vi_register(VIRegister::Width, vi.width);
  command_processor->set_vi_register(VIRegister::Intr, vi.intr);
  command_processor->set_vi_register(VIRegister::VCurrentLine, vi.current);
  command_processor->set_vi_register(VIRegister::Timing, vi.burst.raw);
  command_processor->set_vi_register(VIRegister::VSync, vi.vsync);
  command_processor->set_vi_register(VIRegister::HSync, vi.hsync);
  command_processor->set_vi_register(VIRegister::Leap, vi.hsyncLeap.raw);
  command_processor->set_vi_register(VIRegister::HStart, vi.hstart.raw);
  command_processor->set_vi_register(VIRegister::VStart, vi.vstart.raw);
  command_processor->set_vi_register(VIRegister::VBurst, vi.vburst);
  command_processor->set_vi_register(VIRegister::XScale, vi.xscale.raw);
  command_processor->set_vi_register(VIRegister::YScale, vi.yscale.raw);
  ScanoutOptions opts;
  opts.persist_frame_on_invalid_input = true;
  opts.vi.aa = true;
  opts.vi.scale = true;
  opts.vi.dither_filter = true;
  opts.vi.divot_filter = true;
  opts.vi.gamma_dither = true;
  opts.downscale_steps = true;
  opts.crop_overscan_pixels = true;
  Util::IntrusivePtr<Image> image = command_processor->scanout(opts);
  UpdateScreen(image);
  command_processor->begin_frame_context();
}

void ParallelRDP::EnqueueCommand(int command_length, const u32 *buffer) const {
  command_processor->enqueue_command(command_length, buffer);
}

void ParallelRDP::OnFullSync() const { command_processor->wait_for_timeline(command_processor->signal_timeline()); }
