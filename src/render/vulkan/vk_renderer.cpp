// Vulkan 1.3 backend: instance/device creation, swapchain lifecycle, frame synchronisation.
//
// Synchronisation model
//   * N frames in flight (config, 1..3). Each frame owns a command pool/buffer, an
//     "image available" semaphore and an "in flight" fence.
//   * Each swapchain image owns a "render finished" semaphore: presentation of image i may
//     still be reading it when frame slots wrap, so it cannot be per-frame.
//   * The fence is reset only after a successful acquire, so an early return never leaves
//     an unsignalled fence that the next frame would wait on forever.
//
// Swapchain recreation is triggered by VK_ERROR_OUT_OF_DATE_KHR, VK_SUBOPTIMAL_KHR,
// window resize notifications and present-mode changes. A zero-sized drawable (minimised)
// skips frames without touching the swapchain.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <stdexcept>

#include "core/log.h"
#include "render/renderer.h"

namespace lantern::render {
namespace {

constexpr uint32_t kVertSpv[] =
#include "placeholder.vert.spv.inc"
    ;
constexpr uint32_t kFragSpv[] =
#include "placeholder.frag.spv.inc"
    ;

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr const char* kSub = "vulkan";
constexpr uint32_t kRequiredApi = VK_API_VERSION_1_3;

std::atomic<uint64_t> g_validation_errors{0};

const char* result_name(VkResult r) {
  static constexpr std::pair<VkResult, const char*> kNames[] = {
      {VK_SUCCESS, "VK_SUCCESS"},
      {VK_NOT_READY, "VK_NOT_READY"},
      {VK_TIMEOUT, "VK_TIMEOUT"},
      {VK_INCOMPLETE, "VK_INCOMPLETE"},
      {VK_SUBOPTIMAL_KHR, "VK_SUBOPTIMAL_KHR"},
      {VK_ERROR_OUT_OF_HOST_MEMORY, "VK_ERROR_OUT_OF_HOST_MEMORY"},
      {VK_ERROR_OUT_OF_DEVICE_MEMORY, "VK_ERROR_OUT_OF_DEVICE_MEMORY"},
      {VK_ERROR_INITIALIZATION_FAILED, "VK_ERROR_INITIALIZATION_FAILED"},
      {VK_ERROR_DEVICE_LOST, "VK_ERROR_DEVICE_LOST"},
      {VK_ERROR_MEMORY_MAP_FAILED, "VK_ERROR_MEMORY_MAP_FAILED"},
      {VK_ERROR_LAYER_NOT_PRESENT, "VK_ERROR_LAYER_NOT_PRESENT"},
      {VK_ERROR_EXTENSION_NOT_PRESENT, "VK_ERROR_EXTENSION_NOT_PRESENT"},
      {VK_ERROR_FEATURE_NOT_PRESENT, "VK_ERROR_FEATURE_NOT_PRESENT"},
      {VK_ERROR_INCOMPATIBLE_DRIVER, "VK_ERROR_INCOMPATIBLE_DRIVER"},
      {VK_ERROR_TOO_MANY_OBJECTS, "VK_ERROR_TOO_MANY_OBJECTS"},
      {VK_ERROR_FORMAT_NOT_SUPPORTED, "VK_ERROR_FORMAT_NOT_SUPPORTED"},
      {VK_ERROR_SURFACE_LOST_KHR, "VK_ERROR_SURFACE_LOST_KHR"},
      {VK_ERROR_NATIVE_WINDOW_IN_USE_KHR, "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR"},
      {VK_ERROR_OUT_OF_DATE_KHR, "VK_ERROR_OUT_OF_DATE_KHR"},
  };
  for (const auto& [code, name] : kNames)
    if (code == r) return name;
  return "VkResult(unknown)";
}

struct VkError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(VkResult r, const char* what) {
  if (r != VK_SUCCESS) throw VkError(std::format("{} failed: {}", what, result_name(r)));
}
#define VK_CHECK(expr) check((expr), #expr)

std::string version_string(uint32_t v) {
  return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    ++g_validation_errors;
    log::error("vk-validation", "{}", data->pMessage);
  } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    log::warn("vk-validation", "{}", data->pMessage);
  }
  return VK_FALSE;
}

