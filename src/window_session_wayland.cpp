#include "window_session_wayland.hpp"

#include "xdg-shell-client-protocol.h"
#include <wayland-client-protocol.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_wayland.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <linux/input-event-codes.h> // KEY_ESC (evdev keycodes)
#include <unistd.h>                  // ::close (for the keymap fd)

namespace {

constexpr uint32_t kWlCompositorVersion = 4;
constexpr uint32_t kXdgWmBaseVersion = 1;
constexpr uint32_t kWlSeatVersion = 5;

const struct xdg_wm_base_listener kWmBaseListener = {
    .ping = &WaylandWindowSession::on_wm_base_ping,
};

const struct wl_registry_listener kRegistryListener = {
    .global = &WaylandWindowSession::on_global,
    .global_remove = &WaylandWindowSession::on_global_remove,
};

const struct xdg_surface_listener kXdgSurfaceListener = {
    .configure = &WaylandWindowSession::on_xdg_surface_configure,
};

const struct xdg_toplevel_listener kXdgToplevelListener = {
    .configure = &WaylandWindowSession::on_xdg_toplevel_configure,
    .close = &WaylandWindowSession::on_xdg_toplevel_close,
    .configure_bounds = &WaylandWindowSession::on_xdg_toplevel_configure_bounds,
    .wm_capabilities = &WaylandWindowSession::on_xdg_toplevel_wm_capabilities,
};

const struct wl_seat_listener kSeatListener = {
    .capabilities = &WaylandWindowSession::on_seat_capabilities,
    .name = &WaylandWindowSession::on_seat_name,
};

const struct wl_keyboard_listener kKeyboardListener = {
    .keymap = &WaylandWindowSession::on_kb_keymap,
    .enter = &WaylandWindowSession::on_kb_enter,
    .leave = &WaylandWindowSession::on_kb_leave,
    .key = &WaylandWindowSession::on_kb_key,
    .modifiers = &WaylandWindowSession::on_kb_modifiers,
    .repeat_info = &WaylandWindowSession::on_kb_repeat_info,
};

} // namespace

WaylandWindowSession::WaylandWindowSession() : WindowSession(kKind) {}

WaylandWindowSession::~WaylandWindowSession() {
  if (keyboard_)
    wl_keyboard_release(keyboard_);
  if (seat_)
    wl_seat_release(seat_);
  if (xdg_toplevel_)
    xdg_toplevel_destroy(xdg_toplevel_);
  if (xdg_surface_)
    xdg_surface_destroy(xdg_surface_);
  if (surface_)
    wl_surface_destroy(surface_);
  if (wm_base_)
    xdg_wm_base_destroy(wm_base_);
  if (compositor_)
    wl_proxy_destroy(reinterpret_cast<wl_proxy *>(compositor_));
  if (registry_)
    wl_registry_destroy(registry_);
  if (display_)
    wl_display_disconnect(display_);
  wayland_api_shutdown();
}

bool WaylandWindowSession::create_window(unsigned width, unsigned height,
                                         const char *title) {
  if (!wayland_api_init())
    return false;

  display_ = wl_display_connect(nullptr);
  if (!display_) {
    std::fprintf(stderr,
                 "wayland: wl_display_connect failed (WAYLAND_DISPLAY=%s)\n",
                 std::getenv("WAYLAND_DISPLAY") ? std::getenv("WAYLAND_DISPLAY")
                                                : "(unset)");
    return false;
  }

  registry_ = wl_display_get_registry(display_);
  wl_registry_add_listener(registry_, &kRegistryListener, this);
  // First roundtrip: registry globals arrive (and our on_global binds
  // compositor/wm_base/seat). Second roundtrip: seat capabilities
  // arrive (and we create the keyboard).
  wl_display_roundtrip(display_);
  wl_display_roundtrip(display_);

  if (!compositor_ || !wm_base_) {
    std::fprintf(stderr,
                 "wayland: missing required globals "
                 "(compositor=%p wm_base=%p)\n",
                 static_cast<void *>(compositor_),
                 static_cast<void *>(wm_base_));
    return false;
  }

  xdg_wm_base_add_listener(wm_base_, &kWmBaseListener, this);

  surface_ = wl_compositor_create_surface(compositor_);
  xdg_surface_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
  xdg_surface_add_listener(xdg_surface_, &kXdgSurfaceListener, this);

  xdg_toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
  xdg_toplevel_add_listener(xdg_toplevel_, &kXdgToplevelListener, this);
  xdg_toplevel_set_title(xdg_toplevel_, title);
  xdg_toplevel_set_app_id(xdg_toplevel_, "apv");

  wl_surface_commit(surface_);

  while (!configured_ && !error_) {
    if (wl_display_dispatch(display_) < 0) {
      error_ = true;
      break;
    }
  }
  (void)width;
  (void)height;
  return !error_;
}

bool WaylandWindowSession::poll_events(bool &should_close) {
  if (error_)
    return false;
  if (wl_display_dispatch_pending(display_) < 0) {
    error_ = true;
    return false;
  }
  if (wl_display_flush(display_) < 0 && errno != EAGAIN) {
    error_ = true;
    return false;
  }
  if (should_close_)
    should_close = true;
  return !error_;
}

const char *WaylandWindowSession::vk_surface_extension_name() const {
  return VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
}

