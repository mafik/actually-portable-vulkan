#define _COSMO_SOURCE

#include "any_subclass.hpp"
#include "vk_dispatch.hpp"
#include "window_session.hpp"
#include "window_session_wayland.hpp"
#include "window_session_win32.hpp"
#include "window_session_xcb.hpp"

#include <libc/dce.h> // IsWindows
#include <libc/dlopen/dlfcn.h>
#include <libc/nt/signals.h> // AddVectoredExceptionHandler
#include <libc/nt/struct/ntexceptionpointers.h>
#include <libc/nt/struct/ntexceptionrecord.h>
#include <libc/runtime/runtime.h> // ShowCrashReports

// Forward-declare cosmo's internals.
struct CosmoTib;
extern "C" {
void __set_tls(struct CosmoTib *);
struct CosmoTib *__get_tls_privileged(void);
}
#include <cstdlib>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr unsigned kInitialWidth = 800;
constexpr unsigned kInitialHeight = 600;
constexpr const char *kWindowTitle = "Actually Portable Vulkan";

// Inline storage for the chosen WindowSession subclass. 160 bytes covers
// Xcb + Wayland comfortably; grow if a Win32/Cocoa backend needs more.
using SessionStorage = AnySubclass<WindowSession, 160>;

struct App {
  SessionStorage session;
  VkApi vk{};

  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  uint32_t graphics_family = 0;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat swap_format = VK_FORMAT_UNDEFINED;
  VkExtent2D swap_extent = {0, 0};
  std::vector<VkImage> swap_images;

  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkSemaphore sem_acquire = VK_NULL_HANDLE;
  VkSemaphore sem_render = VK_NULL_HANDLE;
  VkFence fence_in_flight = VK_NULL_HANDLE;
};

#define VK_CHECK(expr)                                                         \
  do {                                                                         \
    VkResult _r = (expr);                                                      \
    if (_r != VK_SUCCESS) {                                                    \
      std::fprintf(stderr, "vk: %s failed: %d\n", #expr, int(_r));             \
      return false;                                                            \
    }                                                                          \
  } while (0)

bool select_and_open_session(App &app) {
  // Preference order:
  //   1. APV_BACKEND env override ("win32" | "wayland" | "xcb")
  //   2. Windows host  → Win32
  //   3. $WAYLAND_DISPLAY set → Wayland
  //   4. otherwise     → XCB
  const char *forced = std::getenv("APV_BACKEND");
  const char *wl_disp = std::getenv("WAYLAND_DISPLAY");

  auto try_win32 = [&] {
    app.session.emplace<Win32WindowSession>();
    if (app.session->create_window(kInitialWidth, kInitialHeight, kWindowTitle))
      return true;
    std::fprintf(stderr, "win32 backend failed\n");
    app.session.reset();
    return false;
  };
  auto try_wayland = [&] {
    app.session.emplace<WaylandWindowSession>();
    if (app.session->create_window(kInitialWidth, kInitialHeight, kWindowTitle))
      return true;
    std::fprintf(stderr, "wayland backend failed\n");
    app.session.reset();
    return false;
  };
  auto try_xcb = [&] {
    app.session.emplace<XcbWindowSession>();
    return app.session->create_window(kInitialWidth, kInitialHeight,
                                      kWindowTitle);
  };

  if (forced) {
    if (std::strcmp(forced, "win32") == 0)
      return try_win32();
    if (std::strcmp(forced, "wayland") == 0)
      return try_wayland();
    if (std::strcmp(forced, "xcb") == 0)
      return try_xcb();
    std::fprintf(stderr, "APV_BACKEND=%s not recognised\n", forced);
  }

  if (IsWindows())
    return try_win32();
  if (wl_disp && *wl_disp) {
    if (try_wayland())
      return true;
    std::fprintf(stderr, "falling back to XCB\n");
  }
  return try_xcb();
}

bool create_instance(App &app) {
  if (!vk_load_global(app.vk))
    return false;

  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "Actually Portable Vulkan";
  app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  app_info.pEngineName = "none";
  app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
  app_info.apiVersion = VK_API_VERSION_1_0;

  const char *exts[] = {
      VK_KHR_SURFACE_EXTENSION_NAME,
      app.session->vk_surface_extension_name(),
  };

  VkInstanceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &app_info;
  ci.enabledExtensionCount = 2;
  ci.ppEnabledExtensionNames = exts;

  VK_CHECK(app.vk.vkCreateInstance(&ci, nullptr, &app.instance));
  if (!vk_load_instance(app.vk, app.instance))
    return false;
  return true;
}

bool create_surface(App &app) {
  return app.session->create_vk_surface(app.vk, app.instance, &app.surface);
}

bool pick_physical_device(App &app) {
  uint32_t n = 0;
  VK_CHECK(app.vk.vkEnumeratePhysicalDevices(app.instance, &n, nullptr));
  if (n == 0) {
    std::fprintf(stderr, "vk: no physical devices\n");
    return false;
  }
  std::vector<VkPhysicalDevice> devs(n);
  VK_CHECK(app.vk.vkEnumeratePhysicalDevices(app.instance, &n, devs.data()));

  for (VkPhysicalDevice pd : devs) {
    uint32_t qn = 0;
    app.vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    app.vk.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
    for (uint32_t i = 0; i < qn; ++i) {
      if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        continue;
      VkBool32 present = VK_FALSE;
      VK_CHECK(app.vk.vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, app.surface,
                                                           &present));
      if (!present)
        continue;
      app.phys = pd;
      app.graphics_family = i;
      VkPhysicalDeviceProperties p{};
      app.vk.vkGetPhysicalDeviceProperties(pd, &p);
      std::printf("vk: using %s (queue family %u)\n", p.deviceName, i);
      return true;
    }
  }
  std::fprintf(stderr, "vk: no suitable device+queue\n");
  return false;
}

