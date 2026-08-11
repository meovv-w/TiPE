#pragma once

#include <cstdint>
#include <optional>

namespace tipe {

enum class ShiftKey : std::uint8_t {
    Left,
    Right,
};

class ShiftTapTracker {
public:
    void press(const void *inputContext, ShiftKey key) {
        if (!inputContext) {
            cancel();
            return;
        }
        if (!pending_) {
            pending_ = Pending{inputContext, key};
            return;
        }
        if (pending_->inputContext != inputContext || pending_->key != key) {
            cancel();
        }
    }

    void nonShiftPressed() { cancel(); }

    bool release(const void *inputContext, ShiftKey key) {
        const bool tapped = pending_ && pending_->inputContext == inputContext && pending_->key == key;
        cancel();
        return tapped;
    }

    void cancelForContext(const void *inputContext) {
        if (pending_ && pending_->inputContext == inputContext) {
            cancel();
        }
    }

    void cancel() { pending_.reset(); }

private:
    struct Pending {
        const void *inputContext = nullptr;
        ShiftKey key = ShiftKey::Left;
    };

    std::optional<Pending> pending_;
};

} // namespace tipe
