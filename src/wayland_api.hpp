#pragma once

#include <wayland-client-core.h>
#include <wayland-util.h>

// Loads libwayland-client.so.0 via cosmo_dlopen, resolves the wl_display_*
// / wl_proxy_* functions used by our wrappers and the generated xdg-shell
// protocol, and memcpy's the wl_*_interface metadata from libwayland into
// writable shadow copies we expose with the expected extern "C" symbol
// names. Must be called before any wayland call. Returns false on failure
// (message printed to stderr).
bool wayland_api_init();

void wayland_api_shutdown();
