#pragma once

// WindowSession: a live connection to the host's windowing stack
// (XCB / Wayland / Win32). Owns the window and the event source.
//
// LLVM-style RTTI substitute: every subclass sets `kind` on construction
// and exposes a static `kKind`; isa<T> / cast<T> / dyn_cast<T> below give
// type-safe downcasting under -fno-rtti.

#include "vk_dispatch.hpp"

#include <cstdint>

class WindowSession {
public:
  enum Kind : uint8_t {
    Xcb,
    Wayland,
    Win32,
  };
  const Kind kind;

  virtual ~WindowSession() = default;

  // Opens the window. Return false on failure; error message printed to
  // stderr.
  virtual bool create_window(unsigned width, unsigned height,
                             const char *title) = 0;

  // Drains the event queue. Sets should_close to true on WM-close
  // request or Escape. Returns false if the connection has died.
  virtual bool poll_events(bool &should_close) = 0;

  // Vulkan surface extension this backend needs enabled on the
  // VkInstance (e.g. "VK_KHR_xcb_surface").
  virtual const char *vk_surface_extension_name() const = 0;

  // Builds the VkSurfaceKHR for the open window.
  virtual bool create_vk_surface(const VkApi &vk, VkInstance instance,
                                 VkSurfaceKHR *out_surface) = 0;

protected:
  explicit WindowSession(Kind k) : kind(k) {}
};

template <typename T> bool isa(const WindowSession &s) {
  return s.kind == T::kKind;
}

template <typename T> T *cast(WindowSession *s) { return static_cast<T *>(s); }

template <typename T> const T *cast(const WindowSession *s) {
  return static_cast<const T *>(s);
}

template <typename T> T *dyn_cast(WindowSession *s) {
  return (s && isa<T>(*s)) ? static_cast<T *>(s) : nullptr;
}

template <typename T> const T *dyn_cast(const WindowSession *s) {
  return (s && isa<T>(*s)) ? static_cast<const T *>(s) : nullptr;
}
