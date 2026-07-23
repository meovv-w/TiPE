#pragma once

#include <fcitx/addoninstance.h>

namespace fcitx {
class InputContext;
}

FCITX_ADDON_DECLARE_FUNCTION(TipeUI, updateInputPanel,
                             void(fcitx::InputContext *));
