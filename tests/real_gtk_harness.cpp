#include <gtk/gtk.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <string>

namespace {

struct Harness {
  GtkApplication *app = nullptr;
  GtkWidget *window = nullptr;
  GtkWidget *entry = nullptr;
  GtkIMContext *im = nullptr;
  guint timeout_id = 0;
  std::string last_rect;
  // Set only for the opt-in password/sensitive regression.  In this mode the
  // harness configures the real GtkEntry password input purpose and redacts
  // all text payloads from JSONL, while retaining event/length evidence.
  bool sensitive = false;
  bool no_surrounding = false;
};

bool env_enabled(const char *name) {
  const char *value = std::getenv(name);
  return value && (std::strcmp(value, "1") == 0 ||
                   std::strcmp(value, "true") == 0 ||
                   std::strcmp(value, "yes") == 0);
}

std::string json(const char *text);

std::string redacted_text(const char *field, const char *text, bool sensitive) {
  const std::string name = field ? field : "text";
  if (!sensitive)
    return "\"" + name + "\":" + json(text ? text : "");
  const size_t bytes = text ? std::strlen(text) : 0;
  return "\"" + name + "_length\":" + std::to_string(bytes) +
         ",\"redacted\":true";
}

std::string json(const char *text) {
  std::string out = "\"";
  if (!text) return out + "\"";
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
       *p; ++p) {
    switch (*p) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (*p < 0x20) {
        char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", *p);
        out += buf;
      } else out.push_back(static_cast<char>(*p));
    }
  }
  return out + "\"";
}

