#include "wayland_api.hpp"

// Deliberately *not* including <wayland-client-protocol.h> here: that
// header declares wl_registry_interface et al. as `extern const`, and we
// define them below as non-const so we can memcpy libwayland's tables
// into them at init. Mixing both in one TU would be a C-level const/non-
// const redeclaration error. Cross-TU mismatch is fine — the ELF symbol
// carries no const-ness.

#include <libc/dlopen/dlfcn.h>

#include <cstdio>
#include <cstring>

namespace {

struct WaylandPfns {
  // wl_display_* (opaque wl_display* from the caller's side)
  struct wl_display *(*display_connect)(const char *) = nullptr;
  void (*display_disconnect)(struct wl_display *) = nullptr;
  int (*display_get_fd)(struct wl_display *) = nullptr;
  int (*display_dispatch)(struct wl_display *) = nullptr;
  int (*display_dispatch_pending)(struct wl_display *) = nullptr;
  int (*display_flush)(struct wl_display *) = nullptr;
  int (*display_roundtrip)(struct wl_display *) = nullptr;

  // wl_proxy_* (variadic wl_proxy_marshal_flags is forwarded through an
  // asm trampoline below; this PFN holds the real target address.)
  void (*proxy_destroy)(struct wl_proxy *) = nullptr;
  int (*proxy_add_listener)(struct wl_proxy *, void (**)(void),
                            void *) = nullptr;
  uint32_t (*proxy_get_version)(struct wl_proxy *) = nullptr;
  void (*proxy_set_user_data)(struct wl_proxy *, void *) = nullptr;
  void *(*proxy_get_user_data)(struct wl_proxy *) = nullptr;
};

WaylandPfns g;
void *g_handle = nullptr;

struct InterfaceSlot {
  const char *sym;
  struct wl_interface *shadow; // our writable copy (symbol the linker sees)
};

} // namespace

// Writable shadows of libwayland's interface tables. Non-const so we can
// memcpy into them at init. Declared extern "C" so the symbol names match
// `extern const struct wl_interface wl_foo_interface;` in wayland-client-
// protocol.h / xdg-shell-protocol.c.
extern "C" {
struct wl_interface wl_registry_interface;
struct wl_interface wl_compositor_interface;
struct wl_interface wl_surface_interface;
struct wl_interface wl_seat_interface;
struct wl_interface wl_keyboard_interface;
struct wl_interface wl_callback_interface;
struct wl_interface wl_output_interface;
}

namespace {
const InterfaceSlot kInterfaceSlots[] = {
    {"wl_registry_interface", &wl_registry_interface},
    {"wl_compositor_interface", &wl_compositor_interface},
    {"wl_surface_interface", &wl_surface_interface},
    {"wl_seat_interface", &wl_seat_interface},
    {"wl_keyboard_interface", &wl_keyboard_interface},
    {"wl_callback_interface", &wl_callback_interface},
    {"wl_output_interface", &wl_output_interface},
};
} // namespace

// --- variadic wl_proxy_marshal_flags trampoline ----------------------------

extern "C" void *g_pfn_wl_proxy_marshal_flags;
void *g_pfn_wl_proxy_marshal_flags = nullptr;

#if defined(__x86_64__)
asm(".text\n"
    ".globl wl_proxy_marshal_flags\n"
    ".type  wl_proxy_marshal_flags, @function\n"
    "wl_proxy_marshal_flags:\n"
    "    jmpq *g_pfn_wl_proxy_marshal_flags(%rip)\n"
    ".size wl_proxy_marshal_flags, .-wl_proxy_marshal_flags\n");
#elif defined(__aarch64__)
asm(".text\n"
    ".globl wl_proxy_marshal_flags\n"
    ".type  wl_proxy_marshal_flags, %function\n"
    "wl_proxy_marshal_flags:\n"
    "    adrp x16, g_pfn_wl_proxy_marshal_flags\n"
    "    ldr  x16, [x16, #:lo12:g_pfn_wl_proxy_marshal_flags]\n"
    "    br   x16\n"
    ".size wl_proxy_marshal_flags, .-wl_proxy_marshal_flags\n");
#else
#error "wl_proxy_marshal_flags trampoline needs a port for this arch"
#endif