bool create_device(App &app) {
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = app.graphics_family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;

  const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = dev_exts;

  VK_CHECK(app.vk.vkCreateDevice(app.phys, &dci, nullptr, &app.device));
  if (!vk_load_device(app.vk, app.device))
    return false;
  app.vk.vkGetDeviceQueue(app.device, app.graphics_family, 0, &app.queue);
  return true;
}

bool create_swapchain(App &app) {
  VkSurfaceCapabilitiesKHR caps{};
  VK_CHECK(app.vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
      app.phys, app.surface, &caps));

  uint32_t fn = 0;
  VK_CHECK(app.vk.vkGetPhysicalDeviceSurfaceFormatsKHR(app.phys, app.surface,
                                                       &fn, nullptr));
  std::vector<VkSurfaceFormatKHR> fmts(fn);
  VK_CHECK(app.vk.vkGetPhysicalDeviceSurfaceFormatsKHR(app.phys, app.surface,
                                                       &fn, fmts.data()));

  VkSurfaceFormatKHR chosen = fmts[0];
  for (auto &f : fmts) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
        f.colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR) {
      chosen = f;
      break;
    }
  }
  app.swap_format = chosen.format;

  VkExtent2D ext = caps.currentExtent;
  if (ext.width == 0xFFFFFFFFu) {
    ext.width = kInitialWidth;
    ext.height = kInitialHeight;
  }
  app.swap_extent = ext;

  uint32_t count = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && count > caps.maxImageCount)
    count = caps.maxImageCount;

  VkSwapchainCreateInfoKHR sci{};
  sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  sci.surface = app.surface;
  sci.minImageCount = count;
  sci.imageFormat = chosen.format;
  sci.imageColorSpace = chosen.colorSpace;
  sci.imageExtent = ext;
  sci.imageArrayLayers = 1;
  sci.imageUsage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  sci.clipped = VK_TRUE;

  VK_CHECK(
      app.vk.vkCreateSwapchainKHR(app.device, &sci, nullptr, &app.swapchain));

  uint32_t in = 0;
  VK_CHECK(
      app.vk.vkGetSwapchainImagesKHR(app.device, app.swapchain, &in, nullptr));
  app.swap_images.resize(in);
  VK_CHECK(app.vk.vkGetSwapchainImagesKHR(app.device, app.swapchain, &in,
                                          app.swap_images.data()));
  return true;
}