void emit(const std::string &line) {
  std::fputs(line.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

void emit_rect(Harness *h) {
  GtkEditable *delegate = gtk_editable_get_delegate(GTK_EDITABLE(h->entry));
  if (!delegate || !GTK_IS_TEXT(delegate)) return;
  graphene_rect_t strong{};
  gtk_text_compute_cursor_extents(GTK_TEXT(delegate),
                                  gtk_editable_get_position(delegate),
                                  &strong, nullptr);
  graphene_point_t mapped{};
  if (!gtk_widget_compute_point(GTK_WIDGET(delegate), h->window,
                                 &strong.origin, &mapped)) return;
  const int x = static_cast<int>(std::lround(mapped.x));
  const int y = static_cast<int>(std::lround(mapped.y));
  const int width = std::max(1, static_cast<int>(std::lround(strong.size.width)));
  const int height = std::max(1, static_cast<int>(std::lround(strong.size.height)));
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%d,%d,%d,%d", x, y, width, height);
  if (h->last_rect == buf) return;
  h->last_rect = buf;
  char line[256];
  std::snprintf(line, sizeof(line),
                "{\"event\":\"text_rectangle\",\"coordinate_space\":\"window\",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}",
                x, y, width, height);
  emit(line);
}

void on_preedit_changed(GtkIMContext *context, Harness *h) {
  char *text = nullptr;
  int cursor = 0;
  gtk_im_context_get_preedit_string(context, &text, nullptr, &cursor);
  const std::string value = text ? text : "";
  char line[1024];
  const std::string payload = redacted_text("text", value.c_str(), h->sensitive);
  std::snprintf(line, sizeof(line),
                "{\"event\":\"preedit\",%s,\"cursor\":%d,\"sensitive\":%s}",
                payload.c_str(), cursor, h->sensitive ? "true" : "false");
  emit(line);
  g_free(text);
  emit_rect(h);
}

void on_commit(GtkIMContext *, const char *text, Harness *h) {
  emit("{\"event\":\"commit\"," +
       redacted_text("text", text, h->sensitive) +
       ",\"sensitive\":" + (h->sensitive ? "true" : "false") + "}");
  emit_rect(h);
}

void on_text_notify(GObject *, GParamSpec *, Harness *h) {
  const char *text = gtk_editable_get_text(GTK_EDITABLE(h->entry));
  emit("{\"event\":\"text\"," +
       redacted_text("value", text, h->sensitive) +
       ",\"sensitive\":" + (h->sensitive ? "true" : "false") + "}");
  emit_rect(h);
}

gboolean on_retrieve_surrounding(GtkIMContext *, Harness *h) {
  GtkEditable *editable = GTK_EDITABLE(h->entry);
  if (h->no_surrounding) {
    emit("{\"event\":\"retrieve_surrounding\",\"available\":false,\"sensitive\":" +
         std::string(h->sensitive ? "true" : "false") + "}");
    return TRUE;
  }
  const char *text = gtk_editable_get_text(editable);
  const int cursor = gtk_editable_get_position(editable);
  emit("{\"event\":\"retrieve_surrounding\"," +
       redacted_text("text", text, h->sensitive) +
       ",\"cursor\":" + std::to_string(cursor) +
       ",\"sensitive\":" + (h->sensitive ? "true" : "false") + "}");
  // Observe only. GtkText's own handler owns the surrounding-text reply.
  return FALSE;
}

gboolean on_delete_surrounding(GtkIMContext *, int offset, int n_chars,
                               Harness *h) {
  (void)h;
  emit("{\"event\":\"delete_surrounding\",\"offset\":" +
       std::to_string(offset) + ",\"n_chars\":" +
       std::to_string(n_chars) + "}");
  // Observe only. GtkText's own handler owns the deletion transaction.
  return FALSE;
}

void on_realize(GtkWidget *, Harness *h) {
  emit_rect(h);
  emit(std::string("{\"event\":\"ready\",\"toolkit\":\"gtk4\",\"im_context\":") +
       (h->im ? "true" : "false") +
       ",\"sensitive\":" + (h->sensitive ? "true" : "false") +
       ",\"surrounding_available\":" +
       (h->no_surrounding ? "false" : "true") +
       ",\"input_purpose\":\"" +
       (h->sensitive ? "password" : "free-form") +
       "\",\"echo\":\"" + (h->sensitive ? "hidden" : "visible") + "\"}");
}

gboolean on_timeout(Harness *h) {
  emit("{\"event\":\"timeout\"}");
  g_application_quit(G_APPLICATION(h->app));
  return G_SOURCE_REMOVE;
}

void activate(GtkApplication *app, Harness *h) {
  h->app = app;
  h->sensitive = env_enabled("TIPE_HARNESS_PASSWORD") ||
                 env_enabled("TIPE_HARNESS_SENSITIVE") ||
                 env_enabled("TIPE_HARNESS_PASSWORD_ECHO");
  h->no_surrounding = env_enabled("TIPE_HARNESS_NO_SURROUNDING");
  h->window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(h->window), "TiPE GTK real-app harness");
  gtk_window_set_default_size(GTK_WINDOW(h->window), 640, 100);
  h->entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(h->entry), "TiPE GTK harness");
  if (h->sensitive) {
    gtk_entry_set_visibility(GTK_ENTRY(h->entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(h->entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_input_hints(GTK_ENTRY(h->entry), GTK_INPUT_HINT_PRIVATE);
  }
  // A tiled window can be much taller than an entry. Keep the entry at its
  // natural height so GTK reports the actual text-line rectangle to the IM.
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 24);
  gtk_widget_set_margin_end(box, 24);
  gtk_widget_set_margin_top(box, 24);
  gtk_widget_set_margin_bottom(box, 24);
  if (env_enabled("TIPE_HARNESS_EDGE")) {
    auto *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(box), spacer);
    gtk_widget_set_halign(h->entry, GTK_ALIGN_END);
    gtk_widget_set_size_request(h->entry, 180, -1);
  }
  gtk_box_append(GTK_BOX(box), h->entry);
  if (env_enabled("TIPE_HARNESS_STRETCHED"))
    gtk_widget_set_vexpand(h->entry, TRUE);
  gtk_window_set_child(GTK_WINDOW(h->window), box);

  // Observe the GtkEntry's own key controller and IM context. Creating a
  // second context would exercise a synthetic path and hide integration bugs;
  // this lookup keeps the normal GTK event path intact.
  GtkEditable *delegate = gtk_editable_get_delegate(GTK_EDITABLE(h->entry));
  GtkWidget *controller_widget = delegate ? GTK_WIDGET(delegate) : h->entry;
  GListModel *controllers = gtk_widget_observe_controllers(controller_widget);
  for (guint i = 0; i < g_list_model_get_n_items(controllers); ++i) {
    GObject *object = G_OBJECT(g_list_model_get_item(controllers, i));
    if (GTK_IS_EVENT_CONTROLLER_KEY(object)) {
      h->im = gtk_event_controller_key_get_im_context(
          GTK_EVENT_CONTROLLER_KEY(object));
      if (h->im) {
        g_signal_connect(h->im, "preedit-changed",
                         G_CALLBACK(on_preedit_changed), h);
        g_signal_connect(h->im, "commit", G_CALLBACK(on_commit), h);
        g_signal_connect(h->im, "retrieve-surrounding",
                         G_CALLBACK(on_retrieve_surrounding), h);
        g_signal_connect(h->im, "delete-surrounding",
                         G_CALLBACK(on_delete_surrounding), h);
      }
      g_object_unref(object);
      break;
    }
    g_object_unref(object);
  }
  g_object_unref(controllers);
  g_signal_connect(h->entry, "realize", G_CALLBACK(on_realize), h);
  g_signal_connect(h->entry, "notify::text", G_CALLBACK(on_text_notify), h);
  gtk_widget_grab_focus(h->entry);
  gtk_window_present(GTK_WINDOW(h->window));
  const char *timeout = std::getenv("TIPE_HARNESS_TIMEOUT_MS");
  const guint ms = timeout ? std::max(100u, static_cast<guint>(std::strtoul(timeout, nullptr, 10))) : 10000u;
  h->timeout_id = g_timeout_add(ms, G_SOURCE_FUNC(on_timeout), h);
}

} // namespace

int main(int argc, char **argv) {
  Harness h;
  auto *app = gtk_application_new("org.tipe.RealGtkHarness", G_APPLICATION_NON_UNIQUE);
  g_signal_connect(app, "activate", G_CALLBACK(activate), &h);
  const int result = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return result;
}