bool layer_available(const char* name) {
  uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  return std::any_of(layers.begin(), layers.end(), [&](const auto& l) { return std::strcmp(l.layerName, name) == 0; });
}

bool instance_extension_available(const char* name) {
  uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> exts(count);
  vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
  return std::any_of(exts.begin(), exts.end(), [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
}

PFN_vkCreateDebugUtilsMessengerEXT g_create_messenger = nullptr;
PFN_vkDestroyDebugUtilsMessengerEXT g_destroy_messenger = nullptr;

struct InstanceSetup {
  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  bool validation = false;
};

InstanceSetup create_instance(SurfaceSource* surface, bool want_validation, bool validation_forced) {
  uint32_t loader_version = VK_API_VERSION_1_0;
  vkEnumerateInstanceVersion(&loader_version);
  if (loader_version < kRequiredApi)
    throw VkError(
        std::format("Vulkan loader reports API {}; LANTERN requires Vulkan 1.3. Update the Vulkan loader "
                    "and GPU driver.",
                    version_string(loader_version)));

  std::vector<std::string> ext_storage = surface ? surface->vulkan_instance_extensions() : std::vector<std::string>{};
  InstanceSetup out;
  if (want_validation) {
    if (!layer_available(kValidationLayer)) {
      auto msg =
          "Vulkan validation requested but VK_LAYER_KHRONOS_validation is not installed "
          "(Fedora: sudo dnf install vulkan-validation-layers). Continuing without it.";
      validation_forced ? log::warn(kSub, "{}", msg) : log::info(kSub, "{}", msg);
    } else if (!instance_extension_available(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
      log::warn(kSub, "VK_EXT_debug_utils unavailable; validation messages will not be reported");
    } else {
      out.validation = true;
      ext_storage.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
  }
  for (const auto& e : ext_storage)
    if (!instance_extension_available(e.c_str()))
      throw VkError(std::format("required Vulkan instance extension {} is not available", e));

  std::vector<const char*> exts;
  for (const auto& e : ext_storage) exts.push_back(e.c_str());
  std::vector<const char*> layers;
  if (out.validation) layers.push_back(kValidationLayer);

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "LANTERN";
  app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
  app.pEngineName = "LANTERN";
  app.apiVersion = kRequiredApi;

  VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  dbg.pfnUserCallback = debug_callback;

  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
  ci.ppEnabledExtensionNames = exts.data();
  ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
  ci.ppEnabledLayerNames = layers.data();
  if (out.validation) ci.pNext = &dbg;  // also covers vkCreate/DestroyInstance
  VkResult r = vkCreateInstance(&ci, nullptr, &out.instance);
  if (r == VK_ERROR_INCOMPATIBLE_DRIVER)
    throw VkError("no Vulkan 1.3 capable driver found (VK_ERROR_INCOMPATIBLE_DRIVER)");
  check(r, "vkCreateInstance");
  if (out.validation) {
    g_create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(out.instance, "vkCreateDebugUtilsMessengerEXT"));
    g_destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(out.instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (g_create_messenger) VK_CHECK(g_create_messenger(out.instance, &dbg, nullptr, &out.messenger));
  }
  return out;
}

struct DeviceChoice {
  VkPhysicalDevice device = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
};

// Returns the reason a device is unusable, or nullopt if suitable (and fills queue_family).
std::optional<std::string> evaluate_device(VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t* queue_family) {
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(pd, &props);
  if (props.apiVersion < kRequiredApi)
    return std::format("supports Vulkan {} (needs 1.3)", version_string(props.apiVersion));

  uint32_t ext_count = 0;
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, nullptr);
  std::vector<VkExtensionProperties> exts(ext_count);
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, exts.data());
  if (std::none_of(exts.begin(), exts.end(),
                   [](const auto& e) { return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0; }))
    return "missing VK_KHR_swapchain";

  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f13;
  vkGetPhysicalDeviceFeatures2(pd, &f2);
  if (!f13.dynamicRendering) return "missing dynamicRendering";
  if (!f13.synchronization2) return "missing synchronization2";

  uint32_t qf_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qf_count);
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs.data());
  for (uint32_t i = 0; i < qf_count; ++i) {
    VkBool32 present = VK_FALSE;
    if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
    if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
      *queue_family = i;
      return std::nullopt;
    }
  }
  return "no queue family with graphics + present to this window";
}

