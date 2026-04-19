#pragma once

#include "wayland_api.hpp"
#include "window_session.hpp"

struct wl_registry;
struct wl_compositor;
struct wl_seat;
struct wl_keyboard;
struct wl_surface;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;

class WaylandWindowSession final : public WindowSession {
public:
  static constexpr Kind kKind = WindowSession::Wayland;

  WaylandWindowSession();
  ~WaylandWindowSession() override;

  bool create_window(unsigned width, unsigned height,
                     const char *title) override;
  bool poll_events(bool &should_close) override;
  const char *vk_surface_extension_name() const override;
  bool create_vk_surface(const VkApi &vk, VkInstance instance,
                         VkSurfaceKHR *out_surface) override;

  struct wl_display *display() const { return display_; }
  struct wl_surface *surface() const { return surface_; }

  // ---- registry / globals -------------------------------------------
  static void on_global(void *data, struct wl_registry *, uint32_t name,
                        const char *interface, uint32_t version);
  static void on_global_remove(void *data, struct wl_registry *, uint32_t name);

  // ---- xdg_wm_base --------------------------------------------------
  static void on_wm_base_ping(void *data, struct xdg_wm_base *,
                              uint32_t serial);

  // ---- xdg_surface --------------------------------------------------
  static void on_xdg_surface_configure(void *data, struct xdg_surface *,
                                       uint32_t serial);

  // ---- xdg_toplevel -------------------------------------------------
  static void on_xdg_toplevel_configure(void *data, struct xdg_toplevel *,
                                        int32_t width, int32_t height,
                                        struct wl_array *states);
  static void on_xdg_toplevel_close(void *data, struct xdg_toplevel *);
  static void on_xdg_toplevel_configure_bounds(void *data,
                                               struct xdg_toplevel *,
                                               int32_t width, int32_t height);
  static void on_xdg_toplevel_wm_capabilities(void *data, struct xdg_toplevel *,
                                              struct wl_array *caps);

  // ---- wl_seat ------------------------------------------------------
  static void on_seat_capabilities(void *data, struct wl_seat *, uint32_t caps);
  static void on_seat_name(void *data, struct wl_seat *, const char *name);

  // ---- wl_keyboard --------------------------------------------------
  static void on_kb_keymap(void *data, struct wl_keyboard *, uint32_t format,
                           int32_t fd, uint32_t size);
  static void on_kb_enter(void *data, struct wl_keyboard *, uint32_t serial,
                          struct wl_surface *, struct wl_array *keys);
  static void on_kb_leave(void *data, struct wl_keyboard *, uint32_t serial,
                          struct wl_surface *);
  static void on_kb_key(void *data, struct wl_keyboard *, uint32_t serial,
                        uint32_t time, uint32_t key, uint32_t state);
  static void on_kb_modifiers(void *data, struct wl_keyboard *, uint32_t serial,
                              uint32_t depressed, uint32_t latched,
                              uint32_t locked, uint32_t group);
  static void on_kb_repeat_info(void *data, struct wl_keyboard *, int32_t rate,
                                int32_t delay);

private:
  struct wl_display *display_ = nullptr;
  struct wl_registry *registry_ = nullptr;
  struct wl_compositor *compositor_ = nullptr;
  struct xdg_wm_base *wm_base_ = nullptr;
  struct wl_seat *seat_ = nullptr;
  struct wl_keyboard *keyboard_ = nullptr;
  struct wl_surface *surface_ = nullptr;
  struct xdg_surface *xdg_surface_ = nullptr;
  struct xdg_toplevel *xdg_toplevel_ = nullptr;

  uint32_t compositor_name_ = 0;
  uint32_t wm_base_name_ = 0;
  uint32_t seat_name_ = 0;

  bool configured_ = false;   // xdg_surface configure acked at least once
  bool should_close_ = false; // latched by toplevel close / Esc key
  bool error_ = false;        // connection lost
};
