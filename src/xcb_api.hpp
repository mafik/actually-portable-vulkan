#pragma once

#include <xcb/xcb.h>
#include <xcb/xproto.h>

bool xcb_api_init();

void xcb_api_shutdown();

// Glibc free, resolved out of libxcb's namespace.
void xcb_free(void *p);