int device_score(VkPhysicalDevice pd, const std::string& preferred) {
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(pd, &props);
  int score = 0;
  if (!preferred.empty()) {
    std::string name = props.deviceName, want = preferred;
    std::ranges::transform(name, name.begin(), ::tolower);
    std::ranges::transform(want, want.begin(), ::tolower);
    if (name.find(want) != std::string::npos) score += 1000;
  }
  if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 100;
  if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 50;
  return score;
}

std::vector<VkPhysicalDevice> enumerate_devices(VkInstance instance) {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devs(count);
  vkEnumeratePhysicalDevices(instance, &count, devs.data());
  return devs;
}

class VulkanRenderer final : public Renderer {
 public:
  explicit VulkanRenderer(const RendererCreateInfo& ci) : ci_(ci), present_mode_(ci.graphics.present_mode) {}
  ~VulkanRenderer() override { destroy(); }

  void init() {
    const auto& g = ci_.graphics;
    bool want = g.validation == Validation::On || (g.validation == Validation::Auto && ci_.debug_build);
    auto inst = create_instance(ci_.surface, want, g.validation == Validation::On);
    instance_ = inst.instance;
    messenger_ = inst.messenger;
    info_.validation = inst.validation;

    std::string err;
    uint64_t raw_surface = 0;
    if (!ci_.surface->create_vulkan_surface(instance_, &raw_surface, &err))
      throw VkError("cannot create window surface: " + err);
    surface_ = reinterpret_cast<VkSurfaceKHR>(raw_surface);

    pick_device();
    create_device();
    create_pipeline_cache();
    create_frames();
    if (!recreate_swapchain()) log::info(kSub, "window has zero size at startup; waiting for a resize");
  }

