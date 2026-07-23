#pragma once

#include <fcitx/addoninstance.h>
#include <fcitx/inputcontext.h>
#include <fcitx-utils/metastring.h>

namespace fcitx::wayland {
class ZwpInputMethodV2;
}

FCITX_ADDON_DECLARE_FUNCTION(
    WaylandIMModule, getInputMethodV2,
    fcitx::wayland::ZwpInputMethodV2 *(fcitx::InputContext *));
