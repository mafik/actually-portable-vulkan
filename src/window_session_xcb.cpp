#include "window_session_xcb.hpp"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_xcb.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint8_t kEscapeKeycode = 9;

xcb_atom_t intern_atom(xcb_connection_t *c, const char *name) {
  xcb_intern_atom_cookie_t cookie =
      xcb_intern_atom(c, 0, static_cast<uint16_t>(std::strlen(name)), name);
  xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, cookie, nullptr);
  xcb_atom_t atom = r ? r->atom : 0;
  xcb_free(r);
  return atom;
}

} // namespace

XcbWindowSession::XcbWindowSession() : WindowSession(kKind) {}

XcbWindowSession::~XcbWindowSession() {
  if (conn_)
    xcb_disconnect(conn_);
  xcb_api_shutdown();
}

bool XcbWindowSession::create_window(unsigned width, unsigned height,
                                     const char *title) {
  if (!xcb_api_init())
    return false;

  int screen_num = 0;
  conn_ = xcb_connect(nullptr, &screen_num);
  if (!conn_ || xcb_connection_has_error(conn_)) {
    std::fprintf(stderr, "xcb: connect failed (DISPLAY=%s)\n",
                 std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "(unset)");
    return false;
  }

  const xcb_setup_t *setup = xcb_get_setup(conn_);
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
  for (int i = 0; i < screen_num && it.rem; ++i)
    xcb_screen_next(&it);
  screen_ = it.data;
  if (!screen_) {
    std::fprintf(stderr, "xcb: no screen for screen_num=%d\n", screen_num);
    return false;
  }

  window_ = xcb_generate_id(conn_);

  uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  uint32_t values[2] = {
      screen_->black_pixel,
      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS |
          XCB_EVENT_MASK_STRUCTURE_NOTIFY,
  };

  xcb_create_window(conn_, XCB_COPY_FROM_PARENT, window_, screen_->root, 0, 0,
                    static_cast<uint16_t>(width), static_cast<uint16_t>(height),
                    0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual,
                    mask, values);

  xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, XCB_ATOM_WM_NAME,
                      XCB_ATOM_STRING, 8,
                      static_cast<uint32_t>(std::strlen(title)), title);

  wm_protocols_ = intern_atom(conn_, "WM_PROTOCOLS");
  wm_delete_window_ = intern_atom(conn_, "WM_DELETE_WINDOW");
  xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, wm_protocols_,
                      XCB_ATOM_ATOM, 32, 1, &wm_delete_window_);

  xcb_map_window(conn_, window_);
  xcb_flush(conn_);
  return true;
}

bool XcbWindowSession::poll_events(bool &should_close) {
  while (xcb_generic_event_t *ev = xcb_poll_for_event(conn_)) {
    switch (ev->response_type & 0x7f) {
    case XCB_CLIENT_MESSAGE: {
      auto *cm = reinterpret_cast<xcb_client_message_event_t *>(ev);
      if (cm->data.data32[0] == wm_delete_window_)
        should_close = true;
      break;
    }
    case XCB_KEY_PRESS: {
      auto *kp = reinterpret_cast<xcb_key_press_event_t *>(ev);
      if (kp->detail == kEscapeKeycode)
        should_close = true;
      break;
    }
    default:
      break;
    }
    xcb_free(ev);
  }
  return xcb_connection_has_error(conn_) == 0;
}

const char *XcbWindowSession::vk_surface_extension_name() const {
  return VK_KHR_XCB_SURFACE_EXTENSION_NAME;
}

bool XcbWindowSession::create_vk_surface(const VkApi &vk, VkInstance instance,
                                         VkSurfaceKHR *out_surface) {
  auto pfn = reinterpret_cast<PFN_vkCreateXcbSurfaceKHR>(
      vk_trampoline(reinterpret_cast<void *>(
          vk.vkGetInstanceProcAddr(instance, "vkCreateXcbSurfaceKHR"))));
  if (!pfn) {
    std::fprintf(stderr, "vk: vkCreateXcbSurfaceKHR unavailable (is "
                         "VK_KHR_xcb_surface enabled?)\n");
    return false;
  }

  VkXcbSurfaceCreateInfoKHR ci{};
  ci.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
  ci.connection = conn_;
  ci.window = window_;
  VkResult r = pfn(instance, &ci, nullptr, out_surface);
  if (r != VK_SUCCESS) {
    std::fprintf(stderr, "vk: vkCreateXcbSurfaceKHR failed: %d\n", int(r));
    return false;
  }
  return true;
}