  void destroy() {
    if (device_) {
      vkDeviceWaitIdle(device_);
      save_pipeline_cache();
      destroy_pipeline();
      if (pipeline_cache_) vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
      for (auto& f : frames_) {
        vkDestroyCommandPool(device_, f.pool, nullptr);
        vkDestroyFence(device_, f.in_flight, nullptr);
        vkDestroySemaphore(device_, f.image_available, nullptr);
      }
      frames_.clear();
      destroy_swapchain_resources(swapchain_);
      swapchain_ = VK_NULL_HANDLE;
      vkDestroyDevice(device_, nullptr);
      device_ = VK_NULL_HANDLE;
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (messenger_ && g_destroy_messenger) g_destroy_messenger(instance_, messenger_, nullptr);
    messenger_ = VK_NULL_HANDLE;
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }

  const RendererInfo& info() const override { return info_; }
  uint64_t frames_presented() const override { return frames_presented_; }
  void notify_resize() override { needs_recreate_ = true; }
  void set_present_mode(PresentMode mode) override {
    if (mode == present_mode_) return;
    present_mode_ = mode;
    needs_recreate_ = true;
  }
  void wait_idle() override {
    if (device_) vkDeviceWaitIdle(device_);
  }

  FrameStatus begin_frame() override {
    if (needs_recreate_ || !swapchain_) {
      if (!recreate_swapchain()) return FrameStatus::Skipped;
    }
    Frame& f = frames_[frame_index_];
    VK_CHECK(vkWaitForFences(device_, 1, &f.in_flight, VK_TRUE, UINT64_MAX));

    VkResult r =
        vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.image_available, VK_NULL_HANDLE, &image_index_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
      needs_recreate_ = true;
      return FrameStatus::Skipped;
    }
    if (r == VK_SUBOPTIMAL_KHR)
      needs_recreate_ = true;  // still usable this frame
    else
      check(r, "vkAcquireNextImageKHR");

    VK_CHECK(vkResetFences(device_, 1, &f.in_flight));
    VK_CHECK(vkResetCommandPool(device_, f.pool, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));

    transition(f.cmd, images_[image_index_], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = views_[image_index_];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(f.cmd, &ri);
    in_frame_ = true;
    return FrameStatus::Ready;
  }

  void draw_placeholder(const PlaceholderParams& params) override {
    if (!in_frame_) return;
    Frame& f = frames_[frame_index_];
    VkRect2D rect = content_rect();
    VkViewport vp{static_cast<float>(rect.offset.x),
                  static_cast<float>(rect.offset.y),
                  static_cast<float>(rect.extent.width),
                  static_cast<float>(rect.extent.height),
                  0.0f,
                  1.0f};
    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdSetViewport(f.cmd, 0, 1, &vp);
    vkCmdSetScissor(f.cmd, 0, 1, &rect);
    struct {
      float ox, oy, w, h, time;
    } push{vp.x, vp.y, vp.width, vp.height, params.time_seconds};
    vkCmdPushConstants(f.cmd, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(f.cmd, 3, 1, 0, 0);
  }

  void end_frame() override {
    if (!in_frame_) return;
    in_frame_ = false;
    Frame& f = frames_[frame_index_];
    vkCmdEndRendering(f.cmd);
    transition(f.cmd, images_[image_index_], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_NONE, 0);
    VK_CHECK(vkEndCommandBuffer(f.cmd));

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.image_available;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = render_finished_[image_index_];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd.commandBuffer = f.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cmd;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    VK_CHECK(vkQueueSubmit2(queue_, 1, &si, f.in_flight));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_finished_[image_index_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image_index_;
    VkResult r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
      needs_recreate_ = true;
    else
      check(r, "vkQueuePresentKHR");
    ++frames_presented_;
    frame_index_ = (frame_index_ + 1) % static_cast<uint32_t>(frames_.size());
  }

 private:
  struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
  };

  void pick_device() {
    std::vector<std::string> rejected;
    int best_score = -1;
    for (VkPhysicalDevice pd : enumerate_devices(instance_)) {
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(pd, &props);
      uint32_t qf = 0;
      if (auto why = evaluate_device(pd, surface_, &qf)) {
        rejected.push_back(std::format("  - {}: {}", props.deviceName, *why));
        continue;
      }
      int s = device_score(pd, ci_.graphics.preferred_gpu);
      if (s > best_score) {
        best_score = s;
        physical_ = pd;
        queue_family_ = qf;
      }
    }
    if (!physical_) {
      std::string msg =
          "no suitable Vulkan GPU found. LANTERN needs Vulkan 1.3 with dynamicRendering, "
          "synchronization2 and VK_KHR_swapchain.";
      if (rejected.empty()) msg += "\n  (no Vulkan devices enumerated; is a driver installed?)";
      for (const auto& r : rejected) msg += "\n" + r;
      throw VkError(msg);
    }
    if (!ci_.graphics.preferred_gpu.empty() && best_score < 1000)
      log::warn(kSub, "preferred_gpu '{}' not found among suitable devices; using best available",
                ci_.graphics.preferred_gpu);

    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &driver;
    vkGetPhysicalDeviceProperties2(physical_, &p2);
    info_.backend = "vulkan";
    info_.device = p2.properties.deviceName;
    info_.driver = std::format("{} {}", driver.driverName, driver.driverInfo);
    info_.api_version = version_string(p2.properties.apiVersion);
  }

  void create_device() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;

    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;
    VK_CHECK(vkCreateDevice(physical_, &dci, nullptr, &device_));
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
  }

  std::filesystem::path cache_file() const {
    return ci_.cache_dir.empty() ? std::filesystem::path{} : ci_.cache_dir / "vk_pipeline_cache.bin";
  }

  void create_pipeline_cache() {
    std::vector<char> data;
    if (auto file = cache_file(); !file.empty()) {
      std::ifstream in(file, std::ios::binary);
      if (in) data.assign(std::istreambuf_iterator<char>(in), {});
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();  // drivers must ignore incompatible data
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
      ci.initialDataSize = 0;
      ci.pInitialData = nullptr;
      VK_CHECK(vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_));
    }
  }