bool create_sync_and_cmds(App &app) {
  VkCommandPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = app.graphics_family;
  VK_CHECK(
      app.vk.vkCreateCommandPool(app.device, &pci, nullptr, &app.cmd_pool));

  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = app.cmd_pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VK_CHECK(app.vk.vkAllocateCommandBuffers(app.device, &ai, &app.cmd));

  VkSemaphoreCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VK_CHECK(
      app.vk.vkCreateSemaphore(app.device, &si, nullptr, &app.sem_acquire));
  VK_CHECK(app.vk.vkCreateSemaphore(app.device, &si, nullptr, &app.sem_render));

  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VK_CHECK(
      app.vk.vkCreateFence(app.device, &fi, nullptr, &app.fence_in_flight));
  return true;
}

void record_clear(App &app, uint32_t image_index, VkClearColorValue color) {
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  app.vk.vkBeginCommandBuffer(app.cmd, &bi);

  VkImageMemoryBarrier to_transfer{};
  to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_transfer.srcAccessMask = 0;
  to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_transfer.image = app.swap_images[image_index];
  to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  app.vk.vkCmdPipelineBarrier(app.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                              nullptr, 1, &to_transfer);

  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  app.vk.vkCmdClearColorImage(app.cmd, app.swap_images[image_index],
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                              &range);

  VkImageMemoryBarrier to_present = to_transfer;
  to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_present.dstAccessMask = 0;
  to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  app.vk.vkCmdPipelineBarrier(app.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                              nullptr, 0, nullptr, 1, &to_present);

  app.vk.vkEndCommandBuffer(app.cmd);
}

bool render_frame(App &app, float t) {
  app.vk.vkWaitForFences(app.device, 1, &app.fence_in_flight, VK_TRUE,
                         UINT64_MAX);
  app.vk.vkResetFences(app.device, 1, &app.fence_in_flight);

  uint32_t image_index = 0;
  VkResult ar = app.vk.vkAcquireNextImageKHR(app.device, app.swapchain,
                                             UINT64_MAX, app.sem_acquire,
                                             VK_NULL_HANDLE, &image_index);
  if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
    return true;
  }
  if (ar != VK_SUCCESS) {
    std::fprintf(stderr, "vk: acquire failed: %d\n", int(ar));
    return false;
  }

  VkClearColorValue color{};
  color.float32[0] = 0.5f + 0.5f * std::sin(t * 1.0f);
  color.float32[1] = 0.5f + 0.5f * std::sin(t * 1.3f + 2.0f);
  color.float32[2] = 0.5f + 0.5f * std::sin(t * 1.7f + 4.0f);
  color.float32[3] = 1.0f;

  app.vk.vkResetCommandBuffer(app.cmd, 0);
  record_clear(app, image_index, color);

  VkPipelineStageFlags wait_stage =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &app.sem_acquire;
  si.pWaitDstStageMask = &wait_stage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &app.cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &app.sem_render;

  VK_CHECK(app.vk.vkQueueSubmit(app.queue, 1, &si, app.fence_in_flight));

  VkPresentInfoKHR pi{};
  pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &app.sem_render;
  pi.swapchainCount = 1;
  pi.pSwapchains = &app.swapchain;
  pi.pImageIndices = &image_index;
  VkResult pr = app.vk.vkQueuePresentKHR(app.queue, &pi);
  if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR &&
      pr != VK_ERROR_OUT_OF_DATE_KHR) {
    std::fprintf(stderr, "vk: present failed: %d\n", int(pr));
    return false;
  }
  return true;
}

void destroy_vk(App &app) {
  if (app.device) {
    app.vk.vkDeviceWaitIdle(app.device);
    if (app.fence_in_flight)
      app.vk.vkDestroyFence(app.device, app.fence_in_flight, nullptr);
    if (app.sem_render)
      app.vk.vkDestroySemaphore(app.device, app.sem_render, nullptr);
    if (app.sem_acquire)
      app.vk.vkDestroySemaphore(app.device, app.sem_acquire, nullptr);
    if (app.cmd_pool)
      app.vk.vkDestroyCommandPool(app.device, app.cmd_pool, nullptr);
    if (app.swapchain)
      app.vk.vkDestroySwapchainKHR(app.device, app.swapchain, nullptr);
    app.vk.vkDestroyDevice(app.device, nullptr);
  }
  if (app.instance) {
    if (app.surface)
      app.vk.vkDestroySurfaceKHR(app.instance, app.surface, nullptr);
    app.vk.vkDestroyInstance(app.instance, nullptr);
  }
}