bool WaylandWindowSession::create_vk_surface(const VkApi &vk,
                                             VkInstance instance,
                                             VkSurfaceKHR *out_surface) {
  auto pfn = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
      vk_trampoline(reinterpret_cast<void *>(
          vk.vkGetInstanceProcAddr(instance, "vkCreateWaylandSurfaceKHR"))));
  if (!pfn) {
    std::fprintf(stderr, "vk: vkCreateWaylandSurfaceKHR unavailable (is "
                         "VK_KHR_wayland_surface enabled?)\n");
    return false;
  }

  VkWaylandSurfaceCreateInfoKHR ci{};
  ci.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
  ci.display = display_;
  ci.surface = surface_;
  VkResult r = pfn(instance, &ci, nullptr, out_surface);
  if (r != VK_SUCCESS) {
    std::fprintf(stderr, "vk: vkCreateWaylandSurfaceKHR failed: %d\n", int(r));
    return false;
  }
  return true;
}

// --- registry --------------------------------------------------------------

void WaylandWindowSession::on_global(void *data, struct wl_registry *reg,
                                     uint32_t name, const char *interface,
                                     uint32_t version) {
  auto *self = static_cast<WaylandWindowSession *>(data);

  if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
    uint32_t v =
        version < kWlCompositorVersion ? version : kWlCompositorVersion;
    self->compositor_ = static_cast<wl_compositor *>(
        wl_registry_bind(reg, name, &wl_compositor_interface, v));
    self->compositor_name_ = name;
  } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
    uint32_t v = version < kXdgWmBaseVersion ? version : kXdgWmBaseVersion;
    self->wm_base_ = static_cast<xdg_wm_base *>(
        wl_registry_bind(reg, name, &xdg_wm_base_interface, v));
    self->wm_base_name_ = name;
  } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
    uint32_t v = version < kWlSeatVersion ? version : kWlSeatVersion;
    self->seat_ = static_cast<wl_seat *>(
        wl_registry_bind(reg, name, &wl_seat_interface, v));
    self->seat_name_ = name;
    wl_seat_add_listener(self->seat_, &kSeatListener, self);
  }
}

void WaylandWindowSession::on_global_remove(void *data, struct wl_registry *,
                                            uint32_t name) {
  auto *self = static_cast<WaylandWindowSession *>(data);
  if (name == self->compositor_name_ || name == self->wm_base_name_) {
    self->error_ = true;
  }
}

// --- xdg_wm_base / xdg_surface / xdg_toplevel -----------------------------

void WaylandWindowSession::on_wm_base_ping(void *data, struct xdg_wm_base *wm,
                                           uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(wm, serial);
}

void WaylandWindowSession::on_xdg_surface_configure(void *data,
                                                    struct xdg_surface *xs,
                                                    uint32_t serial) {
  auto *self = static_cast<WaylandWindowSession *>(data);
  xdg_surface_ack_configure(xs, serial);
  self->configured_ = true;
}

void WaylandWindowSession::on_xdg_toplevel_configure(void *,
                                                     struct xdg_toplevel *,
                                                     int32_t, int32_t,
                                                     struct wl_array *) {}

void WaylandWindowSession::on_xdg_toplevel_close(void *data,
                                                 struct xdg_toplevel *) {
  auto *self = static_cast<WaylandWindowSession *>(data);
  self->should_close_ = true;
}

void WaylandWindowSession::on_xdg_toplevel_configure_bounds(
    void *, struct xdg_toplevel *, int32_t, int32_t) {}

void WaylandWindowSession::on_xdg_toplevel_wm_capabilities(
    void *, struct xdg_toplevel *, struct wl_array *) {}

// --- wl_seat / wl_keyboard ------------------------------------------------

void WaylandWindowSession::on_seat_capabilities(void *data,
                                                struct wl_seat *seat,
                                                uint32_t caps) {
  auto *self = static_cast<WaylandWindowSession *>(data);
  const bool has_kb = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
  if (has_kb && !self->keyboard_) {
    self->keyboard_ = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(self->keyboard_, &kKeyboardListener, self);
  } else if (!has_kb && self->keyboard_) {
    wl_keyboard_release(self->keyboard_);
    self->keyboard_ = nullptr;
  }
}

void WaylandWindowSession::on_seat_name(void *, struct wl_seat *,
                                        const char *) {}

void WaylandWindowSession::on_kb_keymap(void *, struct wl_keyboard *, uint32_t,
                                        int32_t fd, uint32_t) {
  if (fd >= 0)
    ::close(fd);
}

void WaylandWindowSession::on_kb_enter(void *, struct wl_keyboard *, uint32_t,
                                       struct wl_surface *, struct wl_array *) {
}

void WaylandWindowSession::on_kb_leave(void *, struct wl_keyboard *, uint32_t,
                                       struct wl_surface *) {}

void WaylandWindowSession::on_kb_key(void *data, struct wl_keyboard *, uint32_t,
                                     uint32_t, uint32_t key, uint32_t state) {
  auto *self = static_cast<WaylandWindowSession *>(data);
  if (state == WL_KEYBOARD_KEY_STATE_PRESSED && key == KEY_ESC) {
    self->should_close_ = true;
  }
}

void WaylandWindowSession::on_kb_modifiers(void *, struct wl_keyboard *,
                                           uint32_t, uint32_t, uint32_t,
                                           uint32_t, uint32_t) {}

void WaylandWindowSession::on_kb_repeat_info(void *, struct wl_keyboard *,
                                             int32_t, int32_t) {}