  void save_pipeline_cache() {
    auto file = cache_file();
    if (file.empty() || !pipeline_cache_) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || size == 0) return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, data.data()) != VK_SUCCESS) return;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(size));
  }

  void create_frames() {
    frames_.resize(std::clamp<uint32_t>(ci_.graphics.frames_in_flight, 1, 3));
    for (auto& f : frames_) {
      VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      pci.queueFamilyIndex = queue_family_;
      VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &f.pool));
      VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      ai.commandPool = f.pool;
      ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ai.commandBufferCount = 1;
      VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &f.cmd));
      VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      VK_CHECK(vkCreateFence(device_, &fci, nullptr, &f.in_flight));
      VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &f.image_available));
    }
  }

  VkPresentModeKHR choose_present_mode() {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &count, modes.data());
    VkPresentModeKHR want = VK_PRESENT_MODE_FIFO_KHR;
    if (present_mode_ == PresentMode::Mailbox) want = VK_PRESENT_MODE_MAILBOX_KHR;
    if (present_mode_ == PresentMode::Immediate) want = VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (std::find(modes.begin(), modes.end(), want) != modes.end()) {
      info_.present_mode = std::string(to_string(present_mode_));
      return want;
    }
    log::warn(kSub, "present mode '{}' unsupported on this surface; using fifo", to_string(present_mode_));
    info_.present_mode = "fifo";
    return VK_PRESENT_MODE_FIFO_KHR;  // always supported
  }

  VkSurfaceFormatKHR choose_format() {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data());
    if (formats.empty()) throw VkError("surface reports no formats");
    // UNORM on purpose: GS output is already display-encoded, so no sRGB conversion on write.
    for (auto want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
      for (const auto& f : formats)
        if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
    return formats.front();
  }

  // Returns false if the drawable is zero-sized (minimised); the swapchain is left as-is.
  bool recreate_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps));
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
      Extent d = ci_.surface->drawable_extent();
      extent.width = std::clamp(d.width, caps.minImageExtent.width, caps.maxImageExtent.width);
      extent.height = std::clamp(d.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return false;

    vkDeviceWaitIdle(device_);
    VkSurfaceFormatKHR format = choose_format();
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0) image_count = std::min(image_count, caps.maxImageCount);

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
      for (auto a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                     VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
        if (caps.supportedCompositeAlpha & a) {
          alpha = a;
          break;
        }

    VkSwapchainKHR old = swapchain_;
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = image_count;
    sci.imageFormat = format.format;
    sci.imageColorSpace = format.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = alpha;
    sci.presentMode = choose_present_mode();
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = old;
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSwapchainKHR(device_, &sci, nullptr, &fresh));
    destroy_swapchain_resources(old);
    swapchain_ = fresh;
    extent_ = extent;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data());
    for (VkImage img : images_) {
      VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vci.image = img;
      vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vci.format = format.format;
      vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView view;
      VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &view));
      views_.push_back(view);
      VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VkSemaphore sem;
      VK_CHECK(vkCreateSemaphore(device_, &semci, nullptr, &sem));
      render_finished_.push_back(sem);
    }
    if (format.format != pipeline_format_) {
      destroy_pipeline();
      create_pipeline(format.format);
    }
    needs_recreate_ = false;
    log::debug(kSub, "swapchain {}x{}, {} images, present={}", extent.width, extent.height, count, info_.present_mode);
    return true;
  }

  // Destroys views/semaphores of the current image set and the given swapchain handle.
  void destroy_swapchain_resources(VkSwapchainKHR chain) {
    for (auto v : views_) vkDestroyImageView(device_, v, nullptr);
    for (auto s : render_finished_) vkDestroySemaphore(device_, s, nullptr);
    views_.clear();
    render_finished_.clear();
    images_.clear();
    if (chain) vkDestroySwapchainKHR(device_, chain, nullptr);
  }

  VkShaderModule shader(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &m));
    return m;
  }

  void create_pipeline(VkFormat color_format) {
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 5};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    VK_CHECK(vkCreatePipelineLayout(device_, &lci, nullptr, &pipeline_layout_));

    VkShaderModule vs = shader(kVertSpv, sizeof(kVertSpv));
    VkShaderModule fs = shader(kFragSpv, sizeof(kFragSpv));
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &color_format;

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &rci;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vin;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vps;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &ds;
    pci.layout = pipeline_layout_;
    VkResult r = vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &pci, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    check(r, "vkCreateGraphicsPipelines");
    pipeline_format_ = color_format;
  }

  void destroy_pipeline() {
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    pipeline_format_ = VK_FORMAT_UNDEFINED;
  }

  VkRect2D content_rect() const {
    if (ci_.graphics.aspect == AspectMode::Stretch) return {{0, 0}, extent_};
    // Largest centred 4:3 rectangle (PS2 SH2 renders a 4:3 image).
    uint32_t w = extent_.width, h = extent_.height;
    if (w * 3 > h * 4)
      w = h * 4 / 3;
    else
      h = w * 3 / 4;
    return {{static_cast<int32_t>((extent_.width - w) / 2), static_cast<int32_t>((extent_.height - h) / 2)}, {w, h}};
  }

  static void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                         VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                         VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  RendererCreateInfo ci_;
  RendererInfo info_;
  PresentMode present_mode_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkExtent2D extent_{};
  std::vector<VkImage> images_;
  std::vector<VkImageView> views_;
  std::vector<VkSemaphore> render_finished_;
  std::vector<Frame> frames_;
  VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkFormat pipeline_format_ = VK_FORMAT_UNDEFINED;
  uint32_t frame_index_ = 0;
  uint32_t image_index_ = 0;
  uint64_t frames_presented_ = 0;
  bool needs_recreate_ = false;
  bool in_frame_ = false;
};

}  // namespace

