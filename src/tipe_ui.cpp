#include "tipe/candidate_painter.hpp"

#include <cairo/cairo.h>
#ifdef TIPE_HAS_XCB_POPUP
#include <cairo/cairo-xcb.h>
#include <xcb/xcb.h>
#endif
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/event.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx/userinterface.h>
#include <fcitx/candidatelist.h>
#include <fcitx/text.h>
#include <fcitx-utils/utf8.h>
#ifdef TIPE_HAS_FCITX_WAYLAND_PUBLIC
#include <fcitx-module/wayland/wayland_public.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::optional<int> env_int(const char *name, int fallback) {
  const char *value = std::getenv(name);
  if (!value || !*value) return fallback;
  char *end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0') return fallback;
  return static_cast<int>(std::clamp(parsed, 1L, 16384L));
}

bool env_enabled(const char *name) {
  const char *value = std::getenv(name);
  return value && (!std::string_view(value).empty()) &&
         (std::string_view(value) == "1" || std::string_view(value) == "true" ||
          std::string_view(value) == "yes");
}

std::string json_escape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20) {
          std::ostringstream escaped;
          escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                  << static_cast<int>(ch);
          out += escaped.str();
        } else {
          out.push_back(static_cast<char>(ch));
        }
    }
  }
  return out;
}

std::string context_key(const fcitx::InputContext *ic) {
  // UUID is public API and remains stable over the lifetime of an input
  // context. Hex encoding avoids leaking raw bytes into a pathname.
  const auto &uuid = ic->uuid();
  std::ostringstream out;
  for (const auto byte : uuid)
    out << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<unsigned int>(byte);
  return out.str();
}

struct PanelState {
  std::vector<std::string> candidates;
  std::string preedit;
  // PasswordOrSensitive contexts must never be rendered or serialized by the
  // optional diagnostic UI.  The engine already suppresses learning events;
  // keep the same boundary here so a configured snapshot directory or event
  // log cannot become a side channel for password text.
  bool sensitive{};
  bool expanded{};
  int selected{-1};
  size_t begin{};
  size_t total{};
};

#ifdef TIPE_HAS_FCITX_WAYLAND_PUBLIC
// Fcitx exposes the wl_display through its public Wayland module, but does
// not expose an InputPanel/Popup factory.  We still inspect the registry to
// distinguish a compositor that advertises an input-panel or input-method
// protocol from a connection that merely happens to exist.  This is a
// read-only probe: no globals are bound and no protocol object is created.
struct WaylandGlobals {
  bool input_method_v1{};
  bool input_panel_v1{};
  bool input_method_v2{};
  bool input_method_manager_v2{};
  // The manager is the only protocol global. A popup surface is a child
  // object created by zwp_input_method_v2, so it normally never appears in
  // the registry. Keep these separate for an auditable capability report.
  bool popup_manager_v2{};
  bool popup_surface_v2{};
  // Registry versions are useful when comparing a compositor trace with
  // the protocol revision Fcitx selected.  A version is only evidence that
  // the global was advertised; it does not grant a UI addon access to the
  // private wayland-im wrapper or to an input-popup object.
  uint32_t input_method_v1_version{};
  uint32_t input_panel_v1_version{};
  uint32_t input_method_v2_version{};
  uint32_t input_method_manager_v2_version{};
  uint32_t popup_manager_v2_version{};
  uint32_t popup_surface_v2_version{};
};