// --- init/shutdown --------------------------------------------------------

bool wayland_api_init() {
  if (g_handle)
    return true;

  g_handle = cosmo_dlopen("libwayland-client.so.0", RTLD_NOW);
  if (!g_handle) {
    std::fprintf(stderr,
                 "wayland: cosmo_dlopen(libwayland-client.so.0) failed: %s\n",
                 cosmo_dlerror());
    return false;
  }

#define LOAD(field, sym)                                                       \
  do {                                                                         \
    void *p = cosmo_dlsym(g_handle, sym);                                      \
    if (!p) {                                                                  \
      std::fprintf(stderr, "wayland: missing %s: %s\n", sym, cosmo_dlerror()); \
      return false;                                                            \
    }                                                                          \
    std::memcpy(&g.field, &p, sizeof p);                                       \
  } while (0)

  LOAD(display_connect, "wl_display_connect");
  LOAD(display_disconnect, "wl_display_disconnect");
  LOAD(display_get_fd, "wl_display_get_fd");
  LOAD(display_dispatch, "wl_display_dispatch");
  LOAD(display_dispatch_pending, "wl_display_dispatch_pending");
  LOAD(display_flush, "wl_display_flush");
  LOAD(display_roundtrip, "wl_display_roundtrip");
  LOAD(proxy_destroy, "wl_proxy_destroy");
  LOAD(proxy_add_listener, "wl_proxy_add_listener");
  LOAD(proxy_get_version, "wl_proxy_get_version");
  LOAD(proxy_set_user_data, "wl_proxy_set_user_data");
  LOAD(proxy_get_user_data, "wl_proxy_get_user_data");

#undef LOAD

  g_pfn_wl_proxy_marshal_flags =
      cosmo_dlsym(g_handle, "wl_proxy_marshal_flags");
  if (!g_pfn_wl_proxy_marshal_flags) {
    std::fprintf(stderr, "wayland: missing wl_proxy_marshal_flags: %s\n",
                 cosmo_dlerror());
    return false;
  }

  for (const auto &slot : kInterfaceSlots) {
    void *p = cosmo_dlsym(g_handle, slot.sym);
    if (!p) {
      std::fprintf(stderr, "wayland: missing %s: %s\n", slot.sym,
                   cosmo_dlerror());
      return false;
    }
    std::memcpy(slot.shadow, p, sizeof(struct wl_interface));
  }

  return true;
}

void wayland_api_shutdown() {
  if (g_handle) {
    cosmo_dlclose(g_handle);
    g_handle = nullptr;
  }
  g = WaylandPfns{};
  g_pfn_wl_proxy_marshal_flags = nullptr;
  for (const auto &slot : kInterfaceSlots) {
    std::memset(slot.shadow, 0, sizeof(struct wl_interface));
  }
}

// --- shadow wrappers matching libwayland's public signatures --------------

extern "C" {

struct wl_display *wl_display_connect(const char *name) {
  return g.display_connect(name);
}

void wl_display_disconnect(struct wl_display *d) { g.display_disconnect(d); }

int wl_display_get_fd(struct wl_display *d) { return g.display_get_fd(d); }
int wl_display_dispatch(struct wl_display *d) { return g.display_dispatch(d); }
int wl_display_dispatch_pending(struct wl_display *d) {
  return g.display_dispatch_pending(d);
}
int wl_display_flush(struct wl_display *d) { return g.display_flush(d); }
int wl_display_roundtrip(struct wl_display *d) {
  return g.display_roundtrip(d);
}

void wl_proxy_destroy(struct wl_proxy *p) { g.proxy_destroy(p); }

int wl_proxy_add_listener(struct wl_proxy *p, void (**impl)(void), void *data) {
  return g.proxy_add_listener(p, impl, data);
}

uint32_t wl_proxy_get_version(struct wl_proxy *p) {
  return g.proxy_get_version(p);
}
void wl_proxy_set_user_data(struct wl_proxy *p, void *data) {
  g.proxy_set_user_data(p, data);
}
void *wl_proxy_get_user_data(struct wl_proxy *p) {
  return g.proxy_get_user_data(p);
}

} // extern "C"
