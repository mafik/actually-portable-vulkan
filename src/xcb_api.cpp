#include "xcb_api.hpp"

#include <libc/dlopen/dlfcn.h>

#include <cstdio>
#include <cstring>

namespace {

struct XcbPfns {
  xcb_connection_t *(*connect_)(const char *, int *) = nullptr;
  void (*disconnect_)(xcb_connection_t *) = nullptr;
  int (*connection_has_error_)(xcb_connection_t *) = nullptr;
  const xcb_setup_t *(*get_setup_)(xcb_connection_t *) = nullptr;
  uint32_t (*generate_id_)(xcb_connection_t *) = nullptr;
  int (*flush_)(xcb_connection_t *) = nullptr;
  xcb_generic_event_t *(*poll_for_event_)(xcb_connection_t *) = nullptr;

  xcb_void_cookie_t (*create_window_)(xcb_connection_t *, uint8_t, xcb_window_t,
                                      xcb_window_t, int16_t, int16_t, uint16_t,
                                      uint16_t, uint16_t, uint16_t,
                                      xcb_visualid_t, uint32_t,
                                      const void *) = nullptr;
  xcb_void_cookie_t (*map_window_)(xcb_connection_t *, xcb_window_t) = nullptr;
  xcb_void_cookie_t (*change_property_)(xcb_connection_t *, uint8_t,
                                        xcb_window_t, xcb_atom_t, xcb_atom_t,
                                        uint8_t, uint32_t,
                                        const void *) = nullptr;

  xcb_intern_atom_cookie_t (*intern_atom_)(xcb_connection_t *, uint8_t,
                                           uint16_t, const char *) = nullptr;
  xcb_intern_atom_reply_t *(*intern_atom_reply_)(
      xcb_connection_t *, xcb_intern_atom_cookie_t,
      xcb_generic_error_t **) = nullptr;

  xcb_screen_iterator_t (*setup_roots_iterator_)(const xcb_setup_t *) = nullptr;
  void (*screen_next_)(xcb_screen_iterator_t *) = nullptr;

  void (*free_)(void *) = nullptr;
};

XcbPfns g;
void *g_handle = nullptr;

} // namespace

bool xcb_api_init() {
  if (g_handle)
    return true;

  g_handle = cosmo_dlopen("libxcb.so.1", RTLD_NOW);
  if (!g_handle) {
    std::fprintf(stderr, "xcb: cosmo_dlopen(libxcb.so.1) failed: %s\n",
                 cosmo_dlerror());
    return false;
  }

#define LOAD(field, sym)                                                       \
  do {                                                                         \
    void *p = cosmo_dlsym(g_handle, sym);                                      \
    if (!p) {                                                                  \
      std::fprintf(stderr, "xcb: missing %s: %s\n", sym, cosmo_dlerror());     \
      return false;                                                            \
    }                                                                          \
    std::memcpy(&g.field, &p, sizeof p);                                       \
  } while (0)

  LOAD(connect_, "xcb_connect");
  LOAD(disconnect_, "xcb_disconnect");
  LOAD(connection_has_error_, "xcb_connection_has_error");
  LOAD(get_setup_, "xcb_get_setup");
  LOAD(generate_id_, "xcb_generate_id");
  LOAD(flush_, "xcb_flush");
  LOAD(poll_for_event_, "xcb_poll_for_event");
  LOAD(create_window_, "xcb_create_window");
  LOAD(map_window_, "xcb_map_window");
  LOAD(change_property_, "xcb_change_property");
  LOAD(intern_atom_, "xcb_intern_atom");
  LOAD(intern_atom_reply_, "xcb_intern_atom_reply");
  LOAD(setup_roots_iterator_, "xcb_setup_roots_iterator");
  LOAD(screen_next_, "xcb_screen_next");

#undef LOAD

  void *p = cosmo_dlsym(g_handle, "free");
  if (!p) {
    std::fprintf(stderr,
                 "xcb: cannot resolve glibc free() via libxcb handle: %s\n",
                 cosmo_dlerror());
    return false;
  }
  std::memcpy(&g.free_, &p, sizeof p);
  return true;
}

void xcb_api_shutdown() {
  if (g_handle) {
    cosmo_dlclose(g_handle);
    g_handle = nullptr;
  }
  g = XcbPfns{};
}

void xcb_free(void *p) {
  if (p && g.free_)
    g.free_(p);
}

extern "C" {

xcb_connection_t *xcb_connect(const char *display, int *screen) {
  return g.connect_(display, screen);
}

void xcb_disconnect(xcb_connection_t *c) { g.disconnect_(c); }

int xcb_connection_has_error(xcb_connection_t *c) {
  return g.connection_has_error_(c);
}

const xcb_setup_t *xcb_get_setup(xcb_connection_t *c) {
  return g.get_setup_(c);
}

uint32_t xcb_generate_id(xcb_connection_t *c) { return g.generate_id_(c); }

int xcb_flush(xcb_connection_t *c) { return g.flush_(c); }

xcb_generic_event_t *xcb_poll_for_event(xcb_connection_t *c) {
  return g.poll_for_event_(c);
}

xcb_void_cookie_t xcb_create_window(xcb_connection_t *c, uint8_t depth,
                                    xcb_window_t wid, xcb_window_t parent,
                                    int16_t x, int16_t y, uint16_t w,
                                    uint16_t h, uint16_t border, uint16_t cls,
                                    xcb_visualid_t visual, uint32_t mask,
                                    const void *values) {
  return g.create_window_(c, depth, wid, parent, x, y, w, h, border, cls,
                          visual, mask, values);
}

xcb_void_cookie_t xcb_map_window(xcb_connection_t *c, xcb_window_t w) {
  return g.map_window_(c, w);
}

xcb_void_cookie_t xcb_change_property(xcb_connection_t *c, uint8_t mode,
                                      xcb_window_t w, xcb_atom_t prop,
                                      xcb_atom_t type, uint8_t fmt,
                                      uint32_t len, const void *data) {
  return g.change_property_(c, mode, w, prop, type, fmt, len, data);
}

xcb_intern_atom_cookie_t xcb_intern_atom(xcb_connection_t *c,
                                         uint8_t only_if_exists,
                                         uint16_t name_len, const char *name) {
  return g.intern_atom_(c, only_if_exists, name_len, name);
}

xcb_intern_atom_reply_t *xcb_intern_atom_reply(xcb_connection_t *c,
                                               xcb_intern_atom_cookie_t cookie,
                                               xcb_generic_error_t **e) {
  return g.intern_atom_reply_(c, cookie, e);
}

xcb_screen_iterator_t xcb_setup_roots_iterator(const xcb_setup_t *s) {
  return g.setup_roots_iterator_(s);
}

void xcb_screen_next(xcb_screen_iterator_t *i) { g.screen_next_(i); }

} // extern "C"