void wayland_registry_global(void *data, wl_registry *, uint32_t,
                             const char *interface, uint32_t version) {
  if (!data || !interface) return;
  auto *globals = static_cast<WaylandGlobals *>(data);
  const std::string_view name(interface);
  if (name == "zwp_input_method_v1") {
    globals->input_method_v1 = true;
    globals->input_method_v1_version = std::max(globals->input_method_v1_version, version);
  } else if (name == "zwp_input_panel_v1") {
    globals->input_panel_v1 = true;
    globals->input_panel_v1_version = std::max(globals->input_panel_v1_version, version);
  } else if (name == "zwp_input_method_v2" ||
             name == "xx_input_method_v1" || name == "xx_input_method_v2") {
    // Fcitx's current wayland-im v2 protocol has used both the zwp and xx
    // interface names in generated protocol revisions.
    globals->input_method_v2 = true;
    globals->input_method_v2_version = std::max(globals->input_method_v2_version, version);
  } else if (name == "zwp_input_method_manager_v2" ||
             name == "xx_input_method_manager_v2") {
    globals->input_method_manager_v2 = true;
    globals->input_method_manager_v2_version =
        std::max(globals->input_method_manager_v2_version, version);
  } else if (name == "zwp_input_popup_manager_v2" ||
             name == "xx_input_popup_manager_v2") {
    globals->popup_manager_v2 = true;
    globals->popup_manager_v2_version = std::max(globals->popup_manager_v2_version, version);
  } else if (name == "zwp_input_popup_surface_v2" ||
             name == "xx_input_popup_surface_v2") {
    globals->popup_surface_v2 = true;
    globals->popup_surface_v2_version = std::max(globals->popup_surface_v2_version, version);
  }
}

void wayland_registry_global_remove(void *, wl_registry *, uint32_t) {}

bool probe_wayland_globals(wl_display *display, WaylandGlobals &globals) {
  if (!display) return false;
  wl_registry *registry = wl_display_get_registry(display);
  if (!registry) return false;
  static const wl_registry_listener listener = {
      wayland_registry_global, wayland_registry_global_remove};
  if (wl_registry_add_listener(registry, &listener, &globals) < 0) {
    wl_registry_destroy(registry);
    return false;
  }
  // Fcitx owns this connection and dispatches it from its event loop.  A
  // single roundtrip here is safe during the connection-created callback and
  // makes the capability result complete before the first frame is logged.
  const int result = wl_display_roundtrip(display);
  wl_registry_destroy(registry);
  return result >= 0;
}
#endif

PanelState read_panel(const fcitx::InputContext *ic) {
  PanelState state;
  if (!ic) return state;
  state.sensitive = ic->capabilityFlags().testAny(
      fcitx::CapabilityFlag::PasswordOrSensitive);
  const auto &panel = ic->inputPanel();
  state.preedit = panel.preedit().toString();
  const auto list = panel.candidateList();
  if (!list) return state;
  state.expanded = list->layoutHint() == fcitx::CandidateLayoutHint::Vertical;
  state.selected = list->cursorIndex();
  const int local_selected = state.selected;

  // CommonCandidateList exposes both pageable and bulk views. Prefer the bulk
  // view so the painter can render the complete page and preserve a global
  // cursor; fall back to the visible list for custom implementations.
  if (const auto *bulk = list->toBulk()) {
    const int total = bulk->totalSize();
    if (total > 0) {
      state.total = static_cast<size_t>(total);
      state.candidates.reserve(state.total);
      for (int i = 0; i < total; ++i) {
        try {
          state.candidates.push_back(bulk->candidateFromAll(i).text().toString());
        } catch (...) {
          state.candidates.clear();
          state.total = 0;
          break;
        }
      }
    }
  }
  if (state.candidates.empty()) {
    state.candidates.reserve(static_cast<size_t>(std::max(0, list->size())));
    for (int i = 0; i < list->size(); ++i)
      state.candidates.push_back(list->candidate(i).text().toString());
    state.total = state.candidates.size();
  }
  if (const auto *cursor = list->toBulkCursor()) {
    const int global = cursor->globalCursorIndex();
    if (global >= 0) state.selected = global;
  }
  // `begin` is the first item in the visible page.  The painter's grid uses
  // global candidate indices, so the page offset must be applied even when a
  // BulkCursor already reports a global selection.  The previous code only
  // set begin while the cursor looked page-local; on page 2 this rendered
  // candidates 0..9 with a selected index of 10, leaving no cell highlighted
  // and making mouse hit-tests select from the wrong visible page.
  const auto visible = static_cast<size_t>(std::max(1, list->size()));
  if (state.total > visible) {
    if (const auto *pageable = list->toPageable(); pageable && pageable->currentPage() >= 0) {
      // CommonCandidateList::size() shrinks on the final page. Its pageSize
      // stays fixed, so use that public API instead of multiplying the final
      // page number by its remaining item count. For other bulk lists, the
      // difference between global and visible cursors gives the same offset.
      if (const auto *common = dynamic_cast<const fcitx::CommonCandidateList *>(list.get()))
        state.begin = static_cast<size_t>(pageable->currentPage()) *
                      static_cast<size_t>(std::max(1, common->pageSize()));
      else if (const auto *cursor = list->toBulkCursor(); cursor &&
               cursor->globalCursorIndex() >= local_selected && local_selected >= 0)
        state.begin = static_cast<size_t>(cursor->globalCursorIndex() - local_selected);
      else
        state.begin = static_cast<size_t>(pageable->currentPage()) * visible;
      // Older/custom candidate lists expose only a page-local cursor.  A
      // BulkCursor has already supplied a global index and must not be offset
      // a second time.
      if (state.selected >= 0 && state.selected < static_cast<int>(visible) &&
          (!list->toBulkCursor() || list->toBulkCursor()->globalCursorIndex() < 0))
        state.selected += static_cast<int>(state.begin);
    }
  }
  if (state.selected < 0) state.selected = -1;
  return state;
}

