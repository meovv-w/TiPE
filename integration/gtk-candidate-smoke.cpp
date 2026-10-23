// Headless checks of the actual patched GTK InputWindow signal/render route.
// No gtk_init(), windows, input injection, or connection to a running Fcitx.
#include "inputwindow.h"
#include "fcitxtheme.h"
#include "fcitx-gclient/fcitxgwatcher.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
int selectedCandidate = -1;
void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
class Window : public fcitx::gtk::InputWindow {
public:
    using InputWindow::InputWindow;
    void update() override { if (visible()) sizeHint(); }
    bool isTipe() const { return tipeInputMethod_; }
    const auto &frame() const { return tipeFrame_; }
    int selected() const { return highlight(); }
};
}

// Inspect the emitted D-Bus page-local selection without using a daemon.
extern "C" void __wrap_fcitx_g_client_select_candidate(FcitxGClient *, int index) {
    selectedCandidate = index;
}

int main() {
    auto *watcher = fcitx_g_watcher_new(); // Intentionally never watch a bus.
    auto *client = fcitx_g_client_new_with_watcher(watcher);
    fcitx::gtk::ClassicUIConfig config;
    Window window(&config, client);
    auto *empty = g_ptr_array_new();
    auto *candidates = g_ptr_array_new();
    std::vector<std::string> texts = {
        "你好", "你好吗", "你好世界", "你好朋友", "拟好", "你号", "泥好", "腻好", "你好呀", "你好啊"};
    while (texts.size() < 40) texts.push_back("候选文本");
    std::vector<FcitxGCandidateItem> items(texts.size());
    for (size_t i = 0; i < texts.size(); ++i) {
        items[i].label = const_cast<char *>("");
        items[i].candidate = texts[i].data();
        g_ptr_array_add(candidates, &items[i]);
    }
    auto signal = [&](int hint, GPtrArray *page, int cursor) {
        g_signal_emit_by_name(client, "update-client-side-ui", empty, -1,
                             empty, empty, page, cursor, hint, false, true);
    };
    g_signal_emit_by_name(client, "current-im", "TiPE", "pinyin", "zh_CN");
    require(!window.isTipe(), "gate must use unique name, not display name");
    signal(2, candidates, 0);
    g_signal_emit_by_name(client, "current-im", "Any label", "tipe", "zh_CN");
    require(window.isTipe(), "TiPE current-im signal did not select painter");
    signal(2, candidates, 0);
    require(window.frame().layout.rows == 1, "compact strip must have one row");
    signal(1, candidates, 0);
    require(window.frame().expanded && window.frame().layout.rows > 1,
            "vertical layout hint did not expand the actual GTK painter");
    const auto cell = window.frame().layout.cells.at(2);
    const int x = cell.rect.x + cell.rect.width / 2;
    const int y = cell.rect.y + cell.rect.height / 2;
    require(window.hover(x, y) && window.selected() == 2,
            "pointer hover did not use the rendered cell geometry");
    window.click(x, y);
    require(selectedCandidate == 2, "click did not select the page-local candidate");
    window.click(-1, -1);
    require(selectedCandidate == 2, "outside click selected a candidate");
    signal(1, candidates, 999);
    require(window.selected() == 39, "stale candidate cursor was not clamped");
    signal(1, candidates, 1);
    require(window.selected() == 1, "stale hover overrode the next keyboard selection");
    const auto [w, h] = window.sizeHint();
    auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    auto *cr = cairo_create(surface);
    window.paint(cr, w, h);
    require(cairo_status(cr) == CAIRO_STATUS_SUCCESS, "GTK painter failed to render");
    cairo_surface_flush(surface);
    auto *pixels = reinterpret_cast<unsigned *>(cairo_image_surface_get_data(surface));
    const int stride = cairo_image_surface_get_stride(surface) / 4;
    const auto selected = window.frame().layout.cells.at(1).rect;
    const unsigned pixel = pixels[(selected.y + 2) * stride + selected.x + selected.width / 2];
    require((pixel & 0x00ffffffu) == 0x000a84ffu,
            "GTK paint used the theme fallback instead of shared TiPE blue selection");
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    signal(2, empty, -1);
    require(!window.visible(), "empty update retained a candidate window");
    g_signal_emit_by_name(client, "current-im", "Pinyin", "pinyin", "zh_CN");
    require(!window.isTipe(), "switching IM retained the TiPE painter");
    g_ptr_array_unref(candidates);
    g_ptr_array_unref(empty);
    g_object_unref(client);
    g_object_unref(watcher);
    std::cout << "GTK candidate signal/render/hover/click/gate smoke passed\n";
}
