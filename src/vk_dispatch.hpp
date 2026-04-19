#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

// Platform-specific surface extensions (VK_KHR_xcb_surface,
// VK_KHR_wayland_surface, ...) are intentionally *not* part of VkApi.
// Concrete WindowSession subclasses resolve their own
// vkCreate{Xcb,Wayland,...}SurfaceKHR via vkGetInstanceProcAddr after
// creating the instance with the matching extension enabled. This keeps
// VkApi windowing-stack-neutral.

#define VK_GLOBAL_FNS(X)                                                       \
  X(vkEnumerateInstanceVersion)                                                \
  X(vkEnumerateInstanceExtensionProperties)                                    \
  X(vkCreateInstance)

#define VK_INSTANCE_FNS(X)                                                     \
  X(vkDestroyInstance)                                                         \
  X(vkEnumeratePhysicalDevices)                                                \
  X(vkGetPhysicalDeviceProperties)                                             \
  X(vkGetPhysicalDeviceQueueFamilyProperties)                                  \
  X(vkGetPhysicalDeviceSurfaceSupportKHR)                                      \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                 \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                      \
  X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                 \
  X(vkDestroySurfaceKHR)                                                       \
  X(vkEnumerateDeviceExtensionProperties)                                      \
  X(vkCreateDevice)                                                            \
  X(vkDestroyDevice)                                                           \
  X(vkGetDeviceProcAddr)

#define VK_DEVICE_FNS(X)                                                       \
  X(vkGetDeviceQueue)                                                          \
  X(vkDeviceWaitIdle)                                                          \
  X(vkCreateSwapchainKHR)                                                      \
  X(vkDestroySwapchainKHR)                                                     \
  X(vkGetSwapchainImagesKHR)                                                   \
  X(vkAcquireNextImageKHR)                                                     \
  X(vkQueuePresentKHR)                                                         \
  X(vkQueueSubmit)                                                             \
  X(vkCreateCommandPool)                                                       \
  X(vkDestroyCommandPool)                                                      \
  X(vkAllocateCommandBuffers)                                                  \
  X(vkFreeCommandBuffers)                                                      \
  X(vkBeginCommandBuffer)                                                      \
  X(vkEndCommandBuffer)                                                        \
  X(vkResetCommandBuffer)                                                      \
  X(vkCmdPipelineBarrier)                                                      \
  X(vkCmdClearColorImage)                                                      \
  X(vkCreateSemaphore)                                                         \
  X(vkDestroySemaphore)                                                        \
  X(vkCreateFence)                                                             \
  X(vkDestroyFence)                                                            \
  X(vkWaitForFences)                                                           \
  X(vkResetFences)

struct VkApi {
  PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

#define X(name) PFN_##name name = nullptr;
  VK_GLOBAL_FNS(X)
  VK_INSTANCE_FNS(X)
  VK_DEVICE_FNS(X)
#undef X
};

bool vk_load_loader(VkApi &api, void *libvulkan_handle);
bool vk_load_global(VkApi &api);
bool vk_load_instance(VkApi &api, VkInstance inst);
bool vk_load_device(VkApi &api, VkDevice dev);

// Wraps a raw function pointer returned by vkGet*ProcAddr (or a platform
// dlsym) so it's safe to invoke from SysV code. Identity on non-Windows
// hosts; on Windows it emits an MS→SysV ABI shim around `p`. Use this on
// every PFN pulled out of vulkan-1.dll.
void *vk_trampoline(void *p);