// Windows first-chance VEH. Registered with AddVectoredExceptionHandler
// (First=1) so it runs ahead of cosmo's own SEH→signal translator.
// Declared __ms_abi__ because Windows calls VEHs with the MS x64 ABI;
// we cast through cosmo's SysV-typed NtVectoredExceptionHandler at
// registration time.
//
// Responsibilities, in order:
//   1. Swallow benign first-chance exceptions Windows uses as side
//      channels (debug prints, thread-naming) so cosmo's handler
//      doesn't translate them into SIGSEGV noise.
//   2. For everything else, restore cosmo's TIB into %gs:0x30 and
//      return EXCEPTION_CONTINUE_SEARCH so cosmo's own VEH gets to
//      run with the TLS layout it expects. cosmo will either
//      longjmp() out to SysV code (which needs cosmo TLS) or
//      terminate the process — in both paths the original foreign
//      %gs is irrelevant, so we don't bother restoring it.
__attribute__((__ms_abi__)) static int32_t
win32_first_veh(struct NtExceptionPointers *info) {
  uint32_t code = info->ExceptionRecord->ExceptionCode;
  //   0x40010006 = DBG_PRINTEXCEPTION_C       (OutputDebugStringA)
  //   0x4001000A = DBG_PRINTEXCEPTION_WIDE_C
  //   0x406D1388 = MS_VC_EXCEPTION            (SetThreadName)
  if (code == 0x40010006u || code == 0x4001000Au || code == 0x406D1388u) {
    return -1; // EXCEPTION_CONTINUE_EXECUTION
  }
  // Real fault: hand it to cosmo with cosmo's TIB active so its
  // libc-using handler path (fprintf, sig helpers, nsync, errno)
  // doesn't NULL-deref through the foreign TEB.
  __set_tls(__get_tls_privileged());
  return 0; // EXCEPTION_CONTINUE_SEARCH
}

} // namespace

int main() {
  if (IsWindows()) {
    int64_t h = AddVectoredExceptionHandler(
        1, reinterpret_cast<NtVectoredExceptionHandler>(&win32_first_veh));
    std::fprintf(stderr, h ? "veh: installed\n" : "veh: install FAILED\n");
  } else {
    ShowCrashReports();
  }

  const char *libvk_name = IsWindows() ? "vulkan-1.dll" : "libvulkan.so.1";
  void *libvk = cosmo_dlopen(libvk_name, RTLD_NOW);
  if (!libvk) {
    std::fprintf(stderr, "cosmo_dlopen(%s) failed: %s\n", libvk_name,
                 cosmo_dlerror());
    return 1;
  }

  App app;
  if (!vk_load_loader(app.vk, libvk)) {
    destroy_vk(app);
    return 1;
  }
  if (!select_and_open_session(app)) {
    destroy_vk(app);
    return 1;
  }
  if (!create_instance(app)) {
    destroy_vk(app);
    return 1;
  }
  if (!create_surface(app)) {
    destroy_vk(app);
    return 1;
  }
  if (!pick_physical_device(app)) {
    destroy_vk(app);
    return 1;
  }
  if (!create_device(app)) {
    destroy_vk(app);
    return 1;
  }

  if (!create_swapchain(app)) {
    destroy_vk(app);
    return 1;
  }
  if (!create_sync_and_cmds(app)) {
    destroy_vk(app);
    return 1;
  }

  std::printf("entering render loop (Esc or WM-close to exit)\n");
  std::fflush(stdout);

  auto t0 = std::chrono::steady_clock::now();
  bool should_close = false;
  while (!should_close) {
    if (!app.session->poll_events(should_close))
      break;
    float t =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - t0)
            .count();
    if (!render_frame(app, t))
      break;
  }

  destroy_vk(app);
  return 0;
}