uint64_t vulkan_validation_error_count() { return g_validation_errors.load(); }

std::unique_ptr<Renderer> create_renderer(const RendererCreateInfo& info, std::string* error) {
  if (info.graphics.backend == RenderBackend::Null) return create_null_renderer();
  if (!info.surface) {
    if (error) *error = "Vulkan backend requires a window surface";
    return nullptr;
  }
  auto r = std::make_unique<VulkanRenderer>(info);
  try {
    r->init();
  } catch (const std::exception& e) {
    r->destroy();
    if (error) *error = e.what();
    return nullptr;
  }
  return r;
}

std::vector<std::string> describe_vulkan_devices(SurfaceSource* surface, std::string* error) {
  std::vector<std::string> out;
  InstanceSetup inst;
  VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
  try {
    inst = create_instance(surface, false, false);
    uint64_t raw = 0;
    std::string err;
    if (surface && surface->create_vulkan_surface(inst.instance, &raw, &err))
      vk_surface = reinterpret_cast<VkSurfaceKHR>(raw);
    for (VkPhysicalDevice pd : enumerate_devices(inst.instance)) {
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(pd, &props);
      uint32_t qf = 0;
      auto why = evaluate_device(pd, vk_surface, &qf);
      out.push_back(std::format("{} (Vulkan {}): {}", props.deviceName, version_string(props.apiVersion),
                                why ? "unsuitable - " + *why : std::string("suitable")));
    }
  } catch (const std::exception& e) {
    if (error) *error = e.what();
  }
  if (vk_surface) vkDestroySurfaceKHR(inst.instance, vk_surface, nullptr);
  if (inst.instance) vkDestroyInstance(inst.instance, nullptr);
  return out;
}

}  // namespace lantern::render
