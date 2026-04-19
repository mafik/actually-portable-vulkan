#pragma once

#include "window_session.hpp"

#include <cstdint>

class Win32WindowSession final : public WindowSession {
public:
  static constexpr Kind kKind = WindowSession::Win32;

  Win32WindowSession();
  ~Win32WindowSession() override;

  bool create_window(unsigned width, unsigned height,
                     const char *title) override;
  bool poll_events(bool &should_close) override;
  const char *vk_surface_extension_name() const override;
  bool create_vk_surface(const VkApi &vk, VkInstance instance,
                         VkSurfaceKHR *out_surface) override;

  int64_t hwnd() const { return hwnd_; }
  int64_t hinstance() const { return hinstance_; }

private:
  static __attribute__((__ms_abi__)) int64_t wndproc(int64_t hwnd, uint32_t msg,
                                                     uint64_t wparam,
                                                     int64_t lparam);

  int64_t hwnd_ = 0;
  int64_t hinstance_ = 0;
  bool should_close_ = false;
  bool error_ = false;
};
