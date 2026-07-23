#ifndef ZWP_INPUT_METHOD_KEYBOARD_GRAB_V2_H_
#define ZWP_INPUT_METHOD_KEYBOARD_GRAB_V2_H_

#include <wayland-client.h>
#include "fcitx-utils/misc.h"
#include "wayland-input-method-unstable-v2-client-protocol.h"

namespace fcitx::wayland {

class ZwpInputMethodKeyboardGrabV2 final {
public:
    using wlType = zwp_input_method_keyboard_grab_v2;
    operator zwp_input_method_keyboard_grab_v2 *() { return data_.get(); }
    explicit ZwpInputMethodKeyboardGrabV2(wlType *data) : data_(data) {}

private:
    static void destructor(zwp_input_method_keyboard_grab_v2 *data) {
        zwp_input_method_keyboard_grab_v2_release(data);
    }

    UniqueCPtr<zwp_input_method_keyboard_grab_v2, &destructor> data_;
};

} // namespace fcitx::wayland

#endif // ZWP_INPUT_METHOD_KEYBOARD_GRAB_V2_H_
