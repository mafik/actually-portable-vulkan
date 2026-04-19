#include "vk_dispatch.hpp"

#include <libc/dlopen/dlfcn.h>

#include <cstdio>
#include <cstring>

void *vk_trampoline(void *p) { return p ? cosmo_dltramp(p) : nullptr; }

bool vk_load_loader(VkApi &api, void *libvulkan_handle) {
  void *p = cosmo_dlsym(libvulkan_handle, "vkGetInstanceProcAddr");
  if (!p) {
    std::fprintf(stderr,
                 "vk: missing vkGetInstanceProcAddr in libvulkan.so.1\n");
    return false;
  }
  p = vk_trampoline(p);
  std::memcpy(&api.vkGetInstanceProcAddr, &p, sizeof p);
  return true;
}

bool vk_load_global(VkApi &api) {
#define X(name)                                                                \
  api.name =                                                                   \
      reinterpret_cast<PFN_##name>(vk_trampoline(reinterpret_cast<void *>(     \
          api.vkGetInstanceProcAddr(VK_NULL_HANDLE, #name))));                 \
  if (!api.name) {                                                             \
    std::fprintf(stderr, "vk: missing global %s\n", #name);                    \
    return false;                                                              \
  }
  VK_GLOBAL_FNS(X)
#undef X
  return true;
}

bool vk_load_instance(VkApi &api, VkInstance inst) {
#define X(name)                                                                \
  api.name = reinterpret_cast<PFN_##name>(vk_trampoline(                       \
      reinterpret_cast<void *>(api.vkGetInstanceProcAddr(inst, #name))));      \
  if (!api.name) {                                                             \
    std::fprintf(stderr, "vk: missing instance %s\n", #name);                  \
    return false;                                                              \
  }
  VK_INSTANCE_FNS(X)
#undef X
  return true;
}

bool vk_load_device(VkApi &api, VkDevice dev) {
#define X(name)                                                                \
  api.name = reinterpret_cast<PFN_##name>(vk_trampoline(                       \
      reinterpret_cast<void *>(api.vkGetDeviceProcAddr(dev, #name))));         \
  if (!api.name) {                                                             \
    std::fprintf(stderr, "vk: missing device %s\n", #name);                    \
    return false;                                                              \
  }
  VK_DEVICE_FNS(X)
#undef X
  return true;
}