class UiAddon final : public fcitx::UserInterface {
 public:
  explicit UiAddon(fcitx::AddonManager *manager) : manager_(manager) {
    // The addon deliberately does not claim the active UI slot unless an
    // explicit output directory is configured. This leaves Classic UI in
    // charge for normal users and makes the fallback behavior deterministic.
    const char *dir = std::getenv("TIPE_UI_SNAPSHOT_DIR");
    if (dir && *dir) snapshot_dir_ = fs::path(dir);
    const char *stream = std::getenv("TIPE_UI_EVENT_LOG");
    if (stream && *stream) event_log_ = fs::path(stream);
    width_ = *env_int("TIPE_UI_WIDTH", 640);
    height_ = *env_int("TIPE_UI_HEIGHT", 320);
    font_pixels_ = *env_int("TIPE_UI_FONT_PIXELS", 16);
    wayland_display_env_ = [] {
      const char *value = std::getenv("WAYLAND_DISPLAY");
      return value && *value;
    }();
#ifdef TIPE_HAS_FCITX_WAYLAND_PUBLIC
    // The public Wayland module API deliberately exposes only connection
    // lifecycle callbacks.  Registering these callbacks lets diagnostics
    // distinguish a real Wayland connection from a headless/XWayland run;
    // it does not pretend that a popup surface can be created by a normal UI
    // addon.  Fcitx's private wayland-im object owns that capability.
    if (manager_) {
      // waylandim owns the active zwp_input_method_v2 object. Recording
      // whether that addon is loaded makes the limitation auditable: the
      // public module exposes connection lifecycle, while the popup factory
      // remains on the private waylandim C++ wrapper.
      wayland_im_module_loaded_ =
          manager_->addon("waylandim", false) != nullptr;
      if (auto *wayland = manager_->addon("wayland", false)) {
        wayland_created_watch_ =
            wayland->call<fcitx::IWaylandModule::addConnectionCreatedCallback>(
                [this](const std::string &name, wl_display *display,
                       fcitx::FocusGroup *) {
                  // The frontend can be loaded after the UI addon. Refresh
                  // this bit when a connection is announced rather than
                  // treating the constructor-time lookup as authoritative.
                  wayland_im_module_loaded_ =
                      manager_ &&
                      manager_->addon("waylandim", false) != nullptr;
                  wayland_module_loaded_ = true;
                  wayland_connection_active_ = display != nullptr;
                  wayland_connection_name_ = name;
                  wayland_probe_state_ = "error";
                  wayland_globals_ = {};
                  if (display) {
                    wayland_probe_state_ =
                        probe_wayland_globals(display, wayland_globals_)
                            ? "complete"
                            : "error";
                  }
                });
        wayland_closed_watch_ =
            wayland->call<fcitx::IWaylandModule::addConnectionClosedCallback>(
                [this](const std::string &name, wl_display *) {
                  if (name == wayland_connection_name_ ||
                      wayland_connection_name_.empty()) {
                    wayland_connection_active_ = false;
                    wayland_connection_name_ = name;
                    wayland_probe_state_ = "not-run";
                    wayland_globals_ = {};
                  }
                });
        wayland_module_loaded_ = true;
      }
    }
#endif
#ifdef TIPE_HAS_XCB_POPUP
    x11_enabled_ = env_enabled("TIPE_UI_X11");
    if (x11_enabled_) open_x11();
    enabled_ = snapshot_dir_.has_value() || event_log_.has_value() ||
               (x11_enabled_ && xcb_connection_ && xcb_window_);
#else
    enabled_ = snapshot_dir_.has_value() || event_log_.has_value();
#endif
    if (manager_ && manager_->instance()) {
      destroy_watch_ = manager_->instance()->watchEvent(
          fcitx::EventType::InputContextDestroyed,
          fcitx::EventWatcherPhase::Default, [this](fcitx::Event &event) {
            auto *destroyed =
                dynamic_cast<fcitx::InputContextDestroyedEvent *>(&event);
            if (destroyed && destroyed->inputContext()) {
#ifdef TIPE_HAS_XCB_POPUP
              if (active_ic_ == destroyed->inputContext()) {
                active_ic_ = nullptr;
                if (xcb_connection_ && xcb_window_)
                  xcb_unmap_window(xcb_connection_, xcb_window_);
              }
#endif
              frames_.erase(destroyed->inputContext());
            }
          });
    }
  }

