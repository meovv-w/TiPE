// Wayland pointer for owned desktop fixtures. Called by desktop_pointer.py,
// which checks fixture focus and allowed target rectangle before every action.
#include <wayland-client.h>
#include "tipe-virtual-pointer-client.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static struct zwlr_virtual_pointer_manager_v1 *manager;
static void global(void *data, struct wl_registry *r, uint32_t name,
                   const char *interface, uint32_t version) {
  (void)data;
  if (!strcmp(interface, "zwlr_virtual_pointer_manager_v1"))
    manager = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface,
                               version < 2 ? version : 2);
}
static void removed(void *data, struct wl_registry *r, uint32_t name) {
  (void)data; (void)r; (void)name;
}
int main(int argc, char **argv) {
  if (argc != 6) return 2;
  unsigned x = strtoul(argv[1], NULL, 10), y = strtoul(argv[2], NULL, 10);
  unsigned width = strtoul(argv[3], NULL, 10), height = strtoul(argv[4], NULL, 10);
  if (!width || !height || x >= width || y >= height) return 2;
  struct wl_display *display = wl_display_connect(NULL);
  if (!display) return 3;
  struct wl_registry *registry = wl_display_get_registry(display);
  const struct wl_registry_listener listener = {global, removed};
  wl_registry_add_listener(registry, &listener, NULL);
  if (wl_display_roundtrip(display) < 0 || !manager) return 4;
  struct zwlr_virtual_pointer_v1 *pointer =
      zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, NULL);
  struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
  uint32_t ms = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
  zwlr_virtual_pointer_v1_motion_absolute(pointer, ms, x, y, width, height);
  zwlr_virtual_pointer_v1_frame(pointer);
  wl_display_roundtrip(display);
  if (!strcmp(argv[5], "click")) {
    // On a headless seat this may be the first pointer device. Allow other
    // clients to bind wl_pointer after wl_seat.capabilities before pressing;
    // a compositor roundtrip alone does not dispatch those clients' queues.
    usleep(50000);
    zwlr_virtual_pointer_v1_button(pointer, ms + 1, 0x110, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    usleep(25000);
    zwlr_virtual_pointer_v1_button(pointer, ms + 26, 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
  } else if (strcmp(argv[5], "move")) return 2;
  zwlr_virtual_pointer_v1_destroy(pointer);
  zwlr_virtual_pointer_manager_v1_destroy(manager);
  wl_registry_destroy(registry);
  wl_display_flush(display);
  wl_display_disconnect(display);
  return 0;
}
