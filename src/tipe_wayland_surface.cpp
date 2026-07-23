#include "wl_surface.h"

namespace fcitx::wayland {

const struct wl_surface_listener WlSurface::listener = {
    .enter = [](void *, wl_surface *, wl_output *) {},
    .leave = [](void *, wl_surface *, wl_output *) {},
    .preferred_buffer_scale =
        [](void *data, wl_surface *wldata, int32_t factor) {
            auto *obj = static_cast<WlSurface *>(data);
            if (obj && static_cast<wl_surface *>(*obj) == wldata) {
                obj->preferredBufferScale()(factor);
            }
        },
    .preferred_buffer_transform =
        [](void *data, wl_surface *wldata, uint32_t transform) {
            auto *obj = static_cast<WlSurface *>(data);
            if (obj && static_cast<wl_surface *>(*obj) == wldata) {
                obj->preferredBufferTransform()(transform);
            }
        },
};

WlSurface::WlSurface(wl_surface *data)
    : version_(wl_surface_get_version(data)), data_(data) {
    wl_surface_set_user_data(*this, this);
    wl_surface_add_listener(*this, &WlSurface::listener, this);
}

void WlSurface::destructor(wl_surface *data) { wl_surface_destroy(data); }

void WlSurface::attach(WlBuffer *, int32_t, int32_t) {}

void WlSurface::damage(int32_t x, int32_t y, int32_t width, int32_t height) {
    wl_surface_damage(*this, x, y, width, height);
}

WlCallback *WlSurface::frame() { return nullptr; }

void WlSurface::setOpaqueRegion(WlRegion *) {}

void WlSurface::setInputRegion(WlRegion *) {}

void WlSurface::commit() { wl_surface_commit(*this); }

void WlSurface::setBufferTransform(int32_t transform) {
    wl_surface_set_buffer_transform(*this, transform);
}

void WlSurface::setBufferScale(int32_t scale) {
    wl_surface_set_buffer_scale(*this, scale);
}

void WlSurface::damageBuffer(int32_t x, int32_t y, int32_t width,
                             int32_t height) {
    wl_surface_damage_buffer(*this, x, y, width, height);
}

void WlSurface::offset(int32_t x, int32_t y) {
#if defined(WL_SURFACE_OFFSET_SINCE_VERSION)
    if (version_ >= WL_SURFACE_OFFSET_SINCE_VERSION) {
        wl_surface_offset(*this, x, y);
    }
#else
    (void)x;
    (void)y;
#endif
}

} // namespace fcitx::wayland