  ~UiAddon() override {
#ifdef TIPE_HAS_FCITX_WAYLAND_PUBLIC
    wayland_closed_watch_.reset();
    wayland_created_watch_.reset();
#endif
#ifdef TIPE_HAS_XCB_POPUP
    xcb_event_source_.reset();
    if (xcb_connection_) {
      if (xcb_window_) xcb_destroy_window(xcb_connection_, xcb_window_);
      xcb_flush(xcb_connection_);
      xcb_disconnect(xcb_connection_);
    }
#endif
  }

  void update(fcitx::UserInterfaceComponent component,
              fcitx::InputContext *ic) override {
    if (!enabled_ || !ic || component != fcitx::UserInterfaceComponent::InputPanel)
      return;
    auto state = read_panel(ic);
    const bool sensitive = state.sensitive;
    // Do not let an optional popup/snapshot retain password preedit or
    // candidate text.  Fcitx normally suppresses this panel itself, but the
    // addon can be enabled explicitly for diagnostics, so enforce the
    // privacy boundary at this last rendering point as well.
    if (sensitive) {
      state.candidates.clear();
      state.preedit.clear();
      state.selected = -1;
      state.begin = state.total = 0;
      state.expanded = false;
    }
    const double scale = std::clamp(ic->scaleFactor(), 0.5, 8.0);
    auto frame = tipe::painter::measure(state.candidates, state.preedit,
                                        state.expanded, width_, height_,
                                        state.selected, state.begin,
                                        font_pixels_, scale);
    frames_[ic] = std::move(frame);
#ifdef TIPE_HAS_XCB_POPUP
    // An X11 override-redirect surface is only valid for X11 clients. Native
    // Wayland input contexts commonly report a zero/relative cursorRect; using
    // it for an X11 popup detaches the candidate window at screen origin.
    const bool x11_client = ic->display().rfind("x11::", 0) == 0;
    if (x11_enabled_ && xcb_connection_ && x11_client) {
      if (sensitive) {
        popup_visible_ = false;
        xcb_unmap_window(xcb_connection_, xcb_window_);
        xcb_flush(xcb_connection_);
      } else {
        show_x11(*ic, frames_[ic]);
      }
    }
#endif
    if (snapshot_dir_) write_snapshot(*ic, frames_[ic], sensitive);
    if (event_log_) write_event(*ic, state, frames_[ic]);
  }

