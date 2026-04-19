#pragma once

#include "window_session.hpp"
#include "xcb_api.hpp"

class XcbWindowSession final : public WindowSession {
public:
    static constexpr Kind kKind = WindowSession::Xcb;

    XcbWindowSession();
    ~XcbWindowSession() override;

    bool create_window(unsigned width, unsigned height, const char* title) override;
    bool poll_events(bool& should_close) override;
    const char* vk_surface_extension_name() const override;
    bool create_vk_surface(const VkApi& vk, VkInstance instance,
                           VkSurfaceKHR* out_surface) override;

    xcb_connection_t* conn()   const { return conn_; }
    xcb_window_t      window() const { return window_; }

private:
    xcb_connection_t* conn_              = nullptr;
    xcb_screen_t*     screen_            = nullptr;
    xcb_window_t      window_            = 0;
    xcb_atom_t        wm_protocols_      = 0;
    xcb_atom_t        wm_delete_window_  = 0;
};