  bool available() override { return enabled_ && !suspended_; }

  void suspend() override { suspended_ = true; }
  void resume() override { suspended_ = false; }

 private:
  void write_snapshot(const fcitx::InputContext &ic,
                      const tipe::painter::Frame &frame, bool sensitive) {
    std::error_code ec;
    fs::create_directories(*snapshot_dir_, ec);
    if (ec) return;
    const auto key = context_key(&ic);
    const fs::path target = *snapshot_dir_ / ("tipe-" + key + ".png");
    if (sensitive) {
      // Remove a stale normal-context frame when an address is reused by a
      // password context.  Never replace it with a redacted image: a caller
      // inspecting the directory must not mistake that artifact for current
      // UI state.
      fs::remove(target, ec);
      return;
    }
    const auto sequence = ++sequence_;
    const fs::path temporary = target.string() + ".tmp-" + std::to_string(sequence);
    const int scale = std::max(1, static_cast<int>(std::ceil(frame.scale_factor)));
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, std::max(1, frame.width * scale),
        std::max(1, frame.height * scale));
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(surface);
      return;
    }
    cairo_t *cr = cairo_create(surface);
    tipe::painter::paint(cr, frame);
    cairo_destroy(cr);
    cairo_surface_write_to_png(surface, temporary.c_str());
    cairo_surface_destroy(surface);
    fs::rename(temporary, target, ec);
    if (ec) fs::remove(temporary, ec);
  }

  void write_event(const fcitx::InputContext &ic, const PanelState &state,
                   const tipe::painter::Frame &frame) {
    std::error_code ec;
    if (event_log_->has_parent_path())
      fs::create_directories(event_log_->parent_path(), ec);
    std::ofstream out(*event_log_, std::ios::app);
    if (!out) return;
    // InputContext::cursorRect() is the only public anchor supplied by the
    // frontend. Keep the raw rectangle in the diagnostic frame so a desktop
    // run can prove whether the popup followed the client's text rectangle;
    // this is deliberately not interpreted as a compositor/global position.
    const auto cursor_rect = ic.cursorRect();
    out << "{\"context\":\"" << context_key(&ic)
        << "\",\"program\":\"" << json_escape(ic.program())
        << "\",\"scale\":" << frame.scale_factor
        << ",\"width\":" << frame.width
        << ",\"height\":" << frame.height
        << ",\"cursor_rect_x\":" << cursor_rect.left()
        << ",\"cursor_rect_y\":" << cursor_rect.top()
        << ",\"cursor_rect_width\":" << cursor_rect.width()
        << ",\"cursor_rect_height\":" << cursor_rect.height()
        << ",\"sensitive\":" << (state.sensitive ? "true" : "false")
        << ",\"preedit\":\""
        << (state.sensitive ? "<redacted>" : json_escape(state.preedit))
        << "\",\"selected\":" << state.selected
        << ",\"begin\":" << state.begin
        << ",\"total\":" << state.total;
#ifdef TIPE_HAS_FCITX_WAYLAND_PUBLIC
    out << ",\"wayland_module_loaded\":"
        << (wayland_module_loaded_ ? "true" : "false")
        << ",\"wayland_im_module_loaded\":"
        << (wayland_im_module_loaded_ ? "true" : "false")
        << ",\"wayland_display_env\":"
        << (wayland_display_env_ ? "true" : "false")
        << ",\"wayland_connection_active\":"
        << (wayland_connection_active_ ? "true" : "false")
        << ",\"wayland_connection\":\""
        << json_escape(wayland_connection_name_)
        << "\",\"wayland_global_probe\":\""
        << wayland_probe_state_
        << "\",\"wayland_input_method_v1\":"
        << (wayland_globals_.input_method_v1 ? "true" : "false")
        << ",\"wayland_input_panel_v1\":"
        << (wayland_globals_.input_panel_v1 ? "true" : "false")
        << ",\"wayland_input_method_v2\":"
        << (wayland_globals_.input_method_v2 ? "true" : "false")
        << ",\"wayland_input_method_manager_v2\":"
        << (wayland_globals_.input_method_manager_v2 ? "true" : "false")
        << ",\"wayland_popup_manager_v2\":"
        << (wayland_globals_.popup_manager_v2 ? "true" : "false")
        << ",\"wayland_popup_surface_v2\":"
        << (wayland_globals_.popup_surface_v2 ? "true" : "false")
        << ",\"wayland_input_method_v1_version\":"
        << wayland_globals_.input_method_v1_version
        << ",\"wayland_input_panel_v1_version\":"
        << wayland_globals_.input_panel_v1_version
        << ",\"wayland_input_method_v2_version\":"
        << wayland_globals_.input_method_v2_version
        << ",\"wayland_input_method_manager_v2_version\":"
        << wayland_globals_.input_method_manager_v2_version
        << ",\"wayland_popup_manager_v2_version\":"
        << wayland_globals_.popup_manager_v2_version
        << ",\"wayland_popup_surface_v2_version\":"
        << wayland_globals_.popup_surface_v2_version
        // This is intentionally false even if a protocol manager is
        // advertised: wayland_public.h has no popup-surface factory.
        << ",\"wayland_public_popup_factory\":false"
        << ",\"wayland_popup_supported\":false"
        << ",\"wayland_popup_reason\":\"public-api-no-popup-factory\""
        << ",\"wayland_fallback\":\"standard-input-panel\""
        // cursorRect is useful for diagnostics and for the X11 fallback, but
        // it is not a compositor-global anchor on a native Wayland client.
        // Keep the distinction machine-readable so a log cannot be mistaken
        // for evidence that a popup surface was positioned.
        << ",\"wayland_anchor_source\":\"cursorRect-diagnostic-only\""
        << ",\"wayland_surface_lifecycle\":\"standard-input-panel\"";
#else
    out << ",\"wayland_module_loaded\":false"
        << ",\"wayland_im_module_loaded\":false"
        << ",\"wayland_display_env\":"
        << (wayland_display_env_ ? "true" : "false")
        << ",\"wayland_connection_active\":false"
        << ",\"wayland_connection\":\"\""
        << ",\"wayland_global_probe\":\"unavailable\""
        << ",\"wayland_input_method_v1\":false"
        << ",\"wayland_input_panel_v1\":false"
        << ",\"wayland_input_method_v2\":false"
        << ",\"wayland_input_method_manager_v2\":false"
        << ",\"wayland_popup_manager_v2\":false"
        << ",\"wayland_popup_surface_v2\":false"
        << ",\"wayland_input_method_v1_version\":0"
        << ",\"wayland_input_panel_v1_version\":0"
        << ",\"wayland_input_method_v2_version\":0"
        << ",\"wayland_input_method_manager_v2_version\":0"
        << ",\"wayland_popup_manager_v2_version\":0"
        << ",\"wayland_popup_surface_v2_version\":0"
        << ",\"wayland_public_popup_factory\":false"
        << ",\"wayland_popup_supported\":false"
        << ",\"wayland_popup_reason\":\"public-api-no-popup-factory\""
        << ",\"wayland_fallback\":\"standard-input-panel\""
        << ",\"wayland_anchor_source\":\"cursorRect-diagnostic-only\""
        << ",\"wayland_surface_lifecycle\":\"standard-input-panel\"";
#endif
#ifdef TIPE_HAS_XCB_POPUP
    out << ",\"x11_visible\":" << (popup_visible_ ? "true" : "false")
        << ",\"x11_x\":" << popup_x_
        << ",\"x11_y\":" << popup_y_
        << ",\"x11_width\":" << popup_width_
        << ",\"x11_height\":" << popup_height_;
#endif
    out << ",\"candidates\":[";
    for (size_t i = 0; i < state.candidates.size(); ++i) {
      if (i) out << ',';
      out << '"' << json_escape(state.candidates[i]) << '"';
    }
    out << "]}\n";
  }

#ifdef TIPE_HAS_XCB_POPUP
  void open_x11() {
    int screen = 0;
    xcb_connection_ = xcb_connect(nullptr, &screen);
    if (!xcb_connection_ || xcb_connection_has_error(xcb_connection_)) {
      if (xcb_connection_) xcb_disconnect(xcb_connection_);
      xcb_connection_ = nullptr;
      return;
    }
    const auto *setup = xcb_get_setup(xcb_connection_);
    const auto *it = xcb_setup_roots_iterator(setup).data;
    if (!it) {
      xcb_disconnect(xcb_connection_);
      xcb_connection_ = nullptr;
      return;
    }
    screen_root_ = it->root;
    screen_width_ = it->width_in_pixels;
    screen_height_ = it->height_in_pixels;
    xcb_window_ = xcb_generate_id(xcb_connection_);
    // Value order follows the numeric order of the mask bits:
    // override-redirect (bit 9), then event-mask (bit 11).
    const uint32_t values[] = {
        1u, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS};
    xcb_create_window(xcb_connection_, XCB_COPY_FROM_PARENT, xcb_window_,
                      screen_root_, 0, 0, width_, height_, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, it->root_visual,
                      XCB_CW_EVENT_MASK | XCB_CW_OVERRIDE_REDIRECT, values);
    xcb_event_source_ = manager_->instance()->eventLoop().addIOEvent(
        xcb_get_file_descriptor(xcb_connection_), fcitx::IOEventFlag::In,
        [this](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) {
          if (!xcb_connection_ || xcb_connection_has_error(xcb_connection_)) {
            x11_enabled_ = false;
            if (!snapshot_dir_ && !event_log_) enabled_ = false;
            return false;
          }
          while (auto *event = xcb_poll_for_event(xcb_connection_)) {
            const uint8_t type = event->response_type & ~0x80;
            if (type == XCB_BUTTON_PRESS) {
              auto *press = reinterpret_cast<xcb_button_press_event_t *>(event);
              if (active_ic_) {
                const auto frame = frames_.find(active_ic_);
                if (frame == frames_.end()) { free(event); continue; }
                const auto hit = tipe::painter::hit_test(frame->second, press->event_x, press->event_y);
                if (hit) select_x11(const_cast<fcitx::InputContext &>(*active_ic_), *hit);
              }
            } else if (type == XCB_EXPOSE && active_ic_) {
              const auto frame = frames_.find(active_ic_);
              if (frame != frames_.end()) paint_x11(frame->second);
            }
            free(event);
          }
          return true;
        });
  }

  void select_x11(fcitx::InputContext &ic, size_t index) {
    const auto list = ic.inputPanel().candidateList();
    if (!list) return;
    if (const auto *bulk = list->toBulk(); bulk && index < static_cast<size_t>(bulk->totalSize())) {
      bulk->candidateFromAll(static_cast<int>(index)).select(&ic);
    } else if (index < static_cast<size_t>(list->size())) {
      list->candidate(static_cast<int>(index)).select(&ic);
    }
  }

  void paint_x11(const tipe::painter::Frame &frame) {
    if (!xcb_connection_ || !xcb_window_) return;
    auto *setup = xcb_get_setup(xcb_connection_);
    auto *screen = xcb_setup_roots_iterator(setup).data;
    xcb_visualtype_t *visual = nullptr;
    for (auto depth = xcb_screen_allowed_depths_iterator(screen);
         depth.rem; xcb_depth_next(&depth)) {
      for (auto visuals = xcb_depth_visuals_iterator(depth.data);
           visuals.rem; xcb_visualtype_next(&visuals)) {
        if (visuals.data->visual_id == screen->root_visual) visual = visuals.data;
      }
    }
    if (!visual) return;
    const int scale = std::max(1, static_cast<int>(std::ceil(frame.scale_factor)));
    auto *surface = cairo_xcb_surface_create(
        xcb_connection_, xcb_window_, visual,
        std::max(1, frame.width * scale), std::max(1, frame.height * scale));
    auto *cr = cairo_create(surface);
    tipe::painter::paint(cr, frame);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
    xcb_flush(xcb_connection_);
  }

  void show_x11(const fcitx::InputContext &ic, const tipe::painter::Frame &frame) {
    if (!xcb_connection_ || !xcb_window_) return;
    const auto rect = ic.cursorRect();
    int x = rect.left();
    int y = rect.bottom() + 2;
    if (frame.candidates.empty() && frame.preedit.empty()) {
      xcb_unmap_window(xcb_connection_, xcb_window_);
      xcb_flush(xcb_connection_);
      popup_visible_ = false;
      active_ic_ = nullptr;
      return;
    }
    const int scale = std::max(1, static_cast<int>(std::ceil(frame.scale_factor)));
    const int w = std::max(1, frame.width * scale);
    const int h = std::max(1, frame.height * scale);
    if (x + w > screen_width_) x = std::max(0, screen_width_ - w);
    if (y + h > screen_height_) y = std::max(0, rect.top() - h - 2);
    const uint32_t values[] = {static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                               static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    xcb_configure_window(xcb_connection_, xcb_window_,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);
    xcb_map_window(xcb_connection_, xcb_window_);
    popup_x_ = x;
    popup_y_ = y;
    popup_width_ = w;
    popup_height_ = h;
    popup_visible_ = true;
    active_ic_ = &ic;
    paint_x11(frame);
  }
#endif

  fcitx::AddonManager *manager_{};
  std::optional<fs::path> snapshot_dir_;
  std::optional<fs::path> event_log_;
  std::unordered_map<const fcitx::InputContext *, tipe::painter::Frame> frames_;
  // Keep the watcher alive after frames_ has been constructed and destroy it
  // before frames_ so a final InputContextDestroyed event cannot race a dead
  // container during addon teardown.
  std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> destroy_watch_;
#ifdef TIPE_HAS_FCITX_WAYLAND_PUBLIC
  std::unique_ptr<fcitx::HandlerTableEntry<fcitx::WaylandConnectionCreated>>
      wayland_created_watch_;
  std::unique_ptr<fcitx::HandlerTableEntry<fcitx::WaylandConnectionClosed>>
      wayland_closed_watch_;
  bool wayland_module_loaded_{};
  bool wayland_im_module_loaded_{};
  bool wayland_display_env_{};
  bool wayland_connection_active_{};
  std::string wayland_connection_name_;
  std::string wayland_probe_state_{"not-run"};
  WaylandGlobals wayland_globals_;
#endif
#ifdef TIPE_HAS_XCB_POPUP
  std::unique_ptr<fcitx::EventSourceIO> xcb_event_source_;
  xcb_connection_t *xcb_connection_{};
  xcb_window_t xcb_window_{};
  xcb_window_t screen_root_{};
  const fcitx::InputContext *active_ic_{};
  int screen_width_{}, screen_height_{};
  int popup_x_{}, popup_y_{}, popup_width_{}, popup_height_{};
  bool popup_visible_{};
#endif
  int width_{640};
  int height_{320};
  int font_pixels_{16};
  std::atomic_uint64_t sequence_{0};
  bool enabled_{};
  bool suspended_{};
#ifdef TIPE_HAS_XCB_POPUP
  bool x11_enabled_{};
#endif
};

class UiAddonFactory final : public fcitx::AddonFactory {
 public:
  fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
    return new UiAddon(manager);
  }
};

} // namespace

FCITX_ADDON_FACTORY_V2_BACKWARDS(tipe_ui, UiAddonFactory)
